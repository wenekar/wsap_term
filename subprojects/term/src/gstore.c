#include "gstore.h"
#include "internal.h"
#include "screen.h"
#include "unicode/grapheme.h"

#include <stdlib.h>
#include <string.h>

#define GSTORE_MAX_ENTRIES 0x00FFFFFFu

struct gentry {
    union {
        uint32_t offset;    /* Offset into arena for code points past index 1 */
        uint32_t next_free; /* Next entry in free list (1-based, 0 = none) */
    };
    uint16_t len;           /* Number of code points */
    bool mark;              /* Reachable in current GC sweep */
    bool allocated;         /* True if actively holding a cluster */
    uint32_t cp0;           /* First code point (inlined) */
    uint32_t cp1;           /* Second code point (inlined) */
};

_Static_assert(sizeof(struct gentry) == 16,
               "struct gentry must stay 16 bytes (4 entries per cache line)");

struct gslot {
    uint32_t hash;          /* 32-bit hash of cluster code points */
    uint32_t entry_idx;     /* 1-based index into g->entries (0 = empty) */
};

struct gstore {
    struct wsap_vtcore *term;
    uint32_t *arena;
    size_t arena_len;
    size_t arena_cap;

    struct gentry *entries;
    uint32_t entries_count;
    uint32_t entries_cap;
    uint32_t free_head; /* 1-based index, 0 = none */

    uint32_t live_count;
    uint32_t last_live;
    uint32_t sweep_threshold;
    uint32_t pin_depth;

    struct gslot *slots;
    size_t slot_count;
};

static uint64_t hash_cps(const uint32_t *cps, int len)
{
    uint64_t h = HASH64_BASIS;
    for (int i = 0; i < len; i++)
        h = hash64_step(h, cps[i]);
    return hash64_finish(h);
}

static inline bool gentry_matches(const struct gstore *g, const struct gentry *e,
                                  const uint32_t *cps, int n)
{
    if ((int)e->len != n)
        return false;
    if (e->cp0 != cps[0] || e->cp1 != cps[1])
        return false;
    if (n == 2)
        return true;
    return !memcmp(&g->arena[e->offset], &cps[2], (size_t)(n - 2) * sizeof(*cps));
}

int wsap_vtcore_gstore_expand(const struct gstore *g, uint32_t handle, uint32_t *cps, int cap)
{
    if (cap <= 0)
        return 0;
    if (!g || !wsap_vtcore_grapheme_is_handle(handle)) {
        cps[0] = handle;
        return 1;
    }
    uint32_t idx = handle - WSAP_VTCORE_GRAPHEME_HANDLE_BASE;
    if (idx >= g->entries_count || !g->entries[idx].allocated) {
        cps[0] = 0xFFFD;
        return 1;
    }
    const struct gentry *e = &g->entries[idx];
    int n = (int)e->len;
    if (n > cap) {
        cps[0] = 0xFFFD;
        return 1;
    }
    cps[0] = e->cp0;
    cps[1] = e->cp1;
    if (n > 2)
        memcpy(&cps[2], &g->arena[e->offset], (size_t)(n - 2) * sizeof(*cps));
    return n;
}

