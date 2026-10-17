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
     * The free list. Claims take [0, n_free) in order through `cursor`.
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
    uint32_t epoch;
};

struct pending {
    uint32_t slot;
    uint32_t tag;
    uint16_t pos;
};

struct cell_cache_worker {
    struct cell_l0 l0;
    /* The L0 domain. */
    const struct cell_class *dom_class;
    uint64_t dom_font;
    uint32_t dom_fg, dom_bg;
    uint32_t dom_epoch;
    int n_l0;
    uint32_t l0_slots[FONT_VARIANT_COUNT * CELL_CACHE_L0_CPS];
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

/* Drops the part of the free list that the last phase claimed. */
static void free_compact(struct cell_class *k)
{
    int taken = atomic_load_explicit(&k->cursor, memory_order_relaxed);
    if (taken > k->n_free)
        taken = k->n_free;
    if (taken > 0) {
        memmove(k->free, k->free + taken, (size_t)(k->n_free - taken) * sizeof(*k->free));
        k->n_free -= taken;
    }
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
    c->epoch++;
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
            index_remove(k, s);
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
    c->epoch++;
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
static void clock_evict(struct cell_cache *c, struct cell_class *k, int want)
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
        index_remove(k, s);
        k->flags[s] = 0;
        k->free[k->n_free++] = s;
        got++;
    }
    if (got)
        c->epoch++;
}

static void refill(struct cell_cache *c, struct cell_class *k)
{
    int asked = k->asked;
    int target = asked > CELL_CACHE_REFILL_MIN ? asked : CELL_CACHE_REFILL_MIN;
    take_fresh(k, target);
    while (k->n_free < target && class_grow(c, k))
        take_fresh(k, target);
    bool protect = k->protect;
    k->protect = false;
    int want = target - k->n_free;
    if (want > asked)
        want = asked;
    if (!protect && want > 0)
        clock_evict(c, k, want);
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
    k->cell = cell;
    k->tile_px = tile_px;
    k->slab_shift = shift;
    k->bytes = sizeof(*k);
    c->bytes += k->bytes;
    k->next = c->classes;
    c->classes = k;
    return k;
}

static bool l0_domain(const struct cell_cache_worker *w, const struct cell_class *k,
                      const struct cell_key *key)
{
    const uint64_t glyph_bits =
        (uint64_t)(CELL_CACHE_L0_CPS - 1) | (uint64_t)FONT_STYLE_VARIANT_MASK << 32;
    return w->dom_class == k && !(key->glyph & ~glyph_bits) && key->font_version == w->dom_font &&
           key->fg == w->dom_fg && key->bg == w->dom_bg && !key->cursor && !key->flags &&
           !key->half;
}

static void l0_fill(struct cell_cache_worker *w, const struct cell_key *key, uint32_t slot,
                    const uint32_t *px, bool spill)
{
    unsigned cp = (unsigned)(key->glyph & (CELL_CACHE_L0_CPS - 1));
    unsigned v = (unsigned)(key->glyph >> 32) & FONT_STYLE_VARIANT_MASK;
    if (w->l0.px[v][cp])
        return;
    w->l0.px[v][cp] = px;
    if (spill)
        w->l0.spill[v][cp >> 6] |= UINT64_C(1) << (cp & 63);
    w->l0_slots[w->n_l0++] = slot;
}

static void worker_reset(struct cell_cache_worker *w, uint32_t epoch,
                         const struct cell_class *k, uint64_t font_version,
                         uint32_t default_fg, uint32_t default_bg)
{
    for (int i = 0; i < w->n_pending; i++)
        w->pending_index[w->pending[i].pos] = 0;
    w->n_pending = 0;
    w->n_hits = 0;
    w->hit_overflow = false;
    w->asked = 0;
    memset(w->hit_filter, 0, sizeof(w->hit_filter));
    w->phase = k ? k->phase : 0;

    bool same = k && w->dom_class == k && w->dom_font == font_version &&
                w->dom_fg == default_fg && w->dom_bg == default_bg && w->dom_epoch == epoch;
    if (same)
        return;
    if (w->dom_class || w->n_l0)
        memset(&w->l0, 0, sizeof(w->l0));
    w->n_l0 = 0;
    w->dom_class = k;
    w->dom_font = font_version;
    w->dom_fg = default_fg;
    w->dom_bg = default_bg;
    w->dom_epoch = epoch;
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

const struct cell_l0 *cell_cache_worker_l0(const struct cell_cache_worker *w)
{
    return w ? &w->l0 : &empty_l0;
}

struct cell_class *cell_cache_begin(struct cell_cache *c, struct vector2i cell,
                                    uint64_t font_version,
                                    uint32_t default_fg, uint32_t default_bg,
                                    struct cell_cache_worker *const *workers, int n_workers)
{
    struct cell_class *k = c ? class_get(c, cell) : NULL;
    if (k) {
        k->used = c->clock;
        free_compact(k);
        refill(c, k);
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
            worker_reset(workers[i], c ? c->epoch : 0, k, font_version, default_fg, default_bg);
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
    const uint32_t *px = tile_at(k, (uint32_t)slot);
    bool spill = k->flags[slot] & SLOT_SPILL;
    if (l0_domain(w, k, key))
        l0_fill(w, key, (uint32_t)slot, px, spill);
    return (struct cell_tile){ .px = px, .spill = spill };
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

    uint32_t slot = k->free[i];
    uint32_t tag = (uint32_t)h;
    k->keys[slot] = *key;
    k->flags[slot] = spill ? SLOT_SPILL : 0;
    uint32_t pos = tag & (PENDING_INDEX - 1);
    while (w->pending_index[pos])
        pos = (pos + 1) & (PENDING_INDEX - 1);
    w->pending[w->n_pending] = (struct pending){ .slot = slot, .tag = tag, .pos = (uint16_t)pos };
    w->pending_index[pos] = (uint16_t)++w->n_pending;

    uint32_t *px = tile_at(k, slot);
    if (l0_domain(w, k, key))
        l0_fill(w, key, slot, px, spill);
    return px;
}

static void stamp(struct cell_class *k, uint32_t slot, uint32_t now)
{
    if (k->flags[slot] & SLOT_LIVE) {
        k->stamps[slot] = now;
        k->flags[slot] |= SLOT_REF;
    }
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
                c->epoch++;
                continue;
            }
            index_insert(k, s, tag);
            k->tags[s] = tag;
            k->flags[s] |= SLOT_LIVE;
            stamp(k, s, now);
        }
        for (int j = 0; j < w->n_hits; j++)
            stamp(k, w->hits[j], now);
        if (w->dom_class == k)
            for (int j = 0; j < w->n_l0; j++)
                stamp(k, w->l0_slots[j], now);
        w->phase = 0;
    }
    k->asked = asked;
    k->protect = overflow;
    k->last = now;
    k->phase = 0;
    clock_tick(c);
}
