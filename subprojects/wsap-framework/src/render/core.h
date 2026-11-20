#ifndef RENDER_CORE_H
#define RENDER_CORE_H

/*
 * The renderer calls that the terminal layer uses. The terminal layer above
 * this library adapts a terminal frame to a render_grid_view and passes it
 * to these calls, from its render_draw() and render_cursor_cell().
 *
 * A NULL `grid` means that the frame has no grid.
 */

#include "render/grid.h"
#include "render/render.h"

/*
 * Draws one frame. It is render_draw() with the grid already resolved.
 * Every rule of render_draw() applies.
 */
struct render_result render_draw_view(struct renderer *r, struct framebuffer *fb,
                                      const struct render_frame *frame,
                                      const struct render_grid_view *grid);

/*
 * Paints the cells [span_begin, span_begin + span_cols) of row `row` of
 * `grid` into `out`: span_cols * cell_w pixels wide, cell_h pixels high,
 * 0xAARRGGBB, row after row. It is render_cursor_cell() with the grid
 * already resolved. Every other rule of render_cursor_cell() applies.
 */
bool renderer_cursor_band(struct renderer *r, struct font_instance *font,
                          const struct render_grid_view *grid, int row,
                          int span_begin, int span_cols,
                          uint32_t *out, size_t max_px,
                          int *out_w, int *out_h);

#endif
