#include "render/cpu/cell_cache.h"

#include "util/hash.h"

#include <stdatomic.h>
#include <stdlib.h>

/* The tile bytes that a slab aims for. A slab holds a power of two tiles. */
#define SLAB_BYTES ((size_t)256 << 10)
/* The largest cell side that the cache takes. */
#define CELL_MAX 4096

/*
 * The budget cost of one slot besides its tile: the key, the index tag, the
 * stamp, the flags, the free list entry, and four index entries. The index
 * has a power of two size of at least 2 and at most 4 entries for each
 * slot.
 */
#define SLOT_META (sizeof(struct cell_key) + 4 + 4 + 1 + 4 + 4 * sizeof(struct index_entry))

/*
 * A class that was used in the last CLASS_KEEP_FRAMES frames is not freed
 * for another class. Two windows at different scales then do not free
 * each other on every frame. A class that needs room takes slabs from the
 * largest other class instead (make_room).
 */
#define CLASS_KEEP_FRAMES 8u

#define PENDING_INDEX (2 * CELL_CACHE_PENDING)
#define HIT_FILTER 512
/*
 * The seen filter has sets of SEEN_WAYS marks. A new mark goes in front
 * and the oldest one drops. With one way, two keys that share an entry
 * and appear in every frame would replace each other forever.
 */
#define SEEN_WAYS 4

_Static_assert((CELL_CACHE_SEEN & (CELL_CACHE_SEEN - 1)) == 0 && CELL_CACHE_SEEN >= SEEN_WAYS,
               "the seen filter is a power of two");
_Static_assert(PENDING_INDEX <= UINT16_MAX, "a pending index entry fits in 16 bits");
_Static_assert(CELL_CACHE_L0_CPS == 128, "L0 covers ASCII");

/* L0 entry numbers: v * 128 + cp in the ASCII table, then L0_ID0 + i in the id table. */
#define L0_ID0 (FONT_VARIANT_COUNT * CELL_CACHE_L0_CPS)
#define L0_ENTRIES (L0_ID0 + CELL_CACHE_L0_IDS)
_Static_assert(L0_ENTRIES <= UINT16_MAX, "an L0 entry number fits in 16 bits");
/* The glyph id bits of an ASCII L0 key: a code point below 128 and the variant. */
#define L0_ASCII_BITS ((uint64_t)(CELL_CACHE_L0_CPS - 1) | (uint64_t)FONT_STYLE_VARIANT_MASK << 32)

/* Slot flags. */
enum {
    SLOT_LIVE = 1u << 0,  /* the index holds the slot */
    SLOT_REF = 1u << 1,   /* used since the clock hand passed it */
    SLOT_SPILL = 1u << 2, /* the tile glyph has ink outside the cell */
};

/* An index entry. slot1 is the slot + 1. 0 is an empty entry. */
struct index_entry {
    uint32_t slot1;
    uint32_t tag;
};

struct cell_class {
    struct cell_class *next;
    struct vector2i cell;
    size_t tile_px;
    int slab_shift;
    uint32_t **slabs;
    int n_slabs;
    int slots;
    /* Slots [fresh, slots) were never used. */
    int fresh;
    struct cell_key *keys;
    uint32_t *tags;
    uint32_t *stamps;
    uint8_t *flags;
    struct index_entry *index;
    uint32_t index_mask;
    /*
     * The free list. Claims take [0, n_free) from the end through `cursor`,
     * so dropping the claimed part moves nothing.
     * The capacity is `slots`.
     */
    uint32_t *free;
    int n_free;
    int hand;
    /* The claims that the last frame asked for. */
    int asked;
    /* A hit log overflowed in the last frame: evict nothing in this one. */
    bool protect;
    /* The clock at the last begin, and at the last publish. */
    uint32_t used;
    uint32_t last;
    /* The phase number while a phase is open, else 0. */
    uint32_t phase;
    size_t bytes;
    /* The L0 tables of the class. The budget does not count them. */
    struct class_l0 *l0;
    /* Every claim writes the cursor. It has its own cache line. */
    char pad0[64];
    _Atomic int cursor;
    char pad1[64];
};

