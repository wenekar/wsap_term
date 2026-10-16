#include "font/glyph/intern.h"
#include "util/hash.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_CLUSTERS 65536
#define INDEX_INIT_SIZE (2 * MAX_CLUSTERS)

/* Each entry stores the cluster hash and its first two code points.
 * Clusters with two code points do not use the arena. */
struct entry {
    uint64_t hash;
    uint32_t offset;
    uint32_t len;
    uint32_t cp0;
    uint32_t cp1;
};

_Static_assert(sizeof(struct entry) == 24,
               "struct entry must stay 24 bytes");

static uint32_t *arena;
static size_t arena_len, arena_cap;
static struct entry *entries;
static size_t num_entries, entries_cap;
static uint32_t *index_table;
static size_t index_size;

/* The table below is process-global, and the paint pool interns clusters from
 * several worker threads at once. Interning reallocs the arena and frees the
 * index table, so a concurrent reader in glyph_intern_expand() would read freed
 * memory. Every entry point that touches this state holds this lock. */
static pthread_mutex_t grapheme_mu = PTHREAD_MUTEX_INITIALIZER;

static void grapheme_reset(void)
{
    arena_len = 0;
    num_entries = 0;
    if (index_table)
        memset(index_table, 0, index_size * sizeof(*index_table));
}

static uint64_t hash_cps(const uint32_t *cps, int len)
{
    uint64_t h = HASH64_BASIS;
    for (int i = 0; i < len; i++)
        h = hash64_step(h, cps[i]);
    return hash64_finish(h);
}

static inline bool entry_matches(const struct entry *e, const uint32_t *cps, int len, uint64_t h)
{
    if (e->hash != h || (int)e->len != len)
        return false;
    if (e->cp0 != cps[0] || e->cp1 != cps[1])
        return false;
    if (len == 2)
        return true;
    return !memcmp(&arena[e->offset], &cps[2], (size_t)(len - 2) * sizeof(*cps));
}

static bool grow(void **buf, size_t *cap, size_t need, size_t item)
{
    if (need <= *cap)
        return true;
    size_t want = *cap ? *cap * 2 : 256;
    while (want < need)
        want *= 2;
    void *next = realloc(*buf, want * item);
    if (!next)
        return false;
    *buf = next;
    *cap = want;
    return true;
}

static uint32_t glyph_intern_locked(const uint32_t *cps, int len)
{
    if (len <= 0)
        return 0;
    if (len == 1)
        return cps[0];
    if (len > WSAP_VTCORE_GRAPHEME_MAX_LEN)
        len = WSAP_VTCORE_GRAPHEME_MAX_LEN;

    if (num_entries >= MAX_CLUSTERS)
        grapheme_reset();

    if (!index_table) {
        index_size = INDEX_INIT_SIZE;
        index_table = calloc(index_size, sizeof(*index_table));
        if (!index_table)
            return cps[0];
    }

    uint64_t h = hash_cps(cps, len);
    size_t slot;
    for (;;) {
        slot = h & (index_size - 1);
        uint32_t idx;
        while ((idx = index_table[slot]) != 0) {
            uint32_t entry_idx = idx - 1;
            const struct entry *e = &entries[entry_idx];
            if (entry_matches(e, cps, len, h))
                return WSAP_VTCORE_GRAPHEME_HANDLE_BASE + entry_idx;
            slot = (slot + 1) & (index_size - 1);
        }
        if ((num_entries + 1) * 2 < index_size)
            break;
        size_t want = index_size * 2;
        uint32_t *next = calloc(want, sizeof(*next));
        if (!next)
            return cps[0];
        for (size_t s = 0; s < index_size; s++) {
            uint32_t eidx = index_table[s];
            if (!eidx) continue;
            const struct entry *e = &entries[eidx - 1];
            size_t ns = e->hash & (want - 1);
            while (next[ns])
                ns = (ns + 1) & (want - 1);
            next[ns] = eidx;
        }
        free(index_table);
        index_table = next;
        index_size = want;
    }

    void *entries_buf = entries;
    if (!grow(&entries_buf, &entries_cap, num_entries + 1, sizeof(*entries)))
        return cps[0];
    entries = entries_buf;

    uint32_t offset = 0;
    if (len > 2) {
        size_t extra = (size_t)(len - 2);
        void *arena_buf = arena;
        if (!grow(&arena_buf, &arena_cap, arena_len + extra, sizeof(*arena)))
            return cps[0];
        arena = arena_buf;
        memcpy(&arena[arena_len], &cps[2], extra * sizeof(*cps));
        offset = (uint32_t)arena_len;
        arena_len += extra;
    }

    entries[num_entries] = (struct entry){
        .hash = h,
        .offset = offset,
        .len = (uint32_t)len,
        .cp0 = cps[0],
        .cp1 = cps[1],
    };
    index_table[slot] = (uint32_t)(num_entries + 1);
    return WSAP_VTCORE_GRAPHEME_HANDLE_BASE + (uint32_t)num_entries++;
}

uint32_t glyph_intern(const uint32_t *cps, int len)
{
    pthread_mutex_lock(&grapheme_mu);
    uint32_t r = glyph_intern_locked(cps, len);
    pthread_mutex_unlock(&grapheme_mu);
    return r;
}

static int glyph_intern_expand_locked(uint32_t c, uint32_t *out, size_t out_cap)
{
    if (!out_cap)
        return 0;
    if (!wsap_vtcore_grapheme_is_handle(c)) {
        out[0] = c;
        return 1;
    }
    uint32_t i = c - WSAP_VTCORE_GRAPHEME_HANDLE_BASE;
    if (i >= num_entries) {
        out[0] = 0xFFFD;
        return 1;
    }
    const struct entry *e = &entries[i];
    if (e->len > out_cap) {
        out[0] = 0xFFFD;
        return 1;
    }
    out[0] = e->cp0;
    if (e->len > 1)
        out[1] = e->cp1;
    if (e->len > 2)
        memcpy(&out[2], &arena[e->offset], (size_t)(e->len - 2) * sizeof(*out));
    return (int)e->len;
}

int glyph_intern_expand(uint32_t c, uint32_t *out, size_t out_cap)
{
    pthread_mutex_lock(&grapheme_mu);
    int r = glyph_intern_expand_locked(c, out, out_cap);
    pthread_mutex_unlock(&grapheme_mu);
    return r;
}
