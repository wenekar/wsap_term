#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "app/config/settings.h"
#include "app/thumbnail.h"
#include "loop/latency_probe.h"
#include "yasl/yasl.h"
#include "loop/signals.h"
#include "theme/look.h"
#include "render/render.h"
#include "render/term_frame.h"
#include "util/damage.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "util/name.h"
#include "ui/yasl/inspector.h"
#include "ui/yasl/client.h"

static void app_stop(struct app *a);
static int run_loop(struct app *a);

static void app_compose_frame(struct window *w, struct window_frame *frame,
                              void *ud)
{
    struct app *a = ud;
    uint64_t now_ms = window_frame_time_ms(w);
    bool main = frame->surface == window_surface(w);
    if (main) {
        latency_probe_stage(LATENCY_STAGE_RENDER_START);
        window_ui_prepare_frame(w, now_ms);
        terminal_prepare(w, now_ms);
    }
    if (window_closed(w))
        return;

    struct window_ui_frame ui = {0};
    if (!window_ui_build_frame(w, frame->surface, now_ms, &ui))
        ui.repaint = true;

    struct render_frame render = {
        .base = ui.background,
        .ui_font = ui.font,
        .cpu_only = ui.gpu_off,
    };
    struct terminal_frame grid;
    if (ui.suspended) {
        render.cpu_only = true;
    } else {
        render.ui = ui.scene;
        render.ui_damage = main ? &ui.damage : NULL;
        render.repaint = ui.repaint;
        struct terminal_frame_source source = {0};
        if (main && terminal_frame_source(w, now_ms, &source)) {
            /* The theme has no selection colors. The zero selection colors
             * of the frame give the built-in ones. */
            terminal_frame_init(&grid, source.snapshot, source.cursor_visible);
            render.grid = (struct render_grid){
                .frame = &grid,
                .font = source.font,
                .rect = source.rect,
            };
        }
    }
    struct render_result result = render_draw(a->renderer, &frame->target, &render);
    if (window_present(w, frame, &result.damage, result.presented) && main)
        latency_probe_complete();
    if (!window_closed(w))
        window_ui_frame_done(w, frame, now_ms);
}

static bool app_paint_scene(void *ud, struct font_instance *font,
                            uint32_t *pixels, struct vector2i size,
                            const struct ui_scene *scene)
{
    struct app *a = ud;
    struct framebuffer target = {
        .pixels = pixels,
        .size = size,
        .fresh = true,
    };
    struct render_frame frame = {
        .ui = scene,
        .ui_font = font,
        .repaint = true,
        .cpu_only = true,
    };
    (void)render_draw(a->renderer, &target, &frame);
    return true;
}

static void style_window(struct app *a, struct window *w)
{
    const struct wsap_vtcore_theme *theme = look_palette(a->look);
    terminal_set_theme(w, theme);
    window_ui_set_style(w, look_sheet(a->look), theme->bg);
}

static void look_changed(void *ud)
{
    struct app *a = ud;
    for (int i = 0; i < a->host.count; i++)
        style_window(a, a->host.windows[i]);
}

static int picker_count(void *ud)
{
    return look_theme_count(ud);
}

static const char *picker_label(void *ud, int index)
{
    const struct look_theme *theme = look_theme_at(ud, index);
    return theme ? theme->label : "";
}

static bool picker_selected(void *ud, int index)
{
    return look_is_committed(ud, index);
}

static int picker_previewed(void *ud)
{
    return look_previewed(ud);
}

static bool picker_preview(void *ud, int index)
{
    return look_preview(ud, index);
}

static void picker_commit(void *ud)
{
    look_commit(ud);
}

static void picker_cancel(void *ud)
{
    look_cancel(ud);
}

static int picker_rescan(void *ud)
{
    return look_rescan(ud);
}

static struct terminal_theme_picker app_theme_picker(struct app *a)
{
    return (struct terminal_theme_picker){
        .count = picker_count,
        .label = picker_label,
        .selected = picker_selected,
        .previewed = picker_previewed,
        .preview = picker_preview,
        .commit = picker_commit,
        .cancel = picker_cancel,
        .rescan = picker_rescan,
        .ud = a->look,
    };
}

static struct window *open_window(void *ud, struct tab *tab, const char *cwd,
                                  struct vector2i size)
{
    return app_open_terminal(ud, tab, cwd, size);
}

