#ifndef WSAP_UI_YASL_METAL_H
#define WSAP_UI_YASL_METAL_H

#include <stdbool.h>

#include "util/geom.h"

struct yasl_surface;

struct window_metal_frame {
    void *drawable;
    struct vector2i size;
    bool transaction;
};

typedef void (*window_metal_draw_fn)(const struct window_metal_frame *frame,
                                     void *ud);

void *window_metal_attach(struct yasl_surface *s, void *device);

bool window_metal_draw(void *layer, struct yasl_surface *s,
                       window_metal_draw_fn draw, void *ud);

#endif