struct cell_cache {
    size_t budget;
    size_t bytes;
    struct cell_class *classes;
    uint32_t clock;
};

/*
 * The L0 tables of a class and their domain. Only the main thread writes
 * them, in cell_cache_begin() and cell_cache_publish(). The workers read
 * `t` during the phase.
 * - `used` lists the entries that were set since the last clear. Bit e of
 *   `listed` tells that entry e is in the list.
 * - `slot[e]` is the slot of the tile in entry e.
 */
struct class_l0 {
    struct cell_l0 t;
    bool valid;
    uint64_t font;
    uint32_t fg, bg;
    int n_used;
    uint16_t used[L0_ENTRIES];
    uint64_t listed[(L0_ENTRIES + 63) / 64];
    uint32_t slot[L0_ENTRIES];
};

struct pending {
    uint32_t slot;
    uint32_t tag;
    uint16_t pos;
};

struct cell_cache_worker {
    /* The phase that this worker takes part in. 0: none. */
    uint32_t phase;
    int asked;
    int n_hits;
    bool hit_overflow;
    int n_pending;
    uint32_t hits[CELL_CACHE_HIT_LOG];
    /* Slot + 1 of recent hits. A repeat hit is logged once. */
    uint32_t hit_filter[HIT_FILTER];
    struct pending pending[CELL_CACHE_PENDING];
    /* Pending entry + 1. 0 is empty. */
    uint16_t pending_index[PENDING_INDEX];
    uint32_t seen[CELL_CACHE_SEEN / SEEN_WAYS][SEEN_WAYS];
};

static const struct cell_l0 empty_l0;

static inline uint64_t key_hash(const struct cell_key *key)
{
    uint64_t w[4];
    memcpy(w, key, sizeof(w));
    uint64_t h = w[0] * UINT64_C(0x9E3779B97F4A7C15) ^ w[1] * UINT64_C(0xC2B2AE3D27D4EB4F) ^
                 w[2] * UINT64_C(0x165667B19E3779F9) ^ w[3] * UINT64_C(0xD6E8FEB86659FD93);
    return hash64_finish(h);
}

static inline uint32_t *tile_at(const struct cell_class *k, uint32_t slot)
{
    uint32_t mask = (1u << k->slab_shift) - 1;
    return k->slabs[slot >> k->slab_shift] + (size_t)(slot & mask) * k->tile_px;
}

static inline size_t slot_cost(const struct cell_class *k)
{
    return k->tile_px * sizeof(uint32_t) + SLOT_META;
}

static int index_find(const struct cell_class *k, uint32_t tag, const struct cell_key *key)
{
    if (!k->index)
        return -1;
    for (uint32_t i = tag & k->index_mask;; i = (i + 1) & k->index_mask) {
        const struct index_entry *e = &k->index[i];
        if (!e->slot1)
            return -1;
        if (e->tag == tag && cell_key_eq(&k->keys[e->slot1 - 1], key))
            return (int)(e->slot1 - 1);
    }
}

static void index_insert(struct cell_class *k, uint32_t slot, uint32_t tag)
{
    uint32_t i = tag & k->index_mask;
    while (k->index[i].slot1)
        i = (i + 1) & k->index_mask;
    k->index[i] = (struct index_entry){ .slot1 = slot + 1, .tag = tag };
}

/* Removes a slot. A backward shift closes the gap, so no tombstone stays. */
static void index_remove(struct cell_class *k, uint32_t slot)
{
    uint32_t mask = k->index_mask;
    uint32_t i = k->tags[slot] & mask;
    while (k->index[i].slot1 != slot + 1)
        i = (i + 1) & mask;
    for (uint32_t j = (i + 1) & mask; k->index[j].slot1; j = (j + 1) & mask) {
        uint32_t home = k->index[j].tag & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            k->index[i] = k->index[j];
            i = j;
        }
    }
    k->index[i] = (struct index_entry){0};
}

/* ---- L0 ---------------------------------------------------------------- */