struct window *app_open_terminal(struct app *a, struct tab *tab,
                                 const char *cwd, struct vector2i size)
{
    struct window *w = window_open(&a->host, &(struct window_spec){
        .size = size, .graphics_device = renderer_device(a->renderer),
        .compose = app_compose_frame, .compose_ud = a,
    });
    if (!w)
        return NULL;
    if (!terminal_mount(w, &(struct terminal_spec){
            .config = &a->config.terminal, .tab = tab, .cwd = cwd,
            .theme = look_palette(a->look),
            .theme_picker = app_theme_picker(a),
            .preview_capture = app_thumbnail_capture,
            .preview_release = app_thumbnail_release, .preview_ud = a,
            .scene_paint = app_paint_scene, .scene_paint_ud = a,
            .open_window = open_window, .ud = a })) {
        window_close(w);
        return NULL;
    }
    style_window(a, w);
    return w;
}

static bool settings_apply(void *ud, const struct app_config *draft)
{
    return app_apply_config(ud, draft);
}

static bool settings_save(void *ud, const struct app_config *draft,
                          const char *const *keys, size_t count)
{
    return app_save_config(ud, draft, keys, count);
}

static void open_settings(struct app *a)
{
    struct window *w = settings_find(&a->host);
    if (w) {
        window_redraw(w);
        return;
    }
    w = window_open(&a->host, &(struct window_spec){
        .title = "Settings", .size = { .x = 980, .y = 740 },
        .windowed = true, .graphics_device = renderer_device(a->renderer),
        .compose = app_compose_frame, .compose_ud = a,
    });
    if (w && !settings_mount(w, &(struct settings_host){
            .config = &a->config, .config_path = a->config_path,
            .look = a->look, .apply = settings_apply, .save = settings_save,
            .ud = a,
        })) {
        window_close(w);
    } else if (w) {
        style_window(a, w);
    }
}

static bool host_command(void *ud, struct window *w, enum input_shortcut action)
{
    (void)w;
    switch (action) {
    case INPUT_SHORTCUT_SETTINGS:
        open_settings(ud);
        return true;
    default:
        return false;
    }
}

static void host_empty(void *ud, struct window *last)
{
    struct app *a = ud;
    app_stats_report_final(&a->stats, last);
#ifndef __APPLE__
    yasl_loop_quit(a->loop);
#endif
}

static void host_quit(void *ud)
{
    app_stop(ud);
}

static void host_reopen(void *ud)
{
    struct app *a = ud;
    if (!a->host.count)
        app_open_terminal(a, NULL, NULL, (struct vector2i){0});
}

static bool host_option_set(void *ud, const char *key, const char *value)
{
    struct app *a = ud;
    struct app_config draft = {0};
    if (!opts_copy(&app_config_spec,&draft, &a->config))
        return false;
    bool ok = opts_set(&app_config_spec,&draft, key, value) && app_apply_config(a, &draft);
    opts_destroy(&app_config_spec,&draft);
    return ok;
}

static bool same_look(const struct app_config *a, const struct app_config *b)
{
    const char *ta = a->theme ? a->theme : "", *tb = b->theme ? b->theme : "";
    return !strcmp(ta, tb) && !memcmp(&a->palette, &b->palette, sizeof(a->palette));
}

bool app_apply_config(struct app *a, const struct app_config *draft)
{
    struct app_config next = {0};
    if (!opts_copy(&app_config_spec, &next, draft))
        return false;
    bool new_look = !same_look(&a->config, &next);
    struct app_config previous = a->config;
    struct font_settings pf = app_config_font_settings(&previous);
    a->config = next;
    struct font_settings nf = app_config_font_settings(&a->config);
    font_apply_settings(&pf, &nf);
    if (!new_look) {
        look_changed(a);
    } else if (!look_apply(a->look, &a->config.palette, a->config.theme)) {
        font_apply_settings(&nf, &pf);
        a->config = previous;
        opts_destroy(&app_config_spec, &next);
        return false;
    }
    opts_destroy(&app_config_spec, &previous);
    return true;
}

bool app_save_config(struct app *a, const struct app_config *draft,
                     const char *const *keys, size_t count)
{
    return opts_save(&app_config_spec, draft, a->config_path, keys, count);
}

static void on_signal(void *ud, int signo)
{
    struct app *a = ud;
    if (signo == SIGCHLD) {
        session_reap_exited();
    } else if (signo == SIGUSR1) {
        struct app_config reloaded = {0};
        char *path = NULL;
        if (opts_load(&app_config_spec, &reloaded, a->argc, a->argv, &path) != 0 ||
            !app_apply_config(a, &reloaded))
            fprintf(stderr, "[%s] the config did not reload\n", app_name());
        free(path);
        opts_destroy(&app_config_spec, &reloaded);
    } else {
        fprintf(stderr, "[%s] signal %u, exiting\n", app_name(), (unsigned)signo);
        app_stop(a);
    }
}

