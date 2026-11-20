#ifndef RENDER_GRID_H
#define RENDER_GRID_H

/*
 * The grid view of one frame. It is the generic input of the grid path: the
 * visible size, the colors, the row keys, and the walk that resolves the
 * cells. The renderer names no terminal type.
 *
 * The terminal layer builds a view from its frame and passes it to
 * render_draw_view(). A NULL view means: no grid.
 */

#include <stdbool.h>
#include <stdint.h>

#include "render/cell.h"
#include "util/geom.h"

/*
 * The terminal-specific part of a grid.
 * - `walk` resolves the cells of one row into render_cell values.
 * - `line_size` gives the shape of one row.
 * - `row_key` gives the content key of one row. It is never 0 for a row
 *   that the planner can keep or move.
 * - `reserve_walk` makes room for the walk scratch of one thread. It runs
 *   on the main thread and on each pool thread.
 *
 * The functions read `source` and the terminal frame that it points at.
 * They must not change either during a frame.
 */
struct render_grid_ops {
    void (*walk)(struct font_instance *font, const void *source, int row,
                 int c0, int c1, unsigned flags, const struct render_walk_sink *sink);
    enum render_line_size (*line_size)(const void *source, int row);
    uint64_t (*row_key)(const void *source, int row);
    bool (*reserve_walk)(int cols);
};

/*
 * - `cells` is the full terminal size, not the visible size. A reflow keeps
 *   the row keys of rows that it rewraps.
 * - `selection_fg` and `selection_bg` are resolved. A zero value means the
 *   built-in selection colors, as the terminal frame defines them.
 * - `palette_gen`, `default_fg`, `default_bg`, `cursor_color`,
 *   `reverse_video`, and `blink_on` come from the terminal frame.
 */
struct render_grid_view {
    const struct render_grid_ops *ops;
    const void *source;
    struct vector2i cells;
    uint64_t palette_gen;
    uint32_t default_fg, default_bg;
    uint32_t cursor_color;
    uint32_t selection_fg, selection_bg;
    bool reverse_video;
    bool blink_on;
};

#endif
