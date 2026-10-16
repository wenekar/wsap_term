#define _GNU_SOURCE
#include "font/glyph/internal.h"
#include "font/glyph/layout.h"
#include "util/hash.h"

#include <stdlib.h>
#include <string.h>

/*
 * Shaping of terminal text.
 *
 * glyph_cache_shape_segment() takes a segment: cells of one style and one pair of
 * colors. It splits the segment into runs of one script. In a simple
 * script it shapes only windows: each run of input code points with the
 * reach of the font's lookups around it (see font/glyph/layout.h). Code points
 * in the reach are replaced by their stand-ins, so windows that differ
 * only in context the font cannot tell apart share one cache entry. A
 * complex script, or a font the analysis cannot read, shapes the run in
 * full.
 *
 * What a cell shows depends only on the text of its segment. It does not
 * depend on earlier frames or on how fast the text arrived.
 */

#define WHOLE_RUN_CHUNK 16

static __thread struct glyph priv_run_cells[RUN_CACHE_MAX];
/* Per segment: each code point's stand-in and role. */
static __thread uint32_t *seg_canon;
static __thread uint8_t *seg_roles;
static __thread int seg_capacity;

struct run_hit {
    enum font_run_result result;
    const struct glyph *cells;
    const uint32_t *tiles;
    uint64_t id;
};

static struct glyph *run_build_cached_payload(struct font_instance *fi, int count,
                                              uint32_t **out_tiles, uint32_t *out_tile_cap)
{
    struct glyph *produced = buf_new(BK_GLYPH_ARR, count,
                                     (size_t)count * sizeof(*produced));
    if (!produced) {
        *out_tiles = NULL;
        *out_tile_cap = 0;
        return NULL;
    }
    memcpy(produced, priv_run_cells, (size_t)count * sizeof(*produced));
    memset(priv_run_cells, 0, (size_t)count * sizeof(*priv_run_cells));

    size_t n_pixels = (size_t)count * tile_pixels(fi);
    uint32_t *tiles = buf_new(BK_PIX, n_pixels, n_pixels * sizeof(*tiles));
    uint32_t tile_cap = tiles ? (uint32_t)n_pixels : 0;
    if (tiles && !fill_run_tiles(fi, tiles, produced, count)) {
        buf_retire(tiles);
        tiles = NULL;
        tile_cap = 0;
    }
    *out_tiles = tiles;
    *out_tile_cap = tile_cap;
    return produced;
}

static enum font_run_result run_shape(struct font_instance *fi, const struct font_window *w)
{
    release_glyphs(priv_run_cells, w->core_len);
    font_bind_thread(fi);
    const struct font_backend *be = FONT_BACKEND;
    if (!fi || !fi->face || !be->shape_run)
        return FONT_RUN_UNAVAILABLE;
    return be->shape_run(fi->face, w, &fi->m, priv_run_cells);
}

static struct run_hit run_shape_and_cache(struct font_instance *fi,
                                          const struct font_window *w, uint64_t hash,
                                          struct run_slot *victim,
                                          struct run_data *victim_d)
{
    enum font_run_result result = run_shape(fi, w);
    for (int i = 0; result == FONT_RUN_SHAPED && i < w->core_len; i++)
        if (priv_run_cells[i].has_color)
            result = FONT_RUN_UNAVAILABLE;

    struct glyph *produced = NULL;
    uint32_t *tiles = NULL;
    uint32_t tile_cap = 0;

    /* Cache every result. A plain or unavailable window still costs a
     * scan and a shape to derive, and most windows are plain. Only a
     * shaped window carries cells and tiles. */
    struct run_data *new_d = buf_new(BK_RUN_DATA, 1, sizeof(*new_d));
    if (new_d && result == FONT_RUN_SHAPED) {
        produced = run_build_cached_payload(fi, w->core_len, &tiles, &tile_cap);
        if (!produced) {
            buf_retire(new_d);
            new_d = NULL;
        }
    }

