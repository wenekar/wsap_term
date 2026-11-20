#ifndef WSAP_UI_PAINT_H
#define WSAP_UI_PAINT_H

#include "paint/scene.h"
#include "util/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct css_scale_context;
struct css_style;

struct ui_paint_context {
    struct ui_scene *scene;
    struct vector2i size;
};

struct ui_paint_box_style {
    uint32_t bg_color;
    uint32_t border_color;
    int border_width;
    int radius;
    /* The drop shadow of the box. Zero alpha paints none. */
    uint32_t shadow_color;
    struct vector2i shadow_offset;
    int shadow_blur;
};

/* Resolves the drop shadow of `style` into `out`. Returns false when the
 * style has no visible shadow. */
bool ui_paint_shadow_style_of(const struct css_style *style,
                              const struct css_scale_context *scale,
                              int radius, struct ui_paint_box_style *out);

/* Paints the drop shadow of `style` below `local`. */
bool ui_paint_box_shadow(struct ui_paint_context *paint, struct rect2i local,
                         const struct ui_paint_box_style *style);

/* Resolves the paint fields of a box, including the drop shadow, from CSS. */
struct ui_paint_box_style ui_paint_box_style_of(
    const struct css_style *style, const struct css_scale_context *scale);
/* Paints the border and the fill of a box. Writes the inset fill rectangle to
 * `out_fill`, when it is not NULL and the rectangle is not empty. */
bool ui_paint_box(struct ui_paint_context *paint, struct rect2i local,
                  const struct ui_paint_box_style *style,
                  struct rect2i *out_fill);

struct rect2i ui_paint_bounds(const struct ui_paint_context *paint);
bool ui_paint_push_clip(struct ui_paint_context *paint, struct rect2i local);
bool ui_paint_pop_clip(struct ui_paint_context *paint);
bool ui_paint_rect(struct ui_paint_context *paint, struct rect2i local,
                   uint32_t color);
/* Outlines `local` with four non-overlapping edge rectangles. */
bool ui_paint_rect_stroke(struct ui_paint_context *paint, struct rect2i local,
                          int width, uint32_t color);
bool ui_paint_rounded_rect(struct ui_paint_context *paint, struct rect2i local,
                           int radius, uint32_t color);
bool ui_paint_shadow(struct ui_paint_context *paint, struct rect2i local,
                     int radius, int blur, uint32_t color);
bool ui_paint_image(struct ui_paint_context *paint, struct vector2i local,
                    const struct ui_image *image);
bool ui_paint_text(struct ui_paint_context *paint, struct rect2i local,
                   struct ui_text_style style, uint32_t color,
                   const char *text, size_t len);

#endif