bool app_watch_signals(struct app *a)
{
    static const int watched[] = { SIGINT, SIGTERM, SIGCHLD, SIGUSR1 };
    return yasl_loop_watch_signals(a->loop, watched,
                                   (int)(sizeof(watched) / sizeof(watched[0])),
                                   on_signal, a) == 0;
}

static void free_args(char **args)
{
    for (char **p = args; p && *p; p++)
        free(*p);
    free(args);
}

static char **copy_args(int argc, char **argv)
{
    char **copy = calloc((size_t)argc + 1, sizeof(*copy));
    for (int i = 0; copy && i < argc; i++) {
        if (!(copy[i] = strdup(argv[i]))) {
            free_args(copy);
            return NULL;
        }
    }
    return copy;
}

static void app_free(struct app *a)
{
    renderer_destroy(a->renderer);
    a->renderer = NULL;
    if (a->render_started)
        render_shutdown();
    if (a->look) {
        look_set_listener(a->look, NULL, NULL);
        look_destroy(a->look);
    }
    if (a->loop)
        yasl_loop_destroy(a->loop);
    free(a->config_path);
    opts_destroy(&app_config_spec, &a->config);
    free_args(a->argv);
    free(a);
}

struct app *app_create(int argc, char **argv, int *status)
{
    struct app *a = calloc(1, sizeof(*a));
    if (!a) {
        *status = 1;
        return NULL;
    }
    int parsed = opts_load(&app_config_spec, &a->config, argc, argv, &a->config_path);
    if (parsed) {
        *status = parsed == 1 ? 0 : parsed;
        app_free(a);
        return NULL;
    }
    a->argc = argc;
    a->argv = copy_args(argc, argv);
    if (!a->argv) {
        *status = 1;
        app_free(a);
        return NULL;
    }
    struct font_settings fs = app_config_font_settings(&a->config);
    font_apply_settings(NULL, &fs);
    a->render_started = render_init();
    if (!a->render_started) {
        fprintf(stderr, "wsap_term: cannot start the font system\n");
        *status = 1;
        app_free(a);
        return NULL;
    }
    a->renderer = renderer_create(NULL);
    if (!a->renderer) {
        fprintf(stderr, "wsap_term: cannot create the renderer\n");
        *status = 1;
        app_free(a);
        return NULL;
    }
    a->loop = yasl_loop_create();
    if (!a->loop) {
        perror("loop_create");
        *status = 1;
        app_free(a);
        return NULL;
    }
    a->look = look_create(&a->config.palette, a->config.theme);
    if (!a->look) {
        *status = 1;
        app_free(a);
        return NULL;
    }
    window_host_init(&a->host, a->loop, &a->config.window,
                     &(struct window_host_hooks){
                         .empty = host_empty,
                         .quit = host_quit,
                         .reopen = host_reopen,
                         .command = host_command,
                         .ud = a,
                     });
    look_set_listener(a->look, look_changed, a);
    return a;
}

void app_destroy(struct app *a)
{
    if (!a)
        return;
    window_debug_set_hooks(NULL);
    app_stop(a);
    window_host_destroy(&a->host);
    yasl_loop_unwatch_signals(a->loop);
    app_free(a);
}

int app_run(struct app *a)
{
    latency_probe_init();
    if (!app_watch_signals(a) ||
        !app_open_terminal(a, NULL, NULL, (struct vector2i){0}))
        return 1;
    window_debug_set_hooks(&(struct window_debug_hooks){
        .option_set = host_option_set, .ud = a,
    });
    const char *sock = a->config.debug_socket;
    if (sock)
        window_debug_server_start(a->loop, &a->host, sock);
    int result = run_loop(a);
    if (sock)
        window_debug_server_stop();
    return result;
}

static int run_loop(struct app *a)
{
    struct sigaction old_pipe;
    unsigned stats_ms = a->config.stats_ms > 0 ? (unsigned)a->config.stats_ms : 0;
    if (!app_stats_start(&a->stats, a->loop, &a->host, stats_ms))
        return 1;
    if (!signal_ignore(SIGPIPE, &old_pipe)) {
        app_stats_stop(&a->stats);
        return 1;
    }
    int result = yasl_loop_run(a->loop);
    if (result < 0)
        fprintf(stderr, "[%s] loop: %s\n", app_name(), strerror(-result));
    app_stats_stop(&a->stats);
    signal_restore(SIGPIPE, &old_pipe);
    return result < 0 ? 1 : 0;
}

static void app_stop(struct app *a)
{
    while (a->host.count > 0)
        window_close(a->host.windows[a->host.count - 1]);
    yasl_loop_quit(a->loop);
}
