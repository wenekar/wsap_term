#ifndef TERMINAL_RENDER_TERM_RENDER_H
#define TERMINAL_RENDER_TERM_RENDER_H

/*
 * The terminal grid entry points of the renderer. `struct terminal_frame`
 * is terminal content, so these calls stay in the terminal layer and the
 * framework SDK does not ship them. A consumer of the SDK uses
 * render_draw_view() and renderer_cursor_band() from render/core.h, which
 * take a render_grid_view.
 *
 * The definitions are in terminal/render/draw.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/gpu/gpu.h"
#include "render/render.h"

struct font_instance;
struct terminal_frame;

/*
 * Draws one frame into `fb`.
 *
 * - Main thread only. One call at a time for each renderer.
 * - With fb->history, the renderer repaints only what changed since the
 *   last frame of that history. The pixels are always the same as a fresh
 *   paint of the same frame.
 * - fb->pixels NULL with fb->acquire_pixels means: the canvas comes on
 *   demand. The renderer takes the canvas only when the CPU paints the
 *   frame, so a frame that the GPU takes makes no allocation.
 * - Without a history, it paints every pixel and keeps no state. It does
 *   not allocate for that case.
 * - The environment value WSAP_RENDER_FULL=1 makes every frame a fresh
 *   paint. It is a debug knob.
 * - The environment value WSAP_RENDER_SCROLL=move|paint|auto picks how a
 *   scroll is drawn: move the old rows, or paint them again. The default
 *   is auto: the renderer picks the faster one from measured cost. The
 *   renderer reads it once, at creation. Pixels and damage are the same
 *   for all three. It is a test and debug knob.
 * - NULL r, fb, or frame returns a zero result and draws nothing.
 */
struct render_result render_draw(struct renderer *r, struct framebuffer *fb,
                                 const struct render_frame *frame);

/*
 * Paints the cells [span_begin, span_begin + span_cols) of the cursor row
 * of `tf` into `out`: span_cols * cell_w pixels wide, cell_h pixels high,
 * 0xAARRGGBB, row after row. `span_cols` is 1 or 2.
 *
 * The pixels are the same as the pixels of those cells in a CPU frame of
 * `tf` from render_draw(). `tf` gives the cursor: set it visible to get the
 * cells with the cursor on them.
 *
 * Returns false and writes nothing when `out` has less than `max_px`
 * pixels of room, when the row is not a single-size line, or when memory
 * is missing. Main thread, outside render_draw().
 */
bool render_cursor_cell(struct renderer *r, struct font_instance *font,
                        const struct terminal_frame *tf,
                        int span_begin, int span_cols,
                        uint32_t *out, size_t max_px,
                        int *out_w, int *out_h);

/*
 * The test entry point. It is render_gpu_draw_view() with the grid resolved
 * from the terminal frame of `frame`.
 */
enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb,
                                       const struct render_frame *frame);

#endif
