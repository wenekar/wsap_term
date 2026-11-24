#ifndef TERMINAL_RENDER_TERM_RENDER_H
#define TERMINAL_RENDER_TERM_RENDER_H

/*
 * The terminal grid entry points of the renderer. `struct terminal_frame`
 * is terminal content, so these calls stay in the terminal layer and the
 * framework SDK does not ship them. renderer_draw() accepts a generic
 * grid view instead.
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

struct render_grid {
    const struct terminal_frame *frame;
    struct font_instance *font;
    struct rect2i rect;
};

/* Terminal content and UI for one app frame. See struct renderer_frame for
 * the UI fields. The grid is empty if its terminal frame is NULL or invalid. */
struct render_frame {
    struct render_grid grid;
    const struct ui_scene *graphics;
    const struct ui_scene *ui;
    struct font_instance *ui_font;
    const struct ui_damage *ui_damage;
    uint32_t base;
    bool repaint;
    bool cpu_only;
};

/*
 * Draws one frame into `fb`.
 *
 * - Main thread only. One call at a time for each renderer.
 * - With fb->history, the renderer repaints only what changed since the
 *   last frame of that history. The pixels are always the same as a fresh
 *   paint of the same frame.
 * - fb->pixels NULL with fb->acquire_pixels means: the canvas comes on
 *   demand. The renderer takes the canvas only when the CPU paints the
 *   frame, so a frame that the GPU takes needs no canvas.
 * - Without a history, it paints every pixel and keeps no state.
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
 * 0xAARRGGBB, row after row. `span_cols` is 1 or 2. `out_w` and `out_h`
 * receive the width and the height in pixels.
 *
 * The pixels are the same as the pixels of those cells in a CPU frame of
 * `tf` from render_draw(). `tf` gives the cursor: set it visible to get the
 * cells with the cursor on them.
 *
 * Returns false and writes nothing when the frame has no grid, when the
 * cursor position is outside the grid, when the row is not a single-size
 * line, when `max_px` is less than the pixel count of the cells, when the
 * span is outside the row, or when the font metrics or memory are missing.
 * Main thread, outside render_draw().
 */
bool render_cursor_cell(struct renderer *r, struct font_instance *font,
                        const struct terminal_frame *tf,
                        int span_begin, int span_cols,
                        uint32_t *out, size_t max_px,
                        int *out_w, int *out_h);

/*
 * Draws directly through the GPU backend for tests, resolving the grid
 * from the terminal frame.
 */
enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb,
                                       const struct render_frame *frame);

#endif
