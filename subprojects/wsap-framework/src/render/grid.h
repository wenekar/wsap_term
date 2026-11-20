#ifndef RENDER_GRID_H
#define RENDER_GRID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/cell.h"
#include "util/geom.h"

/*
 * Callbacks that resolve grid rows from caller-owned data.
 * - `walk` resolves the cells of one row into render_cell values.
 * - `line_size` gives the shape of one row.
 * - `row_key` gives the content key of one row. It is never 0 for a row
 *   that the planner can keep or move.
 * - `reserve_walk` makes room for the walk scratch of one thread. It runs
 *   on the main thread and on each pool thread.
 *
 * The renderer can call these functions from worker threads. The source
 * must stay readable and unchanged until the frame finishes.
 */
struct render_grid_ops {
    void (*walk)(struct font_instance *font, const void *source, int row,
                 int c0, int c1, unsigned flags, const struct render_walk_sink *sink);
    enum render_line_size (*line_size)(const void *source, int row);
    uint64_t (*row_key)(const void *source, int row);
    bool (*reserve_walk)(int cols);
};

/*
 * A cell grid resolved by the caller for one frame.
 *
 * - `ops` and its callbacks are required when the renderer draws the grid.
 * - `rect` is the grid area in canvas pixels. An empty rectangle or a
 *   negative origin omits the grid. The renderer clips it to the canvas.
 * - `font` NULL selects the default font.
 * - `cells` is the full grid size in columns and rows, before clipping.
 * - `selection_fg` and `selection_bg` are the resolved selection colors.
 */
struct render_grid_view {
    const struct render_grid_ops *ops;
    const void *source;
    struct font_instance *font;
    struct rect2i rect;
    struct vector2i cells;
    uint64_t palette_gen;
    uint32_t default_fg, default_bg;
    uint32_t cursor_color;
    uint32_t selection_fg, selection_bg;
    bool reverse_video;
    bool blink_on;
};

struct renderer;

/* Paints one or two columns starting at `span_begin` on a single-size row
 * into `out` as packed 0xAARRGGBB pixels. Uses the grid data but not its
 * placement. `max_px` is the output capacity in pixels. Returns false if
 * the span is invalid, the buffer is too small, or the renderer cannot
 * allocate its working storage. Writes pixels, `out_w`, and `out_h` on
 * success. Call on the main thread, outside renderer_draw(). */
bool renderer_cursor_band(struct renderer *r, const struct render_grid_view *grid, int row,
                          int span_begin, int span_cols,
                          uint32_t *out, size_t max_px,
                          int *out_w, int *out_h);

#endif