    if (new_d) {
        *new_d = (struct run_data){
            .hash = hash,
            .cells = produced,
            .tiles = tiles,
            .tile_cap = tile_cap,
            .script = w->script,
            .result = result,
            .flags = w->style,
            .count = (uint8_t)w->len,
            .core = (uint8_t)w->core,
            .core_len = (uint8_t)w->core_len,
        };
        memcpy(new_d->codepoints, w->cps, (size_t)w->len * sizeof(*w->cps));

        struct run_data *expected = victim_d;
        if (atomic_compare_exchange_strong_explicit(&victim->data, &expected, new_d,
                memory_order_release, memory_order_acquire)) {
            atomic_store_explicit(&victim->stamp,
                atomic_fetch_add_explicit(&fi->run_stamp, 1, memory_order_relaxed) + 1,
                memory_order_relaxed);
            /* The evicted entry was published, so a worker may still be
             * reading its cells and tiles. Defer its free to the end of the
             * frame when other threads can read it. */
            if (victim_d) {
                if (atomic_load_explicit(&cache_parallel, memory_order_relaxed))
                    buf_retire_run(victim_d);
                else
                    buf_retire(victim_d);
            }
        } else {
            buf_retire(new_d);
        }
    }

    return (struct run_hit){
        .result = result,
        .cells = produced ? produced : priv_run_cells,
        .tiles = tiles,
        .id = hash,
    };
}

/* Shapes a window through the run cache. The cells stay valid until the
 * next lookup on this thread. */
static struct run_hit run_fetch(struct font_instance *fi, const struct font_window *w,
                                int *dead_until)
{
    if (dead_until) *dead_until = -1;
    if (w->len < 1 || w->len > RUN_CACHE_MAX)
        return (struct run_hit){ .result = FONT_RUN_UNAVAILABLE };
    uint16_t style = w->style & FONT_STYLE_VARIANT_MASK;
    const struct font_backend *be = FONT_BACKEND;
    uint64_t hash;
    int dead_at = -1;
    if (be->scan_run) {
        be->scan_run(w->cps, w->len, style, &hash, &dead_at);
    } else {
        /* Not reached now. Both backends set scan_run. */
        hash = hash64_step(HASH64_BASIS, style);
        for (int i = 0; i < w->len; i++)
            hash = hash64_step(hash, w->cps[i]);
    }
    if (dead_at >= 0) {
        if (dead_until) *dead_until = dead_at;
        return (struct run_hit){ .result = FONT_RUN_UNAVAILABLE };
    }
    hash = hash64_finish(hash64_step(hash64_step(hash,
        (uint32_t)w->core | (uint32_t)w->core_len << 8), w->script));
    uint32_t set_idx = (uint32_t)((hash * UINT64_C(0x9E3779B97F4A7C15)) >>
                                  (64 - RUN_CACHE_SETS_BITS));
    struct run_slot *set = &fi->runs[(size_t)set_idx * RUN_CACHE_WAYS];

    struct run_data *d[RUN_CACHE_WAYS];
    for (int i = 0; i < RUN_CACHE_WAYS; i++)
        d[i] = atomic_load_explicit(&set[i].data, memory_order_acquire);

    for (int i = 0; i < RUN_CACHE_WAYS; i++) {
        struct run_data *rd = d[i];
        if (rd && rd->hash == hash && rd->flags == style && rd->count == w->len &&
            rd->core == w->core && rd->core_len == w->core_len &&
            rd->script == w->script &&
            !memcmp(rd->codepoints, w->cps, (size_t)w->len * sizeof(*w->cps))) {
            atomic_store_explicit(&set[i].stamp,
                atomic_load_explicit(&fi->run_stamp, memory_order_relaxed) + 1,
                memory_order_relaxed);
            return (struct run_hit){
                .result = rd->result, .cells = rd->cells, .tiles = rd->tiles, .id = hash,
            };
        }
    }

    struct run_slot *victim = &set[0];
    struct run_data *victim_d = d[0];
    uint64_t victim_stamp = victim_d
        ? atomic_load_explicit(&set[0].stamp, memory_order_relaxed) : 0;
    if (victim_d) {
        for (int i = 1; i < RUN_CACHE_WAYS; i++) {
            if (!d[i]) {
                victim = &set[i];
                victim_d = NULL;
                break;
            }
            uint64_t st = atomic_load_explicit(&set[i].stamp, memory_order_relaxed);
            if (st < victim_stamp) {
                victim = &set[i];
                victim_d = d[i];
                victim_stamp = st;
            }
        }
    }
    struct font_window key = *w;
    key.style = style;
    return run_shape_and_cache(fi, &key, hash, victim, victim_d);
}

