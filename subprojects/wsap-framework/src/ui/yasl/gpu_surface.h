#ifndef WSAP_UI_YASL_GPU_SURFACE_H
#define WSAP_UI_YASL_GPU_SURFACE_H

#include <stdbool.h>

#include "util/geom.h"

struct yasl_surface;
struct present_watch;

/*
 * The GPU surface of one native window surface. A backend supplies it: the
 * Vulkan one, the Metal one, or `gpu_none.c` when the build has no GPU.
 */

/*
 * One frame of one surface that the GPU draws.
 *
 * - `size` is the canvas in pixels: the size of the next drawable of the
 *   layer. It is known before a drawable is taken.
 * - `drawable` is the drawable of the frame, or NULL while the layer has none.
 *   A layer that knows its target fills it before the draw. `acquire` asks the
 *   layer for the drawable after the frame data is ready, and it writes
 *   `drawable`, so the drawable leaves the pool for the shortest time.
 * - `transaction` marks a frame of a size change or a live resize. Metal
 *   presents it inside the Core Animation transaction of the resize. Vulkan
 *   asks the swapchain for an unpaced present mode.
 */
struct window_gpu_frame {
    void *drawable;
    struct vector2i size;
    void *(*acquire)(void *ud);
    void *acquire_ud;
    bool transaction;
};

typedef void (*window_gpu_draw_fn)(const struct window_gpu_frame *frame,
                                   void *ud);

/*
 * What the display reports for the frames of one surface.
 *
 * - `presents` counts the frames that a display showed. NULL counts none.
 * - `sample` means that this surface belongs to the latency probe, so its
 *   frames carry the sample that waits for a presentation.
 */
struct window_gpu_watch {
    struct present_watch *presents;
    bool sample;
};

void *window_gpu_attach(struct yasl_surface *s, void *device);
void window_gpu_detach(void *layer, struct yasl_surface *s);

/*
 * Prepares and submits one frame. `draw` runs once. It sees the drawable that
 * the layer filled already, if any, and it calls `frame->acquire` when it needs
 * one. A layer of zero size runs no draw. Returns true when the layer had a
 * size, so the frame was attempted; a missing drawable is not an error, and
 * the surface asks for the next frame. `watch` says how the display reports
 * the presentation of the frame. Set `transaction` for a frame that must show
 * together with a change of the native surface, as in the refresh callback of
 * yasl; a live resize and a size change set it too.
 */
bool window_gpu_draw(void *layer, struct yasl_surface *s,
                     struct window_gpu_watch watch, bool transaction,
                     window_gpu_draw_fn draw, void *ud);

/* Platform overlay sublayer for idle cursor blinking without frame redraws. */
bool window_gpu_cursor_supported(struct yasl_surface *s);
bool window_gpu_set_cursor(struct yasl_surface *s, bool active, bool visible,
                           struct rect2i rect, uint32_t color,
                           const uint32_t *pixels, uint64_t key);
bool window_gpu_set_cursor_visible(struct yasl_surface *s, bool visible);
void window_gpu_clear_cursor(struct yasl_surface *s);

#endif
