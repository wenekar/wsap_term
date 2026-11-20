#ifndef RENDER_FRAME_TERM_VIEW_H
#define RENDER_FRAME_TERM_VIEW_H

/*
 * The terminal grid adapter. It turns a terminal_frame into the generic
 * render_grid_view of render/grid.h.
 */

#include <stdbool.h>

#include "render/grid.h"

struct terminal_frame;

/*
 * Builds the grid view of `tf` in `out`. Returns false when the frame has no
 * grid: a NULL frame, or a frame with no snapshot. The view borrows the frame
 * and its snapshot, and it stays valid only while they do.
 */
bool render_term_grid_view(const struct terminal_frame *tf, struct render_grid_view *out);

/*
 * Returns the key of grid row `row` as the canvas shows it: a mix of the
 * content key, the selection span of the row, and the cursor x and style
 * when the cursor is on the row. The key is 0 only when the content key of
 * the row is unknown. The planner never keeps or moves such a row.
 */
uint64_t render_term_row_key(const struct terminal_frame *frame, int row);

#endif
