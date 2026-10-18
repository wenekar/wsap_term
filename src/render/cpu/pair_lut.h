#ifndef RENDER_CPU_PAIR_LUT_H
#define RENDER_CPU_PAIR_LUT_H

/*
 * A per-worker cache of blend tables for (fg, bg) color pairs.
 *
 * The row raster and the scene painter share it. A table turns coverage into a final pixel for a known bg. Compose uses it
 * when the pixels under a glyph all have one color: a cell bg, or a uniform
 * run under UI text.
 *
 * Two-hit rule: the first request for a pair returns NULL and records the
 * pair. The second request builds the table and returns it. A pair seen
 * only once uses the per-pixel blend. This keeps floods of one-off colors
 * from building tables that they never reuse.
 *
 * Exactness: a table gives the same pixels as the per-pixel blend. So the
 * pixels do not depend on hits or misses.
 * - alpha table: lut[a] == blend_px(fg, bg, faint ? (a + 1) / 2 : a)
 *   for every a in 0..255 (paint/blend.h).
 * - subpixel table: blend_subpx_lut(lut, bg, r, g, b) ==
 *   blend_subpx(fg, bg, r', g', b') for an opaque bg, where c' is
 *   (c + 1) / 2 when faint, else c.
 *
 * Threads: one cache per thread. No locks. The scene painter keeps one
 * thread-local cache. The raster keeps one in each struct raster_worker.
 *
 * Link contract: pair_lut.c may use only paint/blend.h and the sRGB
 * tables, because the scene painter links it into tests that stub the rest
 * (tests/ui/stubs/scene_paint_deps.c).
 */

#include <stdbool.h>
#include <stdint.h>

#include "paint/blend.h"

#define PAIR_LUT_SLOTS 16

/*
 * The fields are private to pair_lut.c. They are here so that a cache can
 * live inside another struct. An all-zero cache is valid and empty: a slot
 * with `used` false never hits.
 */
struct pair_lut_tag {
    uint32_t fg, bg;
    bool faint;
    bool used;
};

struct pair_lut_alpha_slot {
    struct pair_lut_tag tag, pending;
    uint32_t lut[256];
};

struct pair_lut_subpx_slot {
    struct pair_lut_tag tag, pending;
    struct subpx_lut lut;
};

struct pair_lut {
    struct pair_lut_alpha_slot alpha[PAIR_LUT_SLOTS];
    struct pair_lut_subpx_slot subpx[PAIR_LUT_SLOTS];
};

/* The alpha table of a pair, or NULL on the first request. */
const uint32_t *pair_lut_alpha(struct pair_lut *c, uint32_t fg, uint32_t bg, bool faint);
/* The subpixel table of a pair, or NULL on the first request. */
const struct subpx_lut *pair_lut_subpx(struct pair_lut *c, uint32_t fg, uint32_t bg, bool faint);

#endif
