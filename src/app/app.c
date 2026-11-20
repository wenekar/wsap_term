#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app/app.h"
#include "app/config/settings.h"
#include "app/style.h"
#include "app/thumbnail.h"
#include "runtime/runtime.h"
#include "loop/latency_probe.h"
#include "yasl/yasl.h"
#include "loop/signals.h"
#include "theme/look.h"
#include "paint/scene.h"
#include "render/render.h"
#include "terminal/render/term_frame.h"
#include "util/damage.h"
#include "session/session.h"
#include "app/tabs/tabs.h"
#include "app/tabs/terminal_content.h"
#include "terminal/terminal.h"
#include "terminal/render/term_render.h"
#include "util/app_identity.h"
#include "app/identity.h"

const char *app_name(void)
{
    const struct app_identity *identity = app_identity_current();
    return identity ? identity->id : "";
}

#include "ui/surface.h"
#include "ui/widget.h"
#include "ui/yasl/inspector.h"
#include "ui/yasl/client.h"

static void app_stop(struct app *a);
static int run_loop(struct app *a);

static bool cursor_pixels_fit(struct app *a, size_t need_px)
{
    if (need_px <= a->cursor_px_cap)
        return true;
    uint32_t *next = realloc(a->cursor_px, need_px * sizeof(*next));
    if (!next)
        return false;
    a->cursor_px = next;
    a->cursor_px_cap = need_px;
    return true;
}

static void update_cursor_overlay(struct app *a, struct window *w,
                                  struct terminal *content,
                                  const struct terminal_frame_source *source,
                                  const struct window_ui_frame *ui,
                                  struct vector2i canvas_size, uint64_t now_ms,
                                  struct terminal_frame *grid)
{
    struct terminal_cursor_overlay co = {0};
    bool ok = !ui->gpu_off && !ui->suspended && content &&
              window_cursor_overlay_supported(w) &&
              terminal_cursor_overlay_prepare(content, source, canvas_size, now_ms, &co) &&
              !ui_scene_intersects_rect(ui->scene, co.rect);
    if (!ok) {
        window_set_cursor_overlay(w, &(struct window_cursor_overlay){ .active = false });
        terminal_frame_init(grid, source->snapshot, source->cursor_visible);
        return;
    }

    const uint32_t *pixels = NULL;
    struct terminal_frame cursor_grid = {0};
    if (!co.solid_only) {
        size_t need = (size_t)co.rect.width * (size_t)co.rect.height;
        int bw = 0, bh = 0;
        terminal_frame_init(&cursor_grid, source->snapshot, true);
        if (!cursor_pixels_fit(a, need) ||
            !render_cursor_cell(a->renderer, source->font, &cursor_grid,
                                co.span_begin, co.span_cols,
                                a->cursor_px, a->cursor_px_cap, &bw, &bh) ||
            bw != co.rect.width || bh != co.rect.height) {
            window_set_cursor_overlay(w, &(struct window_cursor_overlay){ .active = false });
            terminal_frame_init(grid, source->snapshot, source->cursor_visible);
            return;
        }
        pixels = a->cursor_px;
    }

    if (window_set_cursor_overlay(w, &(struct window_cursor_overlay){
            .active = true,
            .visible = co.visible,
            .rect = co.rect,
            .color = co.color,
            .pixels = pixels,
            .key = co.cell_key,
        })) {
        terminal_frame_init(grid, source->snapshot, false);
        return;
    }

    terminal_frame_init(grid, source->snapshot, source->cursor_visible);
}

