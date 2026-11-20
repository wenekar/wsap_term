#define _GNU_SOURCE
#include "font/glyph/internal.h"
#include "font/internal.h"
#include "util/hash.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Atomic bool cache_parallel;

void glyph_cache_set_parallel(bool on)
{
    atomic_store_explicit(&cache_parallel, on, memory_order_release);
}

static __thread struct bufhead *tls_pool;
static __thread struct bufhead *tls_defer;

void *buf_new(uint8_t kind, uint32_t cap, size_t size)
{
    if (kind > 7 || cap > ((1u << 29) - 1))
        return NULL;
    for (struct bufhead **pp = &tls_pool; *pp; pp = &(*pp)->next) {
        if ((*pp)->kind == kind && (*pp)->cap >= cap) {
            struct bufhead *h = *pp;
            *pp = h->next;
            return h + 1;
        }
    }
    struct bufhead *h = malloc(sizeof(*h) + size);
    if (!h)
        return NULL;
    h->kind = kind & 7u;
    h->cap = cap & ((1u << 29) - 1);
    return h + 1;
}

static void bufhead_teardown(struct bufhead *h)
{
    if (h->kind == BK_GLYPH_ARR) {
        for (uint32_t i = 0; i < h->cap; i++)
            glyph_release(&((struct glyph *)(h + 1))[i]);
    } else if (h->kind == BK_RUN_DATA) {
        struct run_data *rd = (struct run_data *)(h + 1);
        buf_retire(rd->cells);
        buf_retire(rd->tiles);
        rd->cells = NULL;
        rd->tiles = NULL;
    }
}

void buf_retire(void *p)
{
    if (!p)
        return;
    struct bufhead *h = (struct bufhead *)p - 1;
    if (atomic_load_explicit(&cache_parallel, memory_order_relaxed)) {
        h->next = tls_defer;
        tls_defer = h;
        return;
    }
    bufhead_teardown(h);
    h->next = tls_pool;
    tls_pool = h;
}

void glyph_cache_flush_deferred(void)
{
    while (tls_defer) {
        struct bufhead *h = tls_defer;
        tls_defer = h->next;
        bufhead_teardown(h);
        h->next = tls_pool;
        tls_pool = h;
    }
}

static pthread_mutex_t retire_mu = PTHREAD_MUTEX_INITIALIZER;
static struct bufhead *retire_pending;

void buf_retire_run(void *p)
{
    if (!p)
        return;
    struct bufhead *h = (struct bufhead *)p - 1;
    pthread_mutex_lock(&retire_mu);
    h->next = retire_pending;
    retire_pending = h;
    pthread_mutex_unlock(&retire_mu);
}

void glyph_cache_flush_frame(void)
{
    pthread_mutex_lock(&retire_mu);
    struct bufhead *list = retire_pending;
    retire_pending = NULL;
    pthread_mutex_unlock(&retire_mu);
    while (list) {
        struct bufhead *h = list;
        list = h->next;
        bufhead_teardown(h);
        h->next = tls_pool;
        tls_pool = h;
    }
    glyph_cache_flush_deferred();
}

void release_glyphs(struct glyph *gs, int n)
{
    for (int i = 0; i < n; i++) {
        glyph_release(&gs[i]);
        gs[i] = (struct glyph){0};
    }
}

static __thread struct glyph glyph_scratch;

const struct glyph *glyph_scratch_fill(const struct font_instance *fi, uint32_t cp,
                                       uint16_t style)
{
    release_glyphs(&glyph_scratch, 1);
    font_instance_render_glyph(fi, cp, style, &glyph_scratch);
    return &glyph_scratch;
}

void *cache_slot_fill(struct cache_slot *slot, uint32_t key, uint32_t cp, uint16_t flags,
                      const struct slot_fill_ops *ops)
{
    void *old = atomic_load_explicit(&cache_parallel, memory_order_relaxed)
        ? NULL : atomic_load_explicit(&slot->payload, memory_order_relaxed);
    void *p = ops->produce(cp, flags, old, ops->ctx);
    if (!p)
        return ops->slow(cp, flags, ops->ctx);
    ((struct bufhead *)p - 1)->key = key;
    void *prev = atomic_exchange_explicit(&slot->payload, p, memory_order_acq_rel);
    if (prev != p)
        buf_retire(prev);
    return p;
}

/* The render request of one cache slot fill. */
struct glyph_fill_ctx {
    struct font_instance *fi;
    uint8_t phase;
};