static const struct font_layout *instance_layout(struct font_instance *fi, uint16_t style)
{
    enum font_variant variant = font_variant_of(style);
    struct font_layout *l = atomic_load_explicit(&fi->layouts[variant], memory_order_acquire);
    if (__builtin_expect(l != NULL, 1))
        return l;
    font_bind_thread(fi);
    const struct font_backend *be = FONT_BACKEND;
    hb_face_t *face = fi->face && be->layout_face ? be->layout_face(fi->face, style) : NULL;
    l = font_layout_build(face);
    hb_face_destroy(face);
    if (!l)
        return NULL;
    struct font_layout *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&fi->layouts[variant], &expected, l,
            memory_order_acq_rel, memory_order_acquire)) {
        font_layout_free(l);
        l = expected;
    }
    return l;
}

void instance_drop_layouts(struct font_instance *fi)
{
    for (int i = 0; i < FONT_VARIANT_COUNT; i++)
        font_layout_free(atomic_exchange_explicit(&fi->layouts[i], NULL,
                                                  memory_order_acq_rel));
}

struct segment {
    struct font_instance *fi;
    const struct font_layout *layout;
    const uint32_t *cps;
    /* The stand-in of each code point: the code point itself when it is
     * an input or complex. */
    const uint32_t *canon;
    const uint8_t *roles;
    uint16_t style;
    uint32_t script;
    glyph_span_fn emit;
    void *ctx;
};

static void emit_span(const struct segment *s, const struct run_hit *hit,
                      int core_start, int start, int count)
{
    int offset = start - core_start;
    const struct glyph_span span = {
        .start = start,
        .count = count,
        .cells = hit->cells + offset,
        .tiles = hit->tiles ? hit->tiles + (size_t)offset * tile_pixels(s->fi) : NULL,
        .id = hash64_finish(hit->id ^ ((uint64_t)offset << 48)),
    };
    s->emit(s->ctx, &span);
}

/* Shapes [a, b) in full, WHOLE_RUN_CHUNK cells at a time. */
static void shape_whole(const struct segment *s, int a, int b)
{
    for (int c = a; c + 1 < b;) {
        int n = b - c < WHOLE_RUN_CHUNK ? b - c : WHOLE_RUN_CHUNK;
        const struct font_window w = {
            .cps = s->cps + c, .len = n, .core_len = n,
            .style = s->style, .script = s->script,
        };
        int dead = -1;
        struct run_hit hit = run_fetch(s->fi, &w, &dead);
        if (hit.result == FONT_RUN_SHAPED)
            emit_span(s, &hit, c, c, n);
        c += dead >= 0 ? dead + 1 : n;
    }
}

static bool is_input(const struct segment *s, int i)
{
    return s->roles[i] == LAYOUT_INPUT;
}

/* Shapes the window [ws, we) for its core [cs, ce), and emits the input
 * runs of the core. */
static void shape_window(const struct segment *s, int ws, int we, int cs, int ce)
{
    while (cs < ce && !is_input(s, cs)) cs++;
    while (ce > cs && !is_input(s, ce - 1)) ce--;
    if (cs >= ce || we - ws < 2)
        return;
    const uint32_t *canon = s->canon + ws;
    uint32_t top = 0;
    for (int t = 0; t < we - ws; t++)
        top |= canon[t];
    /* Context never changes, so the canonical window changes exactly when
     * the real one does. */
    if (top < LAYOUT_PAIR_CPS && !font_layout_may_change(s->layout, canon, we - ws))
        return;
    const struct font_window w = {
        .cps = canon, .len = we - ws, .core = cs - ws, .core_len = ce - cs,
        .style = s->style, .script = s->script,
    };
    struct run_hit hit = run_fetch(s->fi, &w, NULL);
    if (hit.result != FONT_RUN_SHAPED)
        return;
    for (int t = cs; t < ce;) {
        if (!is_input(s, t)) {
            t++;
            continue;
        }
        int end = t;
        while (end < ce && is_input(s, end)) end++;
        emit_span(s, &hit, cs, t, end - t);
        t = end;
    }
}

/* Shapes the windows of [a, b), a run of one simple script with no
 * complex code point. */