static void gstore_insert_slot(struct gstore *g, uint32_t h32, uint32_t entry_idx)
{
    uint32_t mask = (uint32_t)g->slot_count - 1;
    uint32_t idx = h32 & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        struct gslot *slot = &g->slots[idx];
        if (slot->entry_idx == 0) {
            slot->hash = h32;
            slot->entry_idx = entry_idx + 1;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static bool grow_slots(struct gstore *g)
{
    size_t want = g->slot_count ? g->slot_count * 2 : 256;
    struct gslot *next = calloc(want, sizeof(*next));
    if (!next)
        return false;

    free(g->slots);
    g->slots = next;
    g->slot_count = want;

    for (uint32_t i = 0; i < g->entries_count; i++) {
        struct gentry *e = &g->entries[i];
        if (!e->allocated)
            continue;
        uint32_t cps_buf[WSAP_VTCORE_GRAPHEME_MAX_LEN];
        int n = wsap_vtcore_gstore_expand(g, WSAP_VTCORE_GRAPHEME_HANDLE_BASE + i, cps_buf, WSAP_VTCORE_GRAPHEME_MAX_LEN);
        uint64_t h = hash_cps(cps_buf, n);
        uint32_t h32 = (uint32_t)h ? (uint32_t)h : 1;
        gstore_insert_slot(g, h32, i);
    }
    return true;
}

static uint32_t gstore_find(const struct gstore *g, const uint32_t *cps, int n, uint32_t h32)
{
    if (!g->slots || g->slot_count == 0)
        return 0;
    uint32_t mask = (uint32_t)g->slot_count - 1;
    uint32_t idx = h32 & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        const struct gslot *slot = &g->slots[idx];
        if (slot->entry_idx == 0)
            return 0;
        if (slot->hash == h32) {
            uint32_t e_idx = slot->entry_idx - 1;
            const struct gentry *e = &g->entries[e_idx];
            if (e->allocated && gentry_matches(g, e, cps, n))
                return WSAP_VTCORE_GRAPHEME_HANDLE_BASE + e_idx;
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

struct gstore *wsap_vtcore_gstore_new(struct wsap_vtcore *t)
{
    struct gstore *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->term = t;
    g->sweep_threshold = 1024;
    return g;
}

void wsap_vtcore_gstore_free(struct gstore *g)
{
    if (!g)
        return;
    free(g->arena);
    free(g->entries);
    free(g->slots);
    free(g);
}

uint32_t wsap_vtcore_gstore_count(const struct gstore *g)
{
    return g ? g->live_count : 0;
}

void wsap_vtcore_gstore_pin(struct gstore *g)
{
    if (g)
        g->pin_depth++;
}

void wsap_vtcore_gstore_unpin(struct gstore *g)
{
    if (g && g->pin_depth > 0)
        g->pin_depth--;
}

void wsap_vtcore_gstore_mark(struct gstore *g, uint32_t handle)
{
    if (!g || !wsap_vtcore_grapheme_is_handle(handle))
        return;
    uint32_t idx = handle - WSAP_VTCORE_GRAPHEME_HANDLE_BASE;
    if (idx < g->entries_count && g->entries[idx].allocated)
        g->entries[idx].mark = true;
}

static void sweep_mark_row(struct gstore *g, const struct cell *cells, int len)
{
    for (int x = 0; x < len; x++) {
        uint32_t c = cells[x].c;
        if (wsap_vtcore_grapheme_is_handle(c))
            wsap_vtcore_gstore_mark(g, c);
    }
}

void wsap_vtcore_gstore_sweep(struct gstore *g, struct wsap_vtcore *t)
{
    if (!g)
        return;

    if (t) {
        if (wsap_vtcore_grapheme_is_handle(t->last_char))
            wsap_vtcore_gstore_mark(g, t->last_char);

        struct screen *s = &t->scr;
        for (int y = 0; y < s->rows; y++) {
            struct srow *row = screen_get_row(s, y);
            if (row && row->kind == ROW_CELLS && row->len > 0)
                sweep_mark_row(g, row->cells, (int)row->len);
        }
        if (t->alternate && t->alt_allocated) {
            struct screen *as = &t->alt_scr;
            for (int y = 0; y < as->rows; y++) {
                struct srow *row = screen_get_row(as, y);
                if (row && row->kind == ROW_CELLS && row->len > 0)
                    sweep_mark_row(g, row->cells, (int)row->len);
            }
        }
    }

    /* Compact the code point arena for clusters with n > 2. */
    uint32_t new_arena_len = 0;
    uint32_t *packed = g->arena ? malloc(g->arena_cap * sizeof(*packed)) : NULL;
    if (packed) {
        for (uint32_t i = 0; i < g->entries_count; i++) {
            struct gentry *e = &g->entries[i];
            if (e->allocated && e->mark && e->len > 2) {
                size_t extra = (size_t)(e->len - 2);
                memcpy(&packed[new_arena_len], &g->arena[e->offset],
                       extra * sizeof(*packed));
                e->offset = new_arena_len;
                new_arena_len += (uint32_t)extra;
            }
        }
        free(g->arena);
        g->arena = packed;
        g->arena_len = new_arena_len;
    }

    /* Rebuild open-addressing slot table and free list. */
    g->free_head = 0;
    uint32_t live = 0;
    if (g->slots)
        memset(g->slots, 0, g->slot_count * sizeof(*g->slots));

    for (uint32_t i = 0; i < g->entries_count; i++) {
        struct gentry *e = &g->entries[i];
        if (e->allocated) {
            if (e->mark) {
                live++;
                e->mark = false;
                if (g->slots) {
                    uint32_t cps_buf[WSAP_VTCORE_GRAPHEME_MAX_LEN];
                    int n = wsap_vtcore_gstore_expand(g, WSAP_VTCORE_GRAPHEME_HANDLE_BASE + i, cps_buf, WSAP_VTCORE_GRAPHEME_MAX_LEN);
                    uint64_t h = hash_cps(cps_buf, n);
                    uint32_t h32 = (uint32_t)h ? (uint32_t)h : 1;
                    gstore_insert_slot(g, h32, i);
                }
            } else {
                e->allocated = false;
                e->len = 0;
                e->offset = 0;
                e->cp0 = 0;
                e->cp1 = 0;
                e->next_free = g->free_head;
                g->free_head = i + 1;
            }
        } else {
            e->next_free = g->free_head;
            g->free_head = i + 1;
        }
    }

    g->live_count = live;
    g->last_live = live;
    g->sweep_threshold = live * 2;
    if (g->sweep_threshold < 1024)
        g->sweep_threshold = 1024;
}

uint32_t wsap_vtcore_gstore_intern(struct gstore *g, const uint32_t *cps, int n)
{
    if (!g || n <= 0)
        return 0;
    if (n == 1)
        return cps[0];
    if (n > WSAP_VTCORE_GRAPHEME_MAX_LEN)
        n = WSAP_VTCORE_GRAPHEME_MAX_LEN;

    uint64_t h = hash_cps(cps, n);
    uint32_t h32 = (uint32_t)h ? (uint32_t)h : 1;

    uint32_t existing = gstore_find(g, cps, n, h32);
    if (existing)
        return existing;

    if (g->term && g->pin_depth == 0 &&
        (g->live_count >= g->sweep_threshold ||
         g->live_count >= GSTORE_MAX_ENTRIES)) {
        wsap_vtcore_gstore_sweep(g, g->term);
        existing = gstore_find(g, cps, n, h32);
        if (existing)
            return existing;
    }

    if (!g->slots || (g->live_count + 1) * 2 > g->slot_count) {
        if (!grow_slots(g) && !g->slots)
            return cps[0];
    }

    uint32_t entry_idx;
    if (g->free_head) {
        entry_idx = g->free_head - 1;
        g->free_head = g->entries[entry_idx].next_free;
    } else {
        if (g->entries_count >= GSTORE_MAX_ENTRIES)
            return cps[0];
        if (g->entries_count == g->entries_cap) {
            size_t new_cap = g->entries_cap ? (size_t)g->entries_cap * 2 : 128;
            if (new_cap > GSTORE_MAX_ENTRIES)
                new_cap = GSTORE_MAX_ENTRIES;
            struct gentry *next = realloc(g->entries, new_cap * sizeof(*next));
            if (!next)
                return cps[0];
            g->entries = next;
            g->entries_cap = (uint32_t)new_cap;
        }
        entry_idx = g->entries_count++;
    }

    struct gentry *e = &g->entries[entry_idx];
    e->cp0 = cps[0];
    e->cp1 = cps[1];
    e->len = (uint16_t)n;
    e->mark = false;
    e->allocated = true;
    e->offset = 0;

    if (n > 2) {
        size_t extra = (size_t)(n - 2);
        if (g->arena_len + extra > g->arena_cap) {
            size_t new_cap = wsap_vtcore_grow_cap(g->arena_cap ? g->arena_cap * 2 : 512,
                                           g->arena_len + extra);
            uint32_t *next = realloc(g->arena, new_cap * sizeof(*next));
            if (!next) {
                e->allocated = false;
                e->next_free = g->free_head;
                g->free_head = entry_idx + 1;
                return cps[0];
            }
            g->arena = next;
            g->arena_cap = new_cap;
        }
        uint32_t off = (uint32_t)g->arena_len;
        memcpy(&g->arena[off], &cps[2], extra * sizeof(*cps));
        g->arena_len += extra;
        e->offset = off;
    }

    gstore_insert_slot(g, h32, entry_idx);
    g->live_count++;
    return WSAP_VTCORE_GRAPHEME_HANDLE_BASE + entry_idx;
}