/* The fill context carries what the slot API does not pass: the instance and
 * the subpixel phase. The style arrives as the `flags` argument. */
static inline void glyph_fill_args(uint16_t flags, void *ctx, uint8_t *phase,
                                   struct font_instance **fi)
{
    const struct glyph_fill_ctx *c = ctx;
    *phase = c ? c->phase : 0;
    *fi = c ? c->fi : NULL;
    (void)flags;
}

static void *glyph_slot_produce(uint32_t cp, uint16_t flags, void *old, void *ctx)
{
    struct font_instance *fi;
    uint8_t phase;
    glyph_fill_args(flags, ctx, &phase, &fi);
    struct glyph *g = old;
    if (!g) {
        g = buf_new(BK_GLYPH_ARR, 1, sizeof(*g));
        if (!g)
            return NULL;
        *g = (struct glyph){0};
    }
    font_instance_render_glyph_at(fi, cp, flags, phase, g);
    return g;
}

static void *glyph_slot_slow(uint32_t cp, uint16_t flags, void *ctx)
{
    struct font_instance *fi;
    uint8_t phase;
    glyph_fill_args(flags, ctx, &phase, &fi);
    if (!phase)
        return (void *)glyph_scratch_fill(fi, cp, flags);
    release_glyphs(&glyph_scratch, 1);
    font_instance_render_glyph_at(fi, cp, flags, phase, &glyph_scratch);
    return &glyph_scratch;
}

static inline const struct glyph *slot_fill(struct font_instance *fi, struct cache_slot *slot,
                                            uint32_t cp, uint16_t flags, uint8_t phase)
{
    uint32_t key = glyph_key_at(cp, flags, phase);
    void *p = cache_slot_lookup(slot, key);
    if (__builtin_expect(p != NULL, 1))
        return (const struct glyph *)p;

    struct glyph_fill_ctx ctx = { .fi = fi, .phase = phase };
    const struct slot_fill_ops ops = { glyph_slot_produce, glyph_slot_slow, &ctx };
    return cache_slot_fill(slot, key, cp, flags, &ops);
}

struct cache_slot *slot_cache_ensure(_Atomic(struct cache_slot *) *ptr, int count)
{
    struct cache_slot *cache = atomic_load_explicit(ptr, memory_order_acquire);
    if (cache)
        return cache;
    cache = calloc((size_t)count, sizeof(*cache));
    if (!cache)
        return NULL;
    struct cache_slot *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(ptr, &expected, cache,
            memory_order_release, memory_order_acquire)) {
        free(cache);
        cache = expected;
    }
    return cache;
}

/* Returns the borrowed glyph of code point `cp` at subpixel `phase`. */
const struct glyph *glyph_cache_fetch_at(struct font_instance *fi, uint32_t cp,
                                         uint16_t style, uint8_t phase)
{
    style = instance_style(fi, style);
    if (__builtin_expect(cp > GLYPH_KEY_CP_MAX, 0))
        return glyph_scratch_fill(fi, cp, style);

    struct cache_slot *slot;
    if (__builtin_expect(phase == 0 && cp < 128 && !style, 1))
        slot = &fi->ascii[cp];
    else if (phase == 0 && cp < 128 && style == FONT_STYLE_ITALIC)
        slot = &fi->ascii_italic[cp];
    else if (__builtin_expect(phase == 0 && cp < LATIN_CACHE_SIZE && !style, 1)) {
        struct cache_slot *latin = slot_cache_ensure(&fi->latin, LATIN_CACHE_SIZE);
        if (!latin)
            return glyph_scratch_fill(fi, cp, style);
        slot = &latin[cp];
    } else {
        struct cache_slot *cjk = slot_cache_ensure(&fi->cjk, CJK_CACHE_SIZE);
        if (!cjk)
            return glyph_scratch_fill(fi, cp, style);
        slot = &cjk[(glyph_key_at(cp, style, phase) * KNUTH_HASH_MULT) >> 20];
    }
    return slot_fill(fi, slot, cp, style, phase);
}

const struct glyph *glyph_cache_fetch(struct font_instance *fi, uint32_t cp,
                                      uint16_t style)
{
    return glyph_cache_fetch_at(fi, cp, style, 0);
}

static struct font_instance *instances;
static struct font_instance *default_instance;
static uint32_t next_instance_id = 1;

_Static_assert(offsetof(struct font_instance, m) == 0,
               "font_instance_metrics() reads the metrics at the start");

void font_bind_thread(const struct font_instance *fi)
{
    if (fi)
        face_activate(&fi->m);
}

