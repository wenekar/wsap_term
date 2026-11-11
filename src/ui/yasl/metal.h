#ifndef WSAP_UI_YASL_METAL_H
#define WSAP_UI_YASL_METAL_H

#include <stdbool.h>

#include "util/geom.h"

struct yasl_surface;
struct present_watch;

/*
 * One frame of one surface that the GPU draws.
 *
 * - `size` is the canvas in pixels: the size of the next drawable of the
 *   layer. It is known before a drawable is taken.
 * - `drawable` is NULL while the surface prepares the frame. `acquire` asks
 *   the layer for the drawable after the frame data is ready, and it writes
 *   `drawable`. The layer holds a drawable for the shortest time this way.
 * - `transaction` means: present inside the Core Animation transaction of a
 *   live resize.
 */
struct window_metal_frame {
    void *drawable;
    struct vector2i size;
    bool transaction;
    void *(*acquire)(void *ud);
    void *acquire_ud;
};

typedef void (*window_metal_draw_fn)(const struct window_metal_frame *frame,
                                     void *ud);

/*
 * What the display reports for the frames of one surface.
 *
 * - `presents` counts the frames that a display showed. NULL counts none.
 * - `sample` means that this surface belongs to the latency probe, so its
 *   frames carry the sample that waits for a presentation.
 */
struct window_metal_watch {
    struct present_watch *presents;
    bool sample;
};

void *window_metal_attach(struct yasl_surface *s, void *device);
void window_metal_detach(void *layer, struct yasl_surface *s);

/*
 * Prepares and submits one frame. `draw` runs once. It sees no drawable, and it
 * calls `frame->acquire` when it needs the drawable. A layer of zero size runs
 * no draw. Returns true when the layer had a size, so the frame was attempted;
 * a missing drawable is not an error, and the surface asks for the next
 * frame. `watch` says how the display reports the presentation of the frame.
 */
bool window_metal_draw(void *layer, struct yasl_surface *s,
                       struct window_metal_watch watch,
                       window_metal_draw_fn draw, void *ud);

#endif
