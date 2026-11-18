#include <stdint.h>

#include "paint/scene.h"
#include "util/geom.h"

int main(void)
{
    struct ui_scene scene = {0};
    struct rect2i rect = {
        .pos = {.x = 1, .y = 2},
        .size = {.x = 3, .y = 4},
    };

    ui_scene_init(&scene);
    if (!rect2i_contains(rect, (struct vector2i){.x = 2, .y = 3}))
        return 1;
    if (!ui_scene_rect(&scene, rect, 0xff000000u))
        return 1;
    if (scene.n_ops != 1)
        return 1;
    ui_scene_destroy(&scene);
    return 0;
}