static int clamp_size(int size_pt)
{
    return size_pt < FONT_MIN_SIZE_PT ? FONT_MIN_SIZE_PT
         : size_pt > FONT_MAX_SIZE_PT ? FONT_MAX_SIZE_PT : size_pt;
}

static void drop_glyph_slots(struct cache_slot *slots, int n)
{
    for (int i = 0; i < n; i++) {
        struct glyph *g = atomic_load_explicit(&slots[i].payload, memory_order_relaxed);
        if (g) buf_retire(g);
    }
    memset(slots, 0, sizeof(*slots) * (size_t)n);
}

static void drop_all(struct font_instance *fi)
{
    drop_glyph_slots(fi->ascii, 128);
    drop_glyph_slots(fi->ascii_italic, 128);
    struct cache_slot *latin = atomic_load_explicit(&fi->latin, memory_order_acquire);
    if (latin)
        drop_glyph_slots(latin, LATIN_CACHE_SIZE);
    struct cache_slot *cjk = atomic_load_explicit(&fi->cjk, memory_order_acquire);
    if (cjk)
        drop_glyph_slots(cjk, CJK_CACHE_SIZE);
    for (int i = 0; i < RUN_CACHE_SIZE; i++) {
        struct run_data *d = atomic_load_explicit(&fi->runs[i].data, memory_order_relaxed);
        if (d) buf_retire(d);
    }
    struct cache_slot *tiles = atomic_load_explicit(&fi->tiles, memory_order_acquire);
    if (tiles) {
        for (int i = 0; i < TILE_CACHE_SIZE; i++) {
            uint32_t *px = atomic_load_explicit(&tiles[i].payload, memory_order_relaxed);
            if (px) buf_retire(px);
        }
        memset(tiles, 0, sizeof(*tiles) * TILE_CACHE_SIZE);
    }
    memset(fi->runs, 0, sizeof(fi->runs));
    atomic_store_explicit(&fi->run_stamp, 0, memory_order_relaxed);
}

/* Returns the font descriptor that instance `fi` was made from. */
static struct font_desc instance_desc(const struct font_instance *fi)
{
    return (struct font_desc){
        .size = { .pt = fi->size_pt, .scale_120 = fi->scale_120 },
        .weight = fi->weight,
        .slant = fi->slant,
        .proportional = fi->proportional,
    };
}

static void instance_reopen_if_stale(struct font_instance *fi)
{
    if (atomic_load_explicit(&fi->built_gen, memory_order_relaxed) == font_generation())
        return;
    const struct font_backend *be = FONT_BACKEND;
    struct font_desc desc = instance_desc(fi);
    instance_drop_layouts(fi);
    if (fi->face && be->close) {
        be->close(fi->face);
        fi->face = NULL;
    }
    if (be->open)
        fi->face = be->open(&desc);
    if (fi->face && be->measure)
        be->measure(fi->face, &fi->m);
    else
        font_measure_desc(&desc, &fi->m);
}

void glyph_cache_rebuild(struct font_instance *fi)
{
    instance_reopen_if_stale(fi);
    atomic_store_explicit(&fi->built_gen, font_generation(), memory_order_release);
    /* Build the tile tables here, on the main thread. A worker that fills a
     * tile then finds them complete and writes nothing. */
    build_default_lut();
    atomic_store_explicit(&fi->built_fg, glyph_tile_fg(), memory_order_relaxed);
    atomic_store_explicit(&fi->built_bg, glyph_tile_bg(), memory_order_relaxed);
    drop_all(fi);
}

static void instance_refresh(struct font_instance *fi)
{
    if (atomic_load_explicit(&cache_parallel, memory_order_relaxed))
        return;
    if (atomic_load_explicit(&fi->built_gen, memory_order_acquire) != font_generation() ||
        atomic_load_explicit(&fi->built_fg, memory_order_relaxed) != glyph_tile_fg() ||
        atomic_load_explicit(&fi->built_bg, memory_order_relaxed) != glyph_tile_bg())
        glyph_cache_rebuild(fi);
}

void glyph_cache_refresh(struct font_instance *fi)
{
    if (fi && !atomic_load_explicit(&cache_parallel, memory_order_relaxed) &&
        atomic_load_explicit(&fi->built_gen, memory_order_acquire) != font_generation())
        glyph_cache_rebuild(fi);
}

void glyph_cache_begin_frame(struct font_instance *fi, uint32_t default_fg,
                             uint32_t default_bg)
{
    glyph_tile_set_colors(default_fg, default_bg);
    instance_refresh(fi);
}

