#define _GNU_SOURCE
#include "font/glyph/internal.h"
#include "paint/blend.h"
#include "util/hash.h"

#include <stdlib.h>

static uint32_t default_lut[256];
static struct render_subpx_lut default_subpx_lut;
static _Atomic bool default_lut_ready;
static _Atomic uint32_t default_lut_fg, default_lut_bg;
static _Atomic uint32_t tile_fg = 0xFFFFFFFFu, tile_bg = 0xFF1E1E2Eu;

static __thread uint32_t *tile_scratch;
static __thread size_t tile_scratch_cap;

void glyph_tile_set_colors(uint32_t fg, uint32_t bg)
{
    atomic_store_explicit(&tile_fg, fg, memory_order_relaxed);
    atomic_store_explicit(&tile_bg, bg, memory_order_relaxed);
}

uint32_t glyph_tile_fg(void)
{
    return atomic_load_explicit(&tile_fg, memory_order_relaxed);
}

uint32_t glyph_tile_bg(void)
{
    return atomic_load_explicit(&tile_bg, memory_order_relaxed);
}

void build_default_lut(void)
{
    uint32_t fg = glyph_tile_fg(), bg = glyph_tile_bg();
    if (atomic_load_explicit(&default_lut_ready, memory_order_relaxed) &&
        atomic_load_explicit(&default_lut_fg, memory_order_relaxed) == fg &&
        atomic_load_explicit(&default_lut_bg, memory_order_relaxed) == bg)
        return;
    for (int a = 0; a < 256; a++)
        default_lut[a] = render_blend_px(fg, bg, (uint8_t)a);
    render_build_subpx_lut(&default_subpx_lut, fg, bg);
    atomic_store_explicit(&default_lut_fg, fg, memory_order_relaxed);
    atomic_store_explicit(&default_lut_bg, bg, memory_order_relaxed);
    atomic_store_explicit(&default_lut_ready, true, memory_order_relaxed);
}

bool glyph_tile_eligible(const struct font_instance *fi, const struct glyph *g)
{
    return g && !g->has_color && g->x_offset == 0 &&
           g->width == fi->m.cell_w && g->alpha;
}

void fill_tile(const struct font_instance *fi, uint32_t *px, const struct glyph *g)
{
    int cw = fi->m.cell_w, ch = fi->m.cell_h;
    if (g->has_subpx_ink && g->subpx) {
        uint32_t bg = glyph_tile_bg();
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                uint8_t sr = g->subpx[y][x * 3 + 0];
                uint8_t sg = g->subpx[y][x * 3 + 1];
                uint8_t sb = g->subpx[y][x * 3 + 2];
                px[(size_t)y * cw + x] = render_blend_subpx_lut(&default_subpx_lut, bg, sr, sg, sb);
            }
    } else {
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                px[(size_t)y * cw + x] = default_lut[g->alpha[y][x]];
    }
}

bool fill_run_tiles(const struct font_instance *fi, uint32_t *tiles,
                    const struct glyph *cells, int count)
{
    for (int i = 0; i < count; i++)
        if (!glyph_tile_eligible(fi, &cells[i]))
            return false;
    build_default_lut();
    for (int i = 0; i < count; i++)
        fill_tile(fi, tiles + (size_t)i * tile_pixels(fi), &cells[i]);
    return true;
}

void prime_ascii_tile(struct font_instance *fi, uint32_t c, const struct glyph *g)
{
    if (!glyph_tile_eligible(fi, g))
        return;
    build_default_lut();
    uint32_t *px = malloc(tile_pixels(fi) * sizeof(*px));
    if (!px)
        return;
    fill_tile(fi, px, g);
    fi->ascii_tiles[c] = px;
}

static const struct glyph *glyph_tile_prepare(struct font_instance *fi, uint32_t cp,
                                              uint16_t flags, size_t *out_count)
{
    const struct glyph *g = glyph_cache_fetch(fi, cp, flags);
    if (!glyph_tile_eligible(fi, g))
        return NULL;
    build_default_lut();
    *out_count = tile_pixels(fi);
    return g;
}

static void *tile_slot_produce(uint32_t cp, uint16_t flags, void *old, void *ctx)
{
    size_t count;
    const struct glyph *g = glyph_tile_prepare(ctx, cp, flags, &count);
    if (!g)
        return NULL;
    uint32_t *pixels = old ? old : buf_new(BK_PIX, count, count * sizeof(*pixels));
    if (pixels)
        fill_tile(ctx, pixels, g);
    return pixels;
}

static void *tile_slot_slow(uint32_t cp, uint16_t flags, void *ctx)
{
    size_t count;
    const struct glyph *g = glyph_tile_prepare(ctx, cp, flags, &count);
    if (!g)
        return NULL;
    if (count > tile_scratch_cap) {
        uint32_t *p = realloc(tile_scratch, count * sizeof(*p));
        if (!p)
            return NULL;
        tile_scratch = p;
        tile_scratch_cap = count;
    }
    fill_tile(ctx, tile_scratch, g);
    return tile_scratch;
}

const uint32_t *glyph_cache_fetch_tile(struct font_instance *fi, uint32_t cp,
                                       uint16_t style)
{
    style = glyph_style(style);
    if (__builtin_expect(cp < 128 && !style, 1)) {
        const uint32_t *tile = fi->ascii_tiles[cp];
        if (tile)
            return tile;
    }
    if (__builtin_expect(cp > GLYPH_KEY_CP_MAX, 0))
        return NULL;

    uint32_t key = glyph_key(cp, style);
    uint32_t idx = (cp < TILE_CACHE_SIZE && !style) ? cp
        : (key * KNUTH_HASH_MULT) >> (32 - TILE_CACHE_BITS);
    void *p = cache_slot_lookup(&fi->tiles[idx], key);
    if (__builtin_expect(p != NULL, 1))
        return (const uint32_t *)p;

    const struct slot_fill_ops ops = { tile_slot_produce, tile_slot_slow, fi };
    return cache_slot_fill(&fi->tiles[idx], key, cp, style, &ops);
}

void tile_worker_cleanup(void)
{
    free(tile_scratch);
    tile_scratch = NULL;
    tile_scratch_cap = 0;
}
