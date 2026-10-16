#ifndef FONT_GLYPH_INTERNAL_H
#define FONT_GLYPH_INTERNAL_H

#include "font/glyph_cache.h"
#include "font/font.h"
#include "font/backend.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define LATIN_CACHE_SIZE 4096
#define CJK_CACHE_SIZE   4096
#define RUN_CACHE_SETS_BITS 11
#define RUN_CACHE_WAYS   4
#define RUN_CACHE_SIZE   ((1 << RUN_CACHE_SETS_BITS) * RUN_CACHE_WAYS)
#define RUN_CACHE_MAX    FONT_RUN_MAX
#define TILE_CACHE_BITS  11
#define TILE_CACHE_SIZE  (1 << TILE_CACHE_BITS)

#define GLYPH_KEY_CP_BITS 21
#define GLYPH_KEY_CP_MAX  0x10FFFFu

_Static_assert(((uint64_t)FONT_STYLE_WIDE << GLYPH_KEY_CP_BITS) <= UINT32_MAX,
               "glyph_key() style bits overflow 32 bits");

static inline uint32_t glyph_key(uint32_t cp, uint16_t style)
{
    return cp | ((uint32_t)style << GLYPH_KEY_CP_BITS);
}

static inline uint16_t glyph_style(uint16_t style)
{
    return style & (FONT_STYLE_BOLD | FONT_STYLE_ITALIC | FONT_STYLE_EMOJI |
                    FONT_STYLE_TEXT | FONT_STYLE_WIDE);
}

struct bufhead {
    alignas(max_align_t) struct bufhead *next;
    uint32_t cap;
    uint32_t key;
    uint8_t kind;
};
enum { BK_GLYPH_ARR, BK_PIX, BK_RUN_DATA };

extern _Atomic bool cache_parallel;

void *buf_new(uint8_t kind, uint32_t cap, size_t size);
void buf_retire(void *p);
void buf_retire_run(void *p);

struct cache_slot {
    _Atomic(void *) payload;
};

static inline void *cache_slot_lookup(struct cache_slot *slot, uint32_t key)
{
    void *p = atomic_load_explicit(&slot->payload, memory_order_acquire);
    if (__builtin_expect(p && ((const struct bufhead *)p - 1)->key == key, 1))
        return p;
    return NULL;
}

struct slot_fill_ops {
    void *(*produce)(uint32_t cp, uint16_t flags, void *old, void *ctx);
    void *(*slow)(uint32_t cp, uint16_t flags, void *ctx);
    void *ctx;
};

void *cache_slot_fill(struct cache_slot *slot, uint32_t key, uint32_t cp, uint16_t flags,
                      const struct slot_fill_ops *ops);

struct run_data {
    uint64_t hash;
    struct glyph *cells;
    uint32_t *tiles;
    uint32_t codepoints[RUN_CACHE_MAX];
    uint32_t tile_cap;
    uint32_t script;
    enum font_run_result result;
    uint16_t flags;
    uint8_t count;
    uint8_t core, core_len;
};

struct run_slot {
    _Atomic(struct run_data *) data;
    _Atomic uint64_t stamp;
};

struct font_instance {
    struct font_metrics m;
    int size_pt;
    uint32_t scale_120;
    uint32_t id;
    int refs;
    struct font_instance *next;

    font_handle_t *face;
    _Atomic uint64_t built_gen;
    _Atomic uint32_t built_fg, built_bg;

    struct cache_slot ascii[128];
    struct cache_slot ascii_italic[128];
    struct cache_slot latin[LATIN_CACHE_SIZE];
    _Atomic(struct cache_slot *) cjk;
    uint32_t *ascii_tiles[128];
    struct run_slot runs[RUN_CACHE_SIZE];
    _Atomic(struct font_layout *) layouts[FONT_VARIANT_COUNT];
    struct cache_slot tiles[TILE_CACHE_SIZE];
    _Atomic uint64_t run_stamp;
};

static inline size_t tile_pixels(const struct font_instance *fi)
{
    return (size_t)fi->m.cell_w * (size_t)fi->m.cell_h;
}

void build_default_lut(void);
void glyph_tile_set_colors(uint32_t fg, uint32_t bg);
uint32_t glyph_tile_fg(void);
uint32_t glyph_tile_bg(void);
bool glyph_tile_eligible(const struct font_instance *fi, const struct glyph *g);
void fill_tile(const struct font_instance *fi, uint32_t *px, const struct glyph *g);
bool fill_run_tiles(const struct font_instance *fi, uint32_t *tiles,
                    const struct glyph *cells, int count);
void prime_ascii_tile(struct font_instance *fi, uint32_t c, const struct glyph *g);
void tile_worker_cleanup(void);

void run_worker_cleanup(void);
void instance_drop_layouts(struct font_instance *fi);

void release_glyphs(struct glyph *gs, int n);
const struct glyph *glyph_scratch_fill(const struct font_instance *fi, uint32_t cp, uint16_t style);

#endif
