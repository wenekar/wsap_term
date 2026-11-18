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
 * Fills `out` from `tf`. Returns false when the frame has no grid: a NULL
 * frame, or a frame with no snapshot. The view points at the frame and its
 * snapshot. It stays valid as long as they do.
 */
bool render_term_grid_view(const struct terminal_frame *tf, struct render_grid_view *out);

/*
 * The key of grid row `row` as the canvas shows it: a mix of the content
 * key, the selection span of the row, and the cursor x and style when the
 * cursor is on the row. It is never 0 for a row that the planner can keep
 * or move. The key mixes the platform hash, so the tests can use it.
 */
uint64_t render_term_row_key(const struct terminal_frame *frame, int row);

#endif
