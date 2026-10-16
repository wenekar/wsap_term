#ifndef RENDER_SCENE_PAINT_H
#define RENDER_SCENE_PAINT_H

#include "util/geom.h"

#include <stdbool.h>
#include <stdint.h>

struct font_instance;
struct ui_scene;

/* Text ops draw with fi's glyphs. Paints one whole frame. Starts with
 * glyph_cache_refresh(), so do not call it from an emit callback or a
 * paint worker. */
bool ui_scene_paint(struct font_instance *fi, uint32_t *px, int width, int height,
                    const struct ui_scene *scene);

/* Paints a part of a frame. It does not refresh fi. The caller calls
 * glyph_cache_refresh() once per frame before the first part. */
bool ui_scene_paint_clipped(struct font_instance *fi, uint32_t *px, int width, int height,
                            const struct ui_scene *scene,
                            struct rect2i paint_clip);

#endif