/* A key of the L0 domain: the default pair of the frame, no pixel flags, half 0. */
static inline bool l0_domain(const struct cell_class *k, const struct cell_key *key)
{
    const struct class_l0 *z = k->l0;
    return z->valid && key->glyph && key->font_version == z->font && key->fg == z->fg &&
           key->bg == z->bg && !key->cursor && !key->flags && !key->half;
}

static inline bool l0_ascii(const struct cell_key *key)
{
    return !(key->glyph & ~L0_ASCII_BITS);
}

/* The ASCII entry of a key of the L0 domain. */
static inline unsigned l0_ascii_entry(const struct cell_key *key)
{
    unsigned cp = (unsigned)(key->glyph & (CELL_CACHE_L0_CPS - 1));
    unsigned v = (unsigned)(key->glyph >> 32) & FONT_STYLE_VARIANT_MASK;
    return v * CELL_CACHE_L0_CPS + cp;
}

static inline void spill_set(uint64_t *words, unsigned i, bool spill)
{
    uint64_t bit = UINT64_C(1) << (i & 63);
    words[i >> 6] = spill ? words[i >> 6] | bit : words[i >> 6] & ~bit;
}

static inline bool spill_get(const uint64_t *words, unsigned i)
{
    return (words[i >> 6] >> (i & 63)) & 1;
}

static void l0_list(struct class_l0 *z, unsigned e, uint32_t slot)
{
    uint64_t bit = UINT64_C(1) << (e & 63);
    if (!(z->listed[e >> 6] & bit)) {
        z->listed[e >> 6] |= bit;
        z->used[z->n_used++] = (uint16_t)e;
    }
    z->slot[e] = slot;
}

/*
 * Puts the tile of a published slot into L0. The key must be in the L0
 * domain. An ASCII entry is set once. In the id table, the new id goes
 * first in its set, and the last id of the set drops.
 */
static void l0_fill(struct cell_class *k, uint32_t slot)
{
    struct class_l0 *z = k->l0;
    const struct cell_key *key = &k->keys[slot];
    const uint32_t *px = tile_at(k, slot);
    bool spill = (k->flags[slot] & SLOT_SPILL) != 0;
    if (l0_ascii(key)) {
        unsigned e = l0_ascii_entry(key);
        const uint32_t **p = &z->t.px[e / CELL_CACHE_L0_CPS][e % CELL_CACHE_L0_CPS];
        if (*p)
            return;
        *p = px;
        spill_set(z->t.spill[e / CELL_CACHE_L0_CPS], e % CELL_CACHE_L0_CPS, spill);
        l0_list(z, e, slot);
        return;
    }
    unsigned i = cell_l0_index(key->glyph);
    struct cell_l0_id *set = &z->t.ids[i];
    for (int j = 0; j < CELL_CACHE_L0_WAYS; j++)
        if (set[j].id == key->glyph)
            return;
    for (unsigned j = i + CELL_CACHE_L0_WAYS - 1; j > i; j--) {
        if (!z->t.ids[j - 1].id)
            continue;
        z->t.ids[j] = z->t.ids[j - 1];
        spill_set(z->t.id_spill, j, spill_get(z->t.id_spill, j - 1));
        l0_list(z, L0_ID0 + j, z->slot[L0_ID0 + j - 1]);
    }
    set[0] = (struct cell_l0_id){ .id = key->glyph, .px = px };
    spill_set(z->t.id_spill, i, spill);
    l0_list(z, L0_ID0 + i, slot);
}

/* Takes a slot that leaves the index out of L0. */
static void l0_forget(struct cell_class *k, uint32_t slot)
{
    struct class_l0 *z = k->l0;
    const struct cell_key *key = &k->keys[slot];
    if (!l0_domain(k, key))
        return;
    if (l0_ascii(key)) {
        unsigned e = l0_ascii_entry(key);
        const uint32_t **p = &z->t.px[e / CELL_CACHE_L0_CPS][e % CELL_CACHE_L0_CPS];
        if (*p && z->slot[e] == slot)
            *p = NULL;
        return;
    }
    unsigned i = cell_l0_index(key->glyph);
    for (unsigned j = i; j < i + CELL_CACHE_L0_WAYS; j++)
        if (z->t.ids[j].id == key->glyph && z->slot[L0_ID0 + j] == slot)
            z->t.ids[j] = (struct cell_l0_id){0};
}

