#include "desktop/window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yasl/yasl.h>

#include "render/render.h"
#include "ui/widgets/icon.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

static void *acquire_device(void *userdata)
{
    return renderer_device(userdata);
}

static void compose(struct window *w, struct window_frame *frame, void *userdata)
{
    uint64_t now_ms = window_frame_time_ms(w);
    bool main = frame->surface == window_surface(w);
    if (main)
        window_ui_prepare_frame(w, now_ms);

    struct window_ui_frame ui = {0};
    bool built = window_ui_build_frame(w, frame->surface, now_ms, &ui);
    struct renderer_frame render = {
        .base = ui.background,
        .ui = ui.scene,
        .ui_font = ui.font,
        .ui_damage = main ? &ui.damage : NULL,
        .repaint = !built || ui.repaint,
        .cpu_only = ui.gpu_off || ui.suspended,
    };
    struct render_result result = renderer_draw(userdata, &frame->target, &render);
    window_present(w, frame, &result.damage, result.presented);
    window_ui_frame_done(w, frame, now_ms);
}

static void quit(void *userdata)
{
    yasl_loop_quit(userdata);
}

static void last_window_closed(void *userdata, struct window *last)
{
    (void)last;
    quit(userdata);
}

int wsap_window_run(const struct wsap_window_spec *spec)
{
    if (!spec || !spec->root || spec->size.x < 0 || spec->size.y < 0 ||
        yasl_get_backend() != YASL_BACKEND_AUTO)
        return EXIT_FAILURE;
    if (!render_init()) {
        fputs("wsap-desktop: cannot initialize the renderer\n", stderr);
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    struct renderer *renderer = renderer_create(NULL);
    struct yasl_loop *loop = renderer ? yasl_loop_create() : NULL;
    if (!renderer) {
        fputs("wsap-desktop: cannot create the renderer\n", stderr);
    } else if (!loop) {
        fputs("wsap-desktop: cannot create the event loop\n", stderr);
    } else {
        struct window_config config = { .size = spec->size };
        struct window_host host;
        window_host_init(&host, loop, &config, &(struct window_host_hooks){
            .empty = last_window_closed,
            .quit = quit,
            .ud = loop,
        });
        struct window *w = window_open(&host, &(struct window_spec){
            .title = spec->title,
            .compose = compose,
            .compose_ud = renderer,
            .acquire_device = acquire_device,
            .device_ud = renderer,
        });
        if (!w) {
            fputs("wsap-desktop: cannot open the window\n", stderr);
        } else if (!window_ui_mount(w, &(struct window_ui_spec){ .root = spec->root })) {
            fputs("wsap-desktop: cannot mount the widget tree\n", stderr);
        } else {
            window_ui_set_style(w, NULL, NULL, 0xff000000u);
            int status = yasl_loop_run(loop);
            if (status < 0)
                fprintf(stderr, "wsap-desktop: event loop: %s\n", strerror(-status));
            else
                result = EXIT_SUCCESS;
        }
        window_host_destroy(&host);
        yasl_loop_destroy(loop);
    }
    renderer_destroy(renderer);
    render_shutdown();
    return result;
}

int wsap_window_show_image(const char *title, const struct ui_image *image)
{
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return EXIT_FAILURE;

    struct ui_icon icon;
    ui_icon_init(&icon);
    ui_icon_set_image(&icon, image);
    struct ui_widget *root = ui_icon_widget(&icon);
    int result = wsap_window_run(&(struct wsap_window_spec){
        .root = root,
        .title = title,
        .size = image->size,
    });
    ui_widget_destroy(root);
    return result;
}
