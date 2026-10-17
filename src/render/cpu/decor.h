#ifndef RENDER_CPU_DECOR_H
#define RENDER_CPU_DECOR_H

/*
 * Decoration masks: underlines, strike, overline, and the bar and underline
 * cursors. A mask is the coverage of one decoration over one cell.
 *
 * One set of masks exists for each (cw, ch, baseline, thickness). The GPU
 * path uploads the same masks, so both backends draw the same pixels.
 *
 * Geometry. t is the line thickness. All rows are clamped into [0, ch).
 * - t = max(1, (pixel_size * 3 + 32) / 64). That is about 1/21 of the em.
 *   struct font_metrics has no underline thickness or position, so decor
 *   takes them from pixel_size and baseline.
 * - single underline: rows [u, u + t), u = min(ch - t, baseline + (ch - baseline) / 2).
 * - double underline: two lines of t rows with a gap of t rows. The lower
 *   line ends at min(ch, u + 2t).
 * - curly underline: an antialiased wave, t thick, centered on row u + t / 2.
 *   Its amplitude is max(1, t) rows.
 * - dotted underline: rows of the single underline, dots and gaps of equal
 *   length.
 * - dashed underline: rows of the single underline, dashes longer than the
 *   gaps.
 * - strike: rows [(ch - t + 1) / 2, (ch - t + 1) / 2 + t). At t = 1 this is row ch / 2,
 *   as in the old renderer.
 * - overline: rows [min(1, ch - t), min(1, ch - t) + t).
 * - cursor bar: columns [0, min(cw, 2t)), all rows.
 * - cursor underline: rows [ch - min(ch, 2t), ch), all columns.
 *
 * Pattern rule. Each cell holds a whole number of periods of each dotted,
 * dashed, and curly pattern. The period is cw / k, and it need not be a
 * whole number of pixels. Each cell starts at phase 0. So the pattern continues across
 * cells, across both halves of a wide glyph, and across cached tiles.
 *
 * Draw order inside a cell: glyph, then underline, strike, overline, then
 * the cursor marks. The decorations use the cell fg. The cursor marks use
 * terminal_frame.cursor_color and are never faint.
 *
 * Math: integer or fixed point. Build each set once. Do not use the FPU in
 * a way that can change between ISA levels.
 */

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>

#include "font/font.h"

#define DECOR_UNDERLINE_KINDS (WSAP_VTCORE_UNDERLINE_DASHED + 1)

struct decor_key {
    int cw, ch;
    int baseline;
    int thickness;
};

/*
 * The coverage of one decoration over one cell.
 *
 * - `cov` holds ch rows of cw bytes. Rows outside [y0, y1) are all zero.
 * - `faint` is the same mask with each value c replaced by (c + 1) / 2.
 * - `solid` means: every value in rows [y0, y1) is 255. A raster can then
 *   fill those rows, or blend them at 128 when faint.
 * - y0 == y1 means an empty mask. underline[WSAP_VTCORE_UNDERLINE_NONE] is
 *   empty.
 */
struct decor_mask {
    int y0, y1;
    bool solid;
    const uint8_t *cov;
    const uint8_t *faint;
};

/*
 * All masks for one key. Read only after decor_get() returns it. Valid until
 * decor_cache_destroy(), or until a decor_get() call evicts it. A set that
 * a frame uses is not evicted during that frame.
 */
struct decor_set {
    struct decor_key key;
    struct decor_mask underline[DECOR_UNDERLINE_KINDS];
    struct decor_mask strike;
    struct decor_mask overline;
    struct decor_mask cursor_bar;
    struct decor_mask cursor_underline;
};

/* The key of a font. */
struct decor_key decor_key_of(const struct font_metrics *m);

/*
 * A small LRU of decor sets. The renderer owns one. Four sets cover the
 * terminal font, the 1.5x font, a thumbnail font, and one spare.
 */
struct decor_cache;

struct decor_cache *decor_cache_create(void);
void decor_cache_destroy(struct decor_cache *c);

/*
 * Returns the set for the metrics of a font. It builds a missing set, and
 * it can evict the least recently used one. Main thread, before the
 * parallel phase. Returns NULL when the memory is missing. The caller then
 * draws no decorations and caches no decorated cell. Steady state
 * allocates nothing.
 */
const struct decor_set *decor_get(struct decor_cache *c, const struct font_metrics *m);

#endif