/* Returns true when instance `fi` was made from descriptor `desc`. */
static bool instance_matches(const struct font_instance *fi, const struct font_desc *desc,
                             int size_pt, uint32_t scale_120)
{
    return fi->size_pt == size_pt && fi->scale_120 == scale_120 &&
           fi->weight == font_desc_weight(desc) && fi->slant == desc->slant &&
           fi->proportional == desc->proportional;
}

struct font_instance *font_instance_acquire(const struct font_desc *desc)
{
    if (!desc)
        return NULL;
    int size_pt = clamp_size(font_size_resolve(desc->size).pt);
    uint32_t scale_120 = desc->size.scale_120 ? desc->size.scale_120 : 120;
    font_apply_desc_family(desc);
    for (struct font_instance *fi = instances; fi; fi = fi->next) {
        if (instance_matches(fi, desc, size_pt, scale_120)) {
            instance_refresh(fi);
            fi->refs++;
            return fi;
        }
    }
    struct font_instance *fi = aligned_alloc(64, (sizeof(*fi) + 63) & ~(size_t)63);
    if (!fi)
        return NULL;
    memset(fi, 0, sizeof(*fi));
    fi->weight = font_desc_weight(desc);
    fi->slant = desc->slant;
    fi->proportional = desc->proportional;
    fi->base_style = font_desc_base_style(desc);
    fi->size_pt = size_pt;
    fi->scale_120 = scale_120;
    struct font_desc made = instance_desc(fi);
    const struct font_backend *be = FONT_BACKEND;
    font_handle_t *face = be->open ? be->open(&made) : NULL;
    if (face) {
        if (!be->measure(face, &fi->m)) {
            be->close(face);
            goto fail;
        }
        fi->face = face;
    } else if (!font_measure_desc(&made, &fi->m)) {
        goto fail;
    }
    atomic_store_explicit(&fi->built_gen, font_generation(), memory_order_relaxed);
    fi->id = next_instance_id++;
    fi->refs = 1;
    fi->next = instances;
    instances = fi;
    glyph_cache_rebuild(fi);
    return fi;
fail:
    free(fi);
    return NULL;
}

struct font_instance *font_instance_ref(struct font_instance *fi)
{
    if (fi)
        fi->refs++;
    return fi;
}

static void instance_free(struct font_instance *fi)
{
    for (struct font_instance **p = &instances; *p; p = &(*p)->next) {
        if (*p == fi) {
            *p = fi->next;
            break;
        }
    }
    drop_all(fi);
    instance_drop_layouts(fi);
    if (fi->face) {
        const struct font_backend *be = FONT_BACKEND;
        if (be->close)
            be->close(fi->face);
        fi->face = NULL;
    }
    free(atomic_load_explicit(&fi->latin, memory_order_relaxed));
    free(atomic_load_explicit(&fi->cjk, memory_order_relaxed));
    free(atomic_load_explicit(&fi->tiles, memory_order_relaxed));
    free(fi);
}

void font_instance_unref(struct font_instance *fi)
{
    if (fi && --fi->refs == 0)
        instance_free(fi);
}

struct font_instance *font_instance_default(void)
{
    if (default_instance)
        instance_refresh(default_instance);
    return default_instance;
}

int font_instance_size_pt(const struct font_instance *fi)
{
    return fi->size_pt;
}

uint32_t font_instance_scale_120(const struct font_instance *fi)
{
    return fi->scale_120;
}

uint64_t font_instance_version(const struct font_instance *fi)
{
    return (uint64_t)fi->id << 32 | (uint32_t)font_generation();
}

bool font_system_init(void)
{
    if (default_instance)
        return true;
    if (!font_backend_init())
        return false;
    const struct font_desc desc = { .size = { .pt = font_configured_size() } };
    default_instance = font_instance_acquire(&desc);
    if (!default_instance) {
        font_backend_shutdown();
        return false;
    }
    return true;
}

void glyph_cache_thread_detach(void)
{
    glyph_cache_flush_deferred();
    while (tls_pool) {
        struct bufhead *h = tls_pool;
        tls_pool = h->next;
        free(h);
    }
    release_glyphs(&glyph_scratch, 1);
    run_worker_cleanup();
    tile_worker_cleanup();
}

void font_system_shutdown(void)
{
    while (instances)
        instance_free(instances);
    default_instance = NULL;
    glyph_cache_thread_detach();
    glyph_cache_flush_frame();
    font_backend_shutdown();
}
