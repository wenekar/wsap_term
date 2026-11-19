#ifndef RENDER_SCENE_PAINT_H
#define RENDER_SCENE_PAINT_H

#include "util/geom.h"

#include <stdbool.h>
#include <stdint.h>

struct font_instance;
struct ui_scene;

bool ui_scene_paint(struct font_instance *fi, uint32_t *px, int width, int height,
                    const struct ui_scene *scene);

bool ui_scene_paint_clipped(struct font_instance *fi, uint32_t *px, int width, int height,
                            const struct ui_scene *scene,
                            struct rect2i paint_clip);

#endif
