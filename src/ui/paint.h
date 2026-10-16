#ifndef WSAP_UI_PAINT_H
#define WSAP_UI_PAINT_H

#include "paint/scene.h"
#include "util/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A widget paints in its own frame: (0, 0) is the top left of its bounds
 * and size is the size of its bounds. */
struct ui_paint_context {
    struct ui_scene *scene;
    struct vector2i size;
};

struct rect2i ui_paint_bounds(const struct ui_paint_context *paint);
bool ui_paint_push_clip(struct ui_paint_context *paint, struct rect2i local);
bool ui_paint_pop_clip(struct ui_paint_context *paint);
bool ui_paint_rect(struct ui_paint_context *paint, struct rect2i local,
                  uint32_t color);
bool ui_paint_rounded_rect(struct ui_paint_context *paint, struct rect2i local,
                           int radius, uint32_t color);
/* Paints a Gaussian drop shadow cast by local. The shadow spills past
 * local by ui_shadow_extent(blur), so paint it before the box itself. */
bool ui_paint_shadow(struct ui_paint_context *paint, struct rect2i local,
                     int radius, int blur, uint32_t color);
bool ui_paint_image(struct ui_paint_context *paint, struct vector2i local,
                    const struct ui_image *image);
bool ui_paint_text(struct ui_paint_context *paint, struct rect2i local,
                   struct ui_text_style style, uint32_t color,
                   const char *text, size_t len);

#endif