static void app_compose_frame(struct window *w, struct window_frame *frame,
                              void *ud)
{
    struct app *a = ud;
    uint64_t now_ms = window_frame_time_ms(w);
    bool main = frame->surface == window_surface(w);
    struct terminal *content = NULL;
    struct rect2i content_bounds = {0};
    if (main) {
        latency_probe_stage(LATENCY_STAGE_RENDER_START);
        /* The service step of the host pass already applied the tab model,
         * with the time of this frame. The UI layout comes first, then the
         * content geometry, and only then the snapshot. Nothing in a frame
         * closes a window or changes the tab model. */
        /* Settle pending stylesheet or root invalidations before taking UI
         * frame requests so initial styling does not schedule a second pass. */
        window_ui_prepare_frame(w, now_ms);
        ui_surface_prepare(window_ui_surface(w));
        content = app_tabs_active(w);
        if (content) {
            terminal_commit_geometry(content);
            terminal_prepare(content, now_ms);
            content_bounds = terminal_input_widget(content)->bounds;
        }
    }

    struct window_ui_frame ui = {0};
    if (!window_ui_build_frame(w, frame->surface, now_ms, &ui))
        ui.repaint = true;
    /* Scene preparation can finish a deferred layout pass. Apply the new
     * content rectangle in this frame, so the snapshot matches the scene. */
    if (content) {
        struct rect2i settled = terminal_input_widget(content)->bounds;
        if (!rect2i_eq(content_bounds, settled)) {
            content_bounds = settled;
            terminal_commit_geometry(content);
            terminal_prepare(content, now_ms);
            if (!window_ui_build_frame(w, frame->surface, now_ms, &ui))
                ui.repaint = true;
        }
    }

    struct render_frame render = {
        .base = ui.background,
        .ui_font = ui.font,
        .cpu_only = ui.gpu_off,
    };
    struct terminal_frame grid;
    if (ui.suspended) {
        render.cpu_only = true;
        if (main)
            window_set_cursor_overlay(w, &(struct window_cursor_overlay){ .active = false });
    } else {
        render.ui = ui.scene;
        render.ui_damage = main ? &ui.damage : NULL;
        render.repaint = ui.repaint;
        struct terminal_frame_source source = {0};
        if (content && terminal_frame_source(content, now_ms, &source)) {
            /* The terminal theme has no selection colors, so the zero values
             * from terminal_frame_init select the built-in ones. */
            if (main) {
                update_cursor_overlay(a, w, content, &source, &ui,
                                      frame->target.size, now_ms, &grid);
            } else {
                terminal_frame_init(&grid, source.snapshot, source.cursor_visible);
            }
            grid.blink_on = source.text_blink_on;
            render.grid = (struct render_grid){
                .frame = &grid,
                .font = source.font,
                .rect = source.rect,
            };
        } else if (main) {
            window_set_cursor_overlay(w, &(struct window_cursor_overlay){ .active = false });
        }
    }
    struct render_result result = render_draw(a->renderer, &frame->target, &render);
    if (result.presented && !render.cpu_only)
        app_runtime_note_gpu_draw(a->runtime, now_ms);
    if (window_present(w, frame, &result.damage, result.presented) && main)
        latency_probe_complete();
    window_ui_frame_done(w, frame, now_ms);

    if (main)
        app_runtime_note_first_frame(a->runtime);
}

bool app_thumbnail_capture(void *ud, void **state, struct font_instance *font,
                           struct wsap_vtcore *term, int width, struct ui_image *image)
{
    struct app *a = ud;
    return app_runtime_thumbnail_capture(a ? a->runtime : NULL, state, font,
                                         term, width, image);
}

void app_thumbnail_release(void *state)
{
    app_runtime_thumbnail_release(state);
}

static void style_window(struct app *a, struct window *w)
{
    if (app_tabs_is(w))
        app_tabs_set_theme(w, &a->palette);
    window_ui_set_style(w, app_style_sheet(), look_sheet(a->look),
                        a->palette.bg);
}

/* The `ansi` keys of a theme data block. The theme module is generic and
   does not know them. */
static const char *const ansi_keys[] = {
    "foreground", "background",
    "color0", "color1", "color2", "color3", "color4", "color5", "color6",
    "color7", "color8", "color9", "color10", "color11", "color12", "color13",
    "color14", "color15",
};

static void refresh_palette(struct app *a)
{
    struct wsap_vtcore_theme palette = a->config.palette;
    for (size_t i = 0; i < sizeof(ansi_keys) / sizeof(ansi_keys[0]); i++) {
        const char *value = look_theme_value(a->look, "ansi", ansi_keys[i]);
        if (value && !wsap_vtcore_theme_set_key(&palette, ansi_keys[i], value))
            fprintf(stderr, "[%s] theme: bad 'ansi' value for %s: %s\n",
                    app_name(), ansi_keys[i], value);
    }
    a->palette = palette;
}