static void shape_islands(const struct segment *s, const struct font_layout *l,
                          int a, int b)
{
    int back = l->reach_back, ahead = l->reach_ahead;
    int j = a;
    for (;;) {
        while (j < b && !is_input(s, j)) j++;
        if (j >= b)
            return;
        int first = j, last = j;
        while (last < b && is_input(s, last)) last++;
        int ws = first - back > a ? first - back : a;
        int we = last + ahead < b ? last + ahead : b;
        /* Take in every input run within reach of the window. */
        for (int q = last; q < we;) {
            if (!is_input(s, q)) {
                q++;
                continue;
            }
            while (q < b && is_input(s, q)) q++;
            last = q;
            we = last + ahead < b ? last + ahead : b;
        }
        if (we - ws <= RUN_CACHE_MAX) {
            shape_window(s, ws, we, first, last);
        } else {
            int step = RUN_CACHE_MAX - back - ahead;
            for (int cs = first; cs < last; cs += step) {
                int ce = cs + step < last ? cs + step : last;
                shape_window(s, cs - back > ws ? cs - back : ws,
                             ce + ahead < we ? ce + ahead : we, cs, ce);
            }
        }
        j = we;
    }
}

static hb_script_t cp_script(uint32_t cp)
{
    if (cp < 128)
        return (cp | 0x20) - 'a' < 26 ? HB_SCRIPT_LATIN : HB_SCRIPT_COMMON;
    return hb_unicode_script(hb_unicode_funcs_get_default(), cp);
}

static bool script_strong(hb_script_t s)
{
    return s != HB_SCRIPT_COMMON && s != HB_SCRIPT_INHERITED && s != HB_SCRIPT_UNKNOWN;
}

static bool ensure_segment(int count)
{
    if (count <= seg_capacity)
        return true;
    uint32_t *canon = realloc(seg_canon, (size_t)count * sizeof(*canon));
    if (canon)
        seg_canon = canon;
    uint8_t *roles = realloc(seg_roles, (size_t)count);
    if (roles)
        seg_roles = roles;
    if (!canon || !roles)
        return false;
    seg_capacity = count;
    return true;
}

void glyph_cache_shape_segment(struct font_instance *fi, const uint32_t *cps, int count,
                               uint16_t style, glyph_span_fn emit, void *ctx)
{
    if (!fi || !cps || count < 2 || !ensure_segment(count))
        return;
    style &= FONT_STYLE_VARIANT_MASK;
    const struct font_layout *l = instance_layout(fi, style);
    if (!l)
        return;

    /* Most segments hold no input code point, and nothing in them needs
     * shaping. */
    uint32_t *canon = seg_canon;
    uint8_t *roles = seg_roles;
    bool any = l->opaque, letter = false;
    uint32_t top = 0;
    for (int i = 0; i < count; i++) {
        uint32_t cp = cps[i], e = font_layout_entry(l, cp);
        canon[i] = font_layout_entry_cp(e);
        roles[i] = (uint8_t)font_layout_entry_role(e);
        any |= roles[i] >= LAYOUT_INPUT;
        top |= cp;
        letter |= (cp | 0x20) - 'a' < 26;
    }
    if (!any)
        return;

    struct segment s = {
        .fi = fi, .layout = l, .cps = cps, .canon = canon, .roles = roles,
        .style = style, .emit = emit, .ctx = ctx,
    };
    /* ASCII text is one run: Latin with a letter, else Common. */
    if (top < 128 && !l->opaque) {
        s.script = letter ? HB_SCRIPT_LATIN : HB_SCRIPT_COMMON;
        shape_islands(&s, l, 0, count);
        return;
    }
    for (int i = 0; i < count;) {
        hb_script_t script = HB_SCRIPT_COMMON;
        int e = i;
        for (; e < count; e++) {
            hb_script_t sc = cp_script(cps[e]);
            if (!script_strong(sc))
                continue;
            if (script == HB_SCRIPT_COMMON)
                script = sc;
            else if (sc != script)
                break;
        }
        s.script = script;
        if (l->opaque || !font_layout_script_simple(script)) {
            shape_whole(&s, i, e);
        } else {
            for (int a = i; a < e;) {
                int b = a;
                while (b < e && roles[b] != LAYOUT_COMPLEX) b++;
                shape_islands(&s, l, a, b);
                int c = b;
                while (c < e && roles[c] == LAYOUT_COMPLEX) c++;
                /* Code points the face does not map shape with a fallback face. */
                if (c - b > 1)
                    shape_whole(&s, b, c);
                a = c;
            }
        }
        i = e;
    }
}

void run_worker_cleanup(void)
{
    release_glyphs(priv_run_cells, RUN_CACHE_MAX);
    free(seg_canon);
    free(seg_roles);
    seg_canon = NULL;
    seg_roles = NULL;
    seg_capacity = 0;
}
