#ifndef RENDER_CPU_RASTER_H
#define RENDER_CPU_RASTER_H

/*
 * The row raster. It paints one grid row of the canvas through a clip
 * window [x0, x1), in two passes. It consumes the output of
 * render_row_walk() and nothing else of the terminal.
 *
 * Coordinates: x is a grid-local pixel column. 0 is grid.rect.x. The cell
 * area is [0, cols * cw). The right margin is [cols * cw, width).
 *
 * The raster walks with RENDER_WALK_PEEK. It builds the cell key from the
 * glyph id first. It fetches the glyph only on a miss, or on a hit with the
 * spill flag.
 *
 * Pass 1 makes every pixel of the window opaque.
 * - A run of BLANK cells with one bg and no decoration or cursor bit is one
 *   fill.
 * - A cell with a cache tile (L0 or cell_cache_find()) is blitted.
 * - Any other cell is composed. It may get a tile through
 *   cell_cache_claim(). Compose uses the pair LUT when the pair has one.
 * - Compose draws, in order: the bg, the part of the glyph inside the cell
 *   span, the decorations, and the cursor marks (render/cpu/decor.h).
 * - The cell span is [col * cw, col * cw + cw). Each half of a wide glyph is
 *   one cell. The right half composes the glyph with an origin offset of
 *   -cw. So each half has its own tile.
 * - The right margin fills with frame->default_bg.
 * - Compose copies the spill of the cell into the spill list. The spill is
 *   the ink outside the span of the glyph: [0, cw) for a one-column glyph,
 *   [0, 2cw) for a wide glyph. An italic overhang or a wide icon spills.
 *   The ink of a wide glyph in the other half is not spill, unless that half
 *   is not in the grid.
 * - A cell whose tile hit has the spill flag fetches its glyph to copy the
 *   spill. No glyph pointer outlives the walk callback.
 *
 * Pass 2 blends the spill list over the pass-1 pixels, from left to right
 * in the order of the spilling cells.
 * - A spill clips to the cell area. It never reaches the margin or the
 *   padding. So the full path and the incremental path agree.
 * - A spill also clips to the window.
 * - Alpha and subpixel spills blend with blend_any and blend_any_subpx.
 *   Color spills compose premultiplied over the pixel.
 *
 * Spill reach. The reach of a row is the largest distance, in pixels, that
 * the ink of any cell extends past its span, to the left or the right,
 * before the clip. A full row paint measures it. A clipped repaint gets the
 * stored reach. It walks the columns that cover [x0 - reach, x1 + reach)
 * and clips every write to [x0, x1). The result is the same as a full row
 * paint inside the window.
 *
 * This replaces bg_ready, ink_end, the restart, and lazy backgrounds of the
 * old row painter.
 *
 * ISA levels. raster.c has one always-inline body. It has one NOINLINE
 * wrapper per level, for the row and for the walk callback, marked with
 * RENDER_TARGET_AVX2 or RENDER_TARGET_AVX512 (render/cpu/isa.h). The
 * kernels of render/cpu/kernels.h inline into each wrapper. The renderer
 * stores the row function of its level. That pointer is the only place
 * where the level is chosen. Every level gives the same pixels as SCALAR.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/cpu/pair_lut.h"
#include "render/term_frame.h"

struct cell_cache;
struct cell_cache_worker;
struct cell_class;
struct decor_set;
struct font_instance;

enum raster_spill_kind : uint8_t {
    RASTER_SPILL_ALPHA,
    RASTER_SPILL_SUBPX,
    RASTER_SPILL_COLOR,
};

/*
 * The spill of one cell, clipped to the cell area and to the window.
 * - [x0, x1) are grid-local columns. [y0, y1) are the rows of the grid row
 *   that can have ink: 0 <= y0 < y1 <= ch. The other rows have none.
 * - `offset` is the byte offset of row y0 in the coverage arena. The arena
 *   holds rows y0 to y1 - 1, one after the other. A row has (x1 - x0) * bpp
 *   bytes. bpp is 1 for ALPHA, 3 for SUBPX, and 4 for COLOR (premultiplied
 *   0xAARRGGBB words). A COLOR offset is a multiple of 4.
 * - ALPHA and SUBPX store the coverage after faint. `fg` is their color.
 * - COLOR stores the glyph pixels. `faint` is its faint bit.
 */
struct raster_spill {
    int x0, x1;
    int y0, y1;
    uint32_t fg;
    size_t offset;
    enum raster_spill_kind kind;
    bool faint;
};

/*
 * The spills of the row that a worker paints now. raster.c empties it at
 * the start of each row. It grows when it needs room, and it keeps its
 * memory. So steady state allocates nothing.
 */
struct raster_spill_list {
    struct raster_spill *items;
    int n, cap;
    uint8_t *cov;
    size_t used, size;
};

/*
 * The per-worker context. One for the main thread and one for each pool
 * worker. Only its owner thread touches it during the phase.
 * - `lut`: the pair LUT cache of this worker.
 * - `cache`: this worker's cell cache state (seen filter, pending table,
 *   hit log). NULL when the renderer has no cell cache.
 * - `spill`: the spill list.
 * - `scratch`: room for one cell of `scratch_px` pixels. A cell that the
 *   window cuts composes here first. It grows when it needs room.
 */
struct raster_worker {
    struct pair_lut lut;
    struct cell_cache_worker *cache;
    struct raster_spill_list spill;
    uint32_t *scratch;
    size_t scratch_px;
};

/*
 * Prepares a zeroed worker. With `cache` non-NULL it creates the cell cache
 * state. Returns false when the memory is missing. Main thread.
 */
bool raster_worker_init(struct raster_worker *w, struct cell_cache *cache);
void raster_worker_release(struct raster_worker *w);

/*
 * What every row of one frame shares. The main thread fills it before the
 * phase. It does not change during the phase.
 * - `origin` is the canvas pixel at the grid rect origin. Row r starts at
 *   origin + r * ch * stride.
 * - `cols` is the number of visible columns. `width` is the clipped grid
 *   rect width: cols * cw plus the right margin.
 * - `decor` NULL means: no decorations. Then no decorated or cursor cell is
 *   cached.
 * - `cache` NULL means: no cell cache this frame.
 * - The default pair and the cursor color come from `frame`.
 */
struct raster_frame {
    struct font_instance *font;
    const struct terminal_frame *frame;
    const struct decor_set *decor;
    struct cell_class *cache;
    uint64_t font_version;
    uint32_t *origin;
    int stride;
    int cw, ch;
    int cols;
    int width;
};

/*
 * One row job.
 * - [x0, x1) is the window, with 0 <= x0 < x1 <= width. A full row is
 *   [0, width).
 * - `reach` is the stored spill reach of the row. A full row ignores it.
 */
struct raster_job {
    int row;
    int x0, x1;
    int reach;
};

/*
 * Paints one row job. Returns the spill reach measured over the cells that
 * it walked. For a full row, that is the reach of the row. Store it in the
 * surface state.
 *
 * Threads: any thread inside the parallel phase, with its own worker. Two
 * jobs can run at the same time when their windows do not overlap. A job
 * writes only the pixels of its row band inside its window. It reads no
 * canvas pixel outside them.
 */
typedef int (*raster_row_fn)(const struct raster_frame *f, struct raster_worker *w,
                             const struct raster_job *job);

/*
 * The row function of an enum render_isa level. The level must be resolved
 * (not RENDER_ISA_AUTO) and built for this CPU architecture. An unknown
 * level gives the SCALAR function.
 */
raster_row_fn raster_row_for(int isa);

#endif
