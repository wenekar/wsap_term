/*
 * A display-free check of the installed desktop, scene, and renderer APIs.
 * tools/ci/sdk_install_check.sh builds this file against wsap-desktop.
 */

#include <stdint.h>
#include <stdlib.h>

#include "desktop/window.h"
#include "paint/scene.h"
#include "render/render.h"

int main(void)
{
    if (!render_init())
        return 1;
    struct renderer *r = renderer_create(NULL);
    if (!r)
        return 2;

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    if (!ui_scene_rect(&scene, (struct rect2i){ .size = { .x = 1, .y = 1 } },
                       0xff123456u))
        return 3;
    uint32_t pixels[2] = {0};
    struct framebuffer fb = { .pixels = pixels, .size = { .x = 2, .y = 1 } };
    renderer_draw(r, &fb, &(struct renderer_frame){ .ui = &scene, .base = 0xff000000u });
    ui_scene_destroy(&scene);
    renderer_destroy(r);
    render_shutdown();
    if (pixels[0] != 0xff123456u || pixels[1] != 0xff000000u)
        return 4;
    if (wsap_window_run(&(struct wsap_window_spec){0}) != EXIT_FAILURE ||
        wsap_window_show_image(NULL, &(struct ui_image){0}) != EXIT_FAILURE)
        return 6;
    return EXIT_SUCCESS;
}