/* Empties L0. Only the listed entries are cleared. */
static void l0_clear(struct class_l0 *z)
{
    for (int i = 0; i < z->n_used; i++) {
        unsigned e = z->used[i];
        if (e < L0_ID0)
            z->t.px[e / CELL_CACHE_L0_CPS][e % CELL_CACHE_L0_CPS] = NULL;
        else
            z->t.ids[e - L0_ID0] = (struct cell_l0_id){0};
    }
    z->n_used = 0;
    memset(z->listed, 0, sizeof(z->listed));
    memset(z->t.spill, 0, sizeof(z->t.spill));
    memset(z->t.id_spill, 0, sizeof(z->t.id_spill));
}

/* Sets the L0 domain of the frame. A new domain empties L0. */
static void l0_domain_set(struct class_l0 *z, uint64_t font, uint32_t fg, uint32_t bg)
{
    if (z->valid && z->font == font && z->fg == fg && z->bg == bg)
        return;
    l0_clear(z);
    z->valid = true;
    z->font = font;
    z->fg = fg;
    z->bg = bg;
}

/* Removes a slot from the index and from L0. */
static void slot_unlink(struct cell_class *k, uint32_t slot)
{
    l0_forget(k, slot);
    index_remove(k, slot);
}

/* Drops the part of the free list that the last phase claimed. */
static void free_compact(struct cell_class *k)
{
    int taken = atomic_load_explicit(&k->cursor, memory_order_relaxed);
    if (taken > k->n_free)
        taken = k->n_free;
    if (taken > 0)
        k->n_free -= taken;
    atomic_store_explicit(&k->cursor, 0, memory_order_relaxed);
}

static void class_free(struct cell_class *k)
{
    for (int i = 0; i < k->n_slabs; i++)
        free(k->slabs[i]);
    free(k->slabs);
    free(k->keys);
    free(k->tags);
    free(k->stamps);
    free(k->flags);
    free(k->index);
    free(k->free);
    free(k->l0);
    free(k);
}

static void class_drop(struct cell_cache *c, struct cell_class *k)
{
    for (struct cell_class **pp = &c->classes; *pp; pp = &(*pp)->next) {
        if (*pp == k) {
            *pp = k->next;
            break;
        }
    }
    c->bytes -= k->bytes;
    class_free(k);
}

/*
 * The least recently used class other than `k` that no frame used for a
 * while. `k` can be NULL.
 */
static struct cell_class *class_idle(const struct cell_cache *c, const struct cell_class *k)
{
    struct cell_class *best = NULL;
    for (struct cell_class *o = c->classes; o; o = o->next) {
        if (o == k || c->clock - o->used < CLASS_KEEP_FRAMES)
            continue;
        if (!best || c->clock - o->used > c->clock - best->used)
            best = o;
    }
    return best;
}

static inline size_t slab_cost(const struct cell_class *k)
{
    return ((size_t)1 << k->slab_shift) * slot_cost(k) + sizeof(uint32_t *);
}

/*
 * The largest class other than `k` that stays at least as large as `k`
 * after it gives `need` bytes to `k`. `has` is the size of `k`. NULL when
 * there is none.
 */
static struct cell_class *class_donor(const struct cell_cache *c, const struct cell_class *k,
                                      size_t has, size_t need)
{
    struct cell_class *best = NULL;
    for (struct cell_class *o = c->classes; o; o = o->next)
        if (o != k && o->n_slabs && (!best || o->bytes > best->bytes))
            best = o;
    if (best && best->bytes - slab_cost(best) >= has + need)
        return best;
    return NULL;
}

static bool grow_array(void *arr, size_t n, size_t elem)
{
    void *old;
    memcpy(&old, arr, sizeof(old));
    void *p = realloc(old, n * elem);
    if (!p)
        return false;
    memcpy(arr, &p, sizeof(p));
    return true;
}