static void look_changed(void *ud)
{
    struct app *a = ud;
    refresh_palette(a);
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

static struct window *open_window(void *ud, app_tab_id tab, const char *cwd,
                                  struct vector2i size)
{
    return app_open_terminal(ud, tab, cwd, size);
}

static bool app_wants_cpu(const struct app *a)
{
    if (a->config.lightweight || a->config.renderer == APP_RENDERER_CPU)
        return true;
    const char *env_gpu = getenv("WSAP_RENDER_GPU");
    if (env_gpu && (!strcmp(env_gpu, "0") || !strcasecmp(env_gpu, "off")))
        return true;
    const char *env_rend = getenv("WSAP_RENDERER");
    if (env_rend && (!strcasecmp(env_rend, "cpu") || !strcasecmp(env_rend, "shm")))
        return true;
    return false;
}

static void *app_acquire_device(void *ud)
{
    struct app *a = ud;
    if (app_wants_cpu(a))
        return NULL;
    return renderer_device(a->renderer);
}

static void app_drop_unused_gpu(struct app *a, const struct window *w)
{
    if (!w || window_has_gpu(w))
        return;
    for (int i = 0; i < a->host.count; i++)
        if (window_has_gpu(a->host.windows[i]))
            return;
    renderer_drop_gpu(a->renderer);
}

/* Creates the terminal content of a new tab. The tab model owns the terminal,
 * and the terminal keeps the adapter state. */
static void *app_create_terminal_content(void *ud, struct window *window,
                                         const char *cwd,
                                         struct vector2i content_size,
                                         int font_size)
{
    struct app *a = ud;
    return app_terminal_content_create(window, &a->tabs, cwd, content_size,
        font_size, &(struct app_terminal_content_spec){
            .config = &a->config.terminal,
            .theme = &a->palette,
            .theme_picker = app_theme_picker(a),
            .preview_capture = app_thumbnail_capture,
            .preview_ud = a,
        });
}

struct window *app_open_terminal(struct app *a, app_tab_id tab,
                                 const char *cwd, struct vector2i size)
{
    bool cpu_mode = app_wants_cpu(a);
    struct window *w = window_open(&a->host, &(struct window_spec){
        .size = size,
        .acquire_device = cpu_mode ? NULL : app_acquire_device, .device_ud = a,
        .compose = app_compose_frame, .compose_ud = a,
    });
    if (!w)
        return NULL;
    app_drop_unused_gpu(a, w);
    if (!app_tabs_mount(w, &(struct app_tabs_spec){
            .cwd = cwd,
            .model = &a->tabs, .tab = tab,
            .theme = &a->palette,
            .create_content = app_create_terminal_content, .create_ud = a,
            .content_ops = &app_terminal_content_ops,
            .preview_release = app_thumbnail_release, .preview_ud = a,
            .scene_paint = app_runtime_paint_scene, .scene_paint_ud = a->runtime,
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
    bool cpu_mode = app_wants_cpu(a);
    w = window_open(&a->host, &(struct window_spec){
        .title = "Settings", .size = { .x = 980, .y = 740 },
        .windowed = true,
        .acquire_device = cpu_mode ? NULL : app_acquire_device, .device_ud = a,
        .compose = app_compose_frame, .compose_ud = a,
    });
    app_drop_unused_gpu(a, w);
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

static bool host_command(void *ud, struct window *w, input_command_id action)
{
    (void)w;
    switch (action) {
    case INPUT_COMMAND_SETTINGS:
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

/* The tab model changes here, outside every frame and every content
 * callback. Retirement opens and closes no window, so a modal step runs it
 * too. */
static void host_service(void *ud, uint64_t now_ms, bool modal)
{
    (void)modal;
    struct app *a = ud;
    app_tabs_service(&a->tabs, now_ms);
}

static bool host_wake_window(void *ud, struct window *w, uint64_t now_ms)
{
    (void)ud;
    if (!window_cursor_overlay_active(w) || window_has_popup(w) ||
        ui_surface_has_modal(window_ui_surface(w)))
        return false;
    struct terminal *content = app_tabs_active(w);
    bool visible = false;
    uint64_t next_ms = 0;
    if (!terminal_cursor_blink_only(content, now_ms, &visible, &next_ms))
        return false;
    if (!window_cursor_overlay_visible(w, visible))
        return false;
    window_wake_at(w, next_ms);
    return true;
}

static void host_reopen(void *ud)
{
    struct app *a = ud;
    if (!a->host.count)
        app_open_terminal(a, 0, NULL, (struct vector2i){0});
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
    } else if (!look_apply(a->look, a->config.theme)) {
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
        /* input.conf is a separate file, so it reloads even when the app
         * config reload fails. */
        window_host_reload_input(&a->host);
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
    app_runtime_destroy(a->runtime);
    a->runtime = NULL;
    if (a->renderer) {
        renderer_destroy(a->renderer);
        a->renderer = NULL;
    }
    if (a->render_started)
        render_shutdown();
    if (a->look) {
        look_set_listener(a->look, NULL, NULL);
        look_destroy(a->look);
    }
    if (a->loop)
        yasl_loop_destroy(a->loop);
    free(a->cursor_px);
    free(a->config_path);
    opts_destroy(&app_config_spec, &a->config);
    free_args(a->argv);
    free(a);
}

static struct app *app_new(int argc, char **argv, bool gpu_first, int *status)
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
    /* The renderer comes before the font system, so a GPU device can start
     * while the process holds little else. */
    bool cpu_mode = app_wants_cpu(a);
    struct render_options ro = {
        .disable_gpu = cpu_mode,
    };
    if (a->config.lightweight) {
        ro.cache_bytes = (ptrdiff_t)1 << 20;
        ro.threads = 1;
        if (a->config.terminal.scrollback == WSAP_VTCORE_SCROLLBACK_DEFAULT)
            a->config.terminal.scrollback = 2000;
    }
    a->renderer = renderer_create(&ro);
    if (!a->renderer) {
        fprintf(stderr, "wsap_term: cannot create the renderer\n");
        *status = 1;
        app_free(a);
        return NULL;
    }
    /* A failed start is not an error: the first window then gets a CPU
     * surface, as it does when renderer_device() fails. */
    if (gpu_first)
        renderer_start_gpu(a->renderer);
    struct font_settings fs = app_config_font_settings(&a->config);
    font_apply_settings(NULL, &fs);
    a->render_started = render_init();
    if (!a->render_started) {
        fprintf(stderr, "wsap_term: cannot start the font system\n");
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
    a->runtime = app_runtime_create(a->renderer, a->loop);
    if (!a->runtime) {
        *status = 1;
        app_free(a);
        return NULL;
    }
    a->look = look_create(app_identity_current(), a->config.theme);
    if (!a->look) {
        *status = 1;
        app_free(a);
        return NULL;
    }
    refresh_palette(a);
    window_host_init(&a->host, a->loop, &a->config.window,
                     &(struct window_host_hooks){
                         .empty = host_empty,
                         .quit = host_quit,
                         .reopen = host_reopen,
                         .command = host_command,
                         .service = host_service,
                         .wake = host_wake_window,
                         .ud = a,
                     });
    look_set_listener(a->look, look_changed, a);
    return a;
}

struct app *app_create(int argc, char **argv, int *status)
{
    return app_new(argc, argv, false, status);
}

struct app *app_create_gpu_first(int argc, char **argv, int *status)
{
    return app_new(argc, argv, true, status);
}

void app_destroy(struct app *a)
{
    if (!a)
        return;
    window_debug_set_hooks(NULL);
    app_stop(a);
    window_host_reap(&a->host);
    if (a->renderer) {
        renderer_destroy(a->renderer);
        a->renderer = NULL;
    }
    /* The host reaps the windows, and each window gives its tabs back to the
     * model. A destroying host runs no more service steps. The model then
     * destroys what is left, such as the tab of a drag that did not end. */
    window_host_destroy(&a->host);
    app_tab_model_fini(&a->tabs);
    yasl_loop_unwatch_signals(a->loop);
    app_free(a);
}

int app_run(struct app *a)
{
    latency_probe_init();
    if (!app_watch_signals(a) ||
        !app_open_terminal(a, 0, NULL, (struct vector2i){0}))
        return 1;
    window_debug_set_hooks(&(struct window_debug_hooks){
        .option_set = host_option_set, .ud = a,
    });
    const char *sock = a->config.debug_socket;
    if (sock)
        window_debug_server_start(a->loop, &a->host, sock);
    app_runtime_trim_heap();
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
