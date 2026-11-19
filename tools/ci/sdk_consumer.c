/*
 * A consumer of the installed framework SDK. It calls one render API and one
 * UI API, so a missing definition fails at link time. The build of this file
 * is the relocatability check of the SDK: an installed archive must link with
 * the compiler of the consumer, and the program must run.
 *
 * It links through the wsap-render and wsap-ui pkg-config files only.
 */

#include <stdint.h>
#include <stdlib.h>

#include "paint/scene.h"
#include "render/render.h"

int main(void)
{
    struct ui_scene scene = {0};
    struct rect2i rect = {
        .pos = {.x = 1, .y = 2},
        .size = {.x = 3, .y = 4},
    };
    ui_scene_init(&scene);
    if (!ui_scene_rect(&scene, rect, 0xff000000u))
        return 1;
    if (scene.n_ops != 1)
        return 2;
    ui_scene_destroy(&scene);

    if (!render_init())
        return 3;
    struct renderer *r = renderer_create(NULL);
    if (!r)
        return 4;
    int isa = renderer_isa(r);
    renderer_destroy(r);
    render_shutdown();
    if (isa < 0)
        return 5;
    return 0;
}