static bool index_resize(struct cell_class *k, uint32_t size)
{
    struct index_entry *index = calloc(size, sizeof(*index));
    if (!index)
        return false;
    struct index_entry *old = k->index;
    uint32_t old_size = old ? k->index_mask + 1 : 0;
    k->index = index;
    k->index_mask = size - 1;
    for (uint32_t i = 0; i < old_size; i++)
        if (old[i].slot1)
            index_insert(k, old[i].slot1 - 1, old[i].tag);
    free(old);
    return true;
}

/*
 * Frees the last slab of a class. Its tiles leave the index and its slots
 * leave the free list. A class with no slab left is dropped.
 */
static void class_shrink(struct cell_cache *c, struct cell_class *k)
{
    uint32_t tiles = 1u << k->slab_shift;
    uint32_t first = (uint32_t)k->slots - tiles;
    for (uint32_t s = first; s < (uint32_t)k->slots; s++)
        if (k->flags[s] & SLOT_LIVE)
            slot_unlink(k, s);
    int n = 0;
    for (int i = 0; i < k->n_free; i++)
        if (k->free[i] < first)
            k->free[n++] = k->free[i];
    k->n_free = n;
    free(k->slabs[--k->n_slabs]);
    k->slots = (int)first;
    if (k->fresh > k->slots)
        k->fresh = k->slots;
    if (k->hand >= k->fresh)
        k->hand = 0;
    size_t cost = slab_cost(k);
    k->bytes -= cost;
    c->bytes -= cost;
    if (!k->n_slabs) {
        class_drop(c, k);
        return;
    }
    /* Shrinking cannot lose data. A failed realloc keeps the larger array. */
    size_t slots = (size_t)k->slots;
    (void)grow_array(&k->slabs, (size_t)k->n_slabs, sizeof(*k->slabs));
    (void)grow_array(&k->keys, slots, sizeof(*k->keys));
    (void)grow_array(&k->tags, slots, sizeof(*k->tags));
    (void)grow_array(&k->stamps, slots, sizeof(*k->stamps));
    (void)grow_array(&k->flags, slots, sizeof(*k->flags));
    (void)grow_array(&k->free, slots, sizeof(*k->free));
    uint32_t size = k->index_mask + 1;
    while (size > 64 && size / 2 >= 2 * slots)
        size /= 2;
    if (size != k->index_mask + 1)
        (void)index_resize(k, size);
}

/*
 * Makes room for `need` more bytes for class `k` of `has` bytes. `k` is
 * NULL for a class that does not exist yet. It first frees the idle other
 * classes, least recently used first. Then it takes slabs from the largest
 * other class while that class stays at least as large. Returns false when
 * no room is left.
 */
static bool make_room(struct cell_cache *c, const struct cell_class *k, size_t has, size_t need)
{
    while (c->bytes + need > c->budget) {
        struct cell_class *o = class_idle(c, k);
        if (o) {
            class_drop(c, o);
            continue;
        }
        o = class_donor(c, k, has, need);
        if (!o)
            return false;
        class_shrink(c, o);
    }
    return true;
}

/*
 * Adds one slab within the budget. Returns false when no room or no memory
 * is left: the class then evicts its own tiles.
 */
static bool class_grow(struct cell_cache *c, struct cell_class *k)
{
    size_t tiles = (size_t)1 << k->slab_shift;
    size_t need = slab_cost(k);
    if ((size_t)k->slots + tiles > INT32_MAX / 4 || !make_room(c, k, k->bytes, need))
        return false;

    size_t slots = (size_t)k->slots + tiles;
    if (!grow_array(&k->slabs, (size_t)k->n_slabs + 1, sizeof(*k->slabs)) ||
        !grow_array(&k->keys, slots, sizeof(*k->keys)) ||
        !grow_array(&k->tags, slots, sizeof(*k->tags)) ||
        !grow_array(&k->stamps, slots, sizeof(*k->stamps)) ||
        !grow_array(&k->flags, slots, sizeof(*k->flags)) ||
        !grow_array(&k->free, slots, sizeof(*k->free)))
        return false;
    uint32_t index_size = k->index ? k->index_mask + 1 : 64;
    while (index_size < 2 * slots)
        index_size *= 2;
    if ((!k->index || index_size != k->index_mask + 1) && !index_resize(k, index_size))
        return false;
    uint32_t *slab = malloc(tiles * k->tile_px * sizeof(uint32_t));
    if (!slab)
        return false;

    k->slabs[k->n_slabs++] = slab;
    memset(k->stamps + k->slots, 0, tiles * sizeof(*k->stamps));
    memset(k->flags + k->slots, 0, tiles * sizeof(*k->flags));
    k->slots = (int)slots;
    k->bytes += need;
    c->bytes += need;
    return true;
}

