/*
 * A consumer of the installed framework SDK. It calls the paint scene API and
 * the render API, so a missing definition fails at link time. The build uses
 * the installed headers and archives alone, through the wsap-render and
 * wsap-ui pkg-config files. tools/ci/sdk_install_check.sh builds and runs
 * this file.
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
