#ifndef WSAP_UI_YASL_METAL_H
#define WSAP_UI_YASL_METAL_H

#include <stdbool.h>

#include "util/geom.h"

struct yasl_surface;

/* Native surfaces present through the Metal layer of the view. Only macOS
 * has one. Elsewhere attach fails, and the window draws on the CPU. */

/* One drawable of the layer. The frame owns it until draw returns. */
struct window_metal_frame {
    void *drawable;
    struct vector2i size;
    bool transaction;
};

typedef void (*window_metal_draw_fn)(const struct window_metal_frame *frame,
                                     void *ud);

/* Binds the layer of a native surface to device. Returns the layer, or
 * NULL when the surface cannot present with Metal. */
void *window_metal_attach(struct yasl_surface *s, void *device);

/* Runs draw with the next drawable of layer, inside an autorelease pool.
 * Returns false when the layer gives no drawable in time. */
bool window_metal_draw(void *layer, struct yasl_surface *s,
                       window_metal_draw_fn draw, void *ud);

#endif