static void take_fresh(struct cell_class *k, int target)
{
    while (k->n_free < target && k->fresh < k->slots)
        k->free[k->n_free++] = (uint32_t)k->fresh++;
}

/*
 * Evicts up to `want` slots by clock. A slot used in the last frame of the
 * class stays. A slot used since the hand last passed it gets one more
 * turn.
 */
static void clock_evict(struct cell_class *k, int want)
{
    int got = 0;
    for (int scan = 0, limit = 2 * k->fresh; got < want && scan < limit; scan++) {
        if (k->hand >= k->fresh)
            k->hand = 0;
        uint32_t s = (uint32_t)k->hand++;
        uint8_t f = k->flags[s];
        if (!(f & SLOT_LIVE) || k->stamps[s] == k->last)
            continue;
        if (f & SLOT_REF) {
            k->flags[s] = f & ~SLOT_REF;
            continue;
        }
        slot_unlink(k, s);
        k->flags[s] = 0;
        k->free[k->n_free++] = s;
        got++;
    }
}

static void stamp(struct cell_class *k, uint32_t slot, uint32_t now)
{
    if (k->flags[slot] & SLOT_LIVE) {
        k->stamps[slot] = now;
        k->flags[slot] |= SLOT_REF;
    }
}

/*
 * Stamps each slot that L0 holds as used in the last frame. An L0 hit
 * leaves no trace in the hit log, so this counts it before an eviction.
 * A listed entry that was cleared later stamps a slot that it no longer
 * holds. That only keeps the slot a little longer.
 */
static void l0_stamp(struct cell_class *k)
{
    const struct class_l0 *z = k->l0;
    for (int i = 0; i < z->n_used; i++)
        stamp(k, z->slot[z->used[i]], k->last);
}

/*
 * Fills the free list. `cells` is the cell count of the frame. The first
 * refill of a class takes room for all of them, so that a cold frame can
 * claim each new key.
 */
static void refill(struct cell_cache *c, struct cell_class *k, int cells)
{
    int asked = k->asked;
    int target = asked > CELL_CACHE_REFILL_MIN ? asked : CELL_CACHE_REFILL_MIN;
    if (!k->fresh && cells > target)
        target = cells;
    take_fresh(k, target);
    while (k->n_free < target && class_grow(c, k))
        take_fresh(k, target);
    bool protect = k->protect;
    k->protect = false;
    int want = target - k->n_free;
    if (want > asked)
        want = asked;
    if (!protect && want > 0) {
        l0_stamp(k);
        clock_evict(k, want);
    }
}

static struct cell_class *class_get(struct cell_cache *c, struct vector2i cell)
{
    for (struct cell_class *k = c->classes; k; k = k->next)
        if (vector2i_eq(k->cell, cell))
            return k;
    if (cell.x <= 0 || cell.y <= 0 || cell.x > CELL_MAX || cell.y > CELL_MAX)
        return NULL;

    size_t tile_px = (size_t)cell.x * (size_t)cell.y;
    size_t cost = tile_px * sizeof(uint32_t) + SLOT_META;
    size_t fixed = sizeof(struct cell_class) + sizeof(uint32_t *);
    if (c->budget < fixed || (c->budget - fixed) / cost < CELL_CACHE_MIN_TILES)
        return NULL;
    size_t max_tiles = (c->budget - fixed) / cost;
    size_t want = SLAB_BYTES / (tile_px * sizeof(uint32_t));
    if (want < CELL_CACHE_MIN_TILES)
        want = CELL_CACHE_MIN_TILES;
    if (want > max_tiles)
        want = max_tiles;
    int shift = 0;
    while (((size_t)2 << shift) <= want)
        shift++;
    /* The class and its first slab must fit. */
    if (!make_room(c, NULL, 0, sizeof(struct cell_class) + ((size_t)1 << shift) * cost +
                                   sizeof(uint32_t *)))
        return NULL;

    struct cell_class *k = calloc(1, sizeof(*k));
    if (!k)
        return NULL;
    k->l0 = calloc(1, sizeof(*k->l0));
    if (!k->l0) {
        free(k);
        return NULL;
    }
    k->cell = cell;
    k->tile_px = tile_px;
    k->slab_shift = shift;
    k->bytes = sizeof(*k);
    c->bytes += k->bytes;
    k->next = c->classes;
    c->classes = k;
    return k;
}

static void worker_reset(struct cell_cache_worker *w, const struct cell_class *k)
{
    for (int i = 0; i < w->n_pending; i++)
        w->pending_index[w->pending[i].pos] = 0;
    w->n_pending = 0;
    /* A filter entry is set only with a log entry or an overflow. */
    if (w->n_hits || w->hit_overflow)
        memset(w->hit_filter, 0, sizeof(w->hit_filter));
    w->n_hits = 0;
    w->hit_overflow = false;
    w->asked = 0;
    w->phase = k ? k->phase : 0;
}

/* Advances the frame clock. 0 and 1 stay unused, so a stamp of 0 is never
 * a recent frame. */
static void clock_tick(struct cell_cache *c)
{
    if (++c->clock < 2)
        c->clock = 2;
}

struct cell_cache *cell_cache_create(size_t budget)
{
    if (!budget)
        return NULL;
    struct cell_cache *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->budget = budget;
    c->clock = 2;
    return c;
}

void cell_cache_destroy(struct cell_cache *c)
{
    if (!c)
        return;
    while (c->classes) {
        struct cell_class *k = c->classes;
        c->classes = k->next;
        class_free(k);
    }
    free(c);
}

struct cell_cache_worker *cell_cache_worker_create(void)
{
    return calloc(1, sizeof(struct cell_cache_worker));
}

void cell_cache_worker_destroy(struct cell_cache_worker *w)
{
    free(w);
}

const struct cell_l0 *cell_class_l0(const struct cell_class *k)
{
    return k ? &k->l0->t : &empty_l0;
}

struct cell_class *cell_cache_begin(struct cell_cache *c, struct vector2i cell,
                                    uint64_t font_version,
                                    uint32_t default_fg, uint32_t default_bg, int cells,
                                    struct cell_cache_worker *const *workers, int n_workers)
{
    struct cell_class *k = c ? class_get(c, cell) : NULL;
    if (k) {
        k->used = c->clock;
        l0_domain_set(k->l0, font_version, default_fg, default_bg);
        free_compact(k);
        refill(c, k, cells);
        /* A class with no room stays in the list, so the next frame does
         * not allocate it again. It is freed when it idles. */
        if (k->slots < CELL_CACHE_MIN_TILES)
            k = NULL;
        else
            k->phase = c->clock;
    }
    /* A frame without a class has no publish. It still counts, so that the
     * other classes idle. */
    if (c && !k)
        clock_tick(c);
    for (int i = 0; i < n_workers; i++)
        if (workers[i])
            worker_reset(workers[i], k);
    return k;
}

struct cell_tile cell_cache_find(const struct cell_class *k, struct cell_cache_worker *w,
                                 const struct cell_key *key)
{
    if (!k || !w || !key->glyph || !k->phase || w->phase != k->phase)
        return (struct cell_tile){0};
    uint32_t tag = (uint32_t)key_hash(key);
    int slot = index_find(k, tag, key);
    if (slot >= 0) {
        uint32_t *f = &w->hit_filter[(uint32_t)slot & (HIT_FILTER - 1)];
        if (*f != (uint32_t)slot + 1) {
            *f = (uint32_t)slot + 1;
            if (w->n_hits < CELL_CACHE_HIT_LOG)
                w->hits[w->n_hits++] = (uint32_t)slot;
            else
                w->hit_overflow = true;
        }
    } else if (w->n_pending) {
        for (uint32_t i = tag & (PENDING_INDEX - 1);; i = (i + 1) & (PENDING_INDEX - 1)) {
            uint16_t e = w->pending_index[i];
            if (!e)
                break;
            const struct pending *p = &w->pending[e - 1];
            if (p->tag == tag && cell_key_eq(&k->keys[p->slot], key)) {
                slot = (int)p->slot;
                break;
            }
        }
    }
    if (slot < 0)
        return (struct cell_tile){0};
    return (struct cell_tile){
        .px = tile_at(k, (uint32_t)slot), .spill = (k->flags[slot] & SLOT_SPILL) != 0,
    };
}

uint32_t *cell_cache_claim(struct cell_class *k, struct cell_cache_worker *w,
                           const struct cell_key *key, bool spill)
{
    if (!k || !w || !key->glyph || !k->phase || w->phase != k->phase)
        return NULL;
    uint64_t h = key_hash(key);
    uint32_t *set = w->seen[(h >> 32) & (CELL_CACHE_SEEN / SEEN_WAYS - 1)];
    uint32_t mark = (uint32_t)(h >> 32) | 1;
    bool seen = false;
    for (int i = 0; i < SEEN_WAYS; i++)
        seen |= set[i] == mark;
    if (!seen) {
        memmove(set + 1, set, (SEEN_WAYS - 1) * sizeof(*set));
        set[0] = mark;
        return NULL;
    }
    w->asked++;
    if (w->n_pending >= CELL_CACHE_PENDING)
        return NULL;
    int i = atomic_fetch_add_explicit(&k->cursor, 1, memory_order_relaxed);
    if (i >= k->n_free)
        return NULL;

    uint32_t slot = k->free[k->n_free - 1 - i];
    uint32_t tag = (uint32_t)h;
    k->keys[slot] = *key;
    k->flags[slot] = spill ? SLOT_SPILL : 0;
    uint32_t pos = tag & (PENDING_INDEX - 1);
    while (w->pending_index[pos])
        pos = (pos + 1) & (PENDING_INDEX - 1);
    w->pending[w->n_pending] = (struct pending){ .slot = slot, .tag = tag, .pos = (uint16_t)pos };
    w->pending_index[pos] = (uint16_t)++w->n_pending;

    return tile_at(k, slot);
}

void cell_cache_publish(struct cell_cache *c, struct cell_class *k,
                        struct cell_cache_worker *const *workers, int n_workers)
{
    if (!c || !k || !k->phase)
        return;
    uint32_t now = c->clock;
    free_compact(k);
    int asked = 0;
    bool overflow = false;
    for (int i = 0; i < n_workers; i++) {
        struct cell_cache_worker *w = workers[i];
        if (!w || w->phase != k->phase)
            continue;
        asked += w->asked;
        overflow |= w->hit_overflow;
        for (int j = 0; j < w->n_pending; j++) {
            uint32_t s = w->pending[j].slot, tag = w->pending[j].tag;
            if (index_find(k, tag, &k->keys[s]) >= 0) {
                /* Another worker published this key first. */
                k->flags[s] = 0;
                k->free[k->n_free++] = s;
                continue;
            }
            index_insert(k, s, tag);
            k->tags[s] = tag;
            k->flags[s] |= SLOT_LIVE;
            stamp(k, s, now);
            if (l0_domain(k, &k->keys[s]))
                l0_fill(k, s);
        }
        for (int j = 0; j < w->n_hits; j++) {
            uint32_t s = w->hits[j];
            stamp(k, s, now);
            if (l0_domain(k, &k->keys[s]))
                l0_fill(k, s);
        }
        w->phase = 0;
    }
    k->asked = asked;
    k->protect = overflow;
    k->last = now;
    k->phase = 0;
    clock_tick(c);
}
