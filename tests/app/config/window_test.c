#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/config/config.h"
#include "app/config/settings.h"
#include "render/scene_paint.h"
#include "theme/look.h"
#include "util/options.h"
#include "util/png.h"
#include "helpers/app_fixture.h"
#include "ui/yasl/client.h"
#include "ui/yasl/testing.h"
#include "ui/debug.h"
#include "ui/ui.h"
#include "ui/yasl/window.h"

static struct ui_widget *find(struct window *window, const char *selector)
{
    struct ui_widget *widget = (struct ui_widget *)ui_debug_find(window_ui_root(window), selector);
    assert(widget);
    return widget;
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

static struct window *open_settings(struct app *app)
{
    struct window *w = settings_find(&app->host);
    if (w)
        return w;
    w = window_create(&app->host, &(struct window_spec){
        .title = "Settings", .size = { .x = 980, .y = 740 }, .windowed = true,
    });
    if (!w)
        return NULL;
    if (!settings_mount(w, &(struct settings_host){
            .config = &app->config, .config_path = app->config_path,
            .look = app->look, .apply = settings_apply, .save = settings_save,
            .ud = app,
        })) {
        window_close(w);
        return NULL;
    }
    window_ui_set_style(w, look_sheet(app->look),
                        look_palette(app->look)->bg);
    return w;
}

static void key(struct window *window, enum ui_key key, unsigned mods)
{
    struct ui_event event = ui_event_key((struct ui_key_event){ .key = key, .mods = mods }, 0);
    assert(ui_surface_dispatch(window_ui_surface(window), &event));
}

static void click(struct window *window, const char *selector)
{
    ui_surface_prepare(window_ui_surface(window));
    struct ui_widget *widget = find(window, selector);
    struct vector2i center = vector2i_add(widget->bounds.pos,
        (struct vector2i){ .x = widget->bounds.width / 2, .y = widget->bounds.height / 2 });
    struct ui_event event = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = center,
    }, 0);
    assert(ui_surface_dispatch(window_ui_surface(window), &event));
    event.data.pointer.kind = UI_POINTER_RELEASE;
    assert(ui_surface_dispatch(window_ui_surface(window), &event));
}

static void edit(struct window *window, const char *selector, const char *value)
{
    ui_surface_focus(window_ui_surface(window), find(window, selector));
    struct ui_event select = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = 'a', .mods = UI_MOD_CTRL,
    }, 0);
    assert(ui_surface_dispatch(window_ui_surface(window), &select));
    struct ui_event text = ui_event_text(value, strlen(value), 0);
    assert(ui_surface_dispatch(window_ui_surface(window), &text));
}

static bool painted(struct window *window, const char *text)
{
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(window_ui_surface(window), &scene));
    bool found = false;
    for (int i = 0; i < scene.n_ops; i++) {
        const struct ui_scene_op *op = &scene.ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == strlen(text) &&
            !memcmp(ui_scene_op_text(&scene, op), text, strlen(text)))
            found = true;
    }
    struct ui_debug_view view = {
        .root = window_ui_root(window), .surface = window_ui_surface(window), .scene = &scene,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    ui_scene_destroy(&scene);
    return found;
}

static void snapshot(struct app *app, struct window *window, const char *name)
{
    const char *dir = getenv("WSAP_UI_SNAPSHOT_DIR");
    if (!dir)
        return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(window_ui_surface(window), &scene));
    struct ui_debug_view view = {
        .root = window_ui_root(window), .surface = window_ui_surface(window), .scene = &scene,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    struct vector2i size = ui_surface_viewport(window_ui_surface(window));
    size_t count = (size_t)size.x * (size_t)size.y;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    assert(pixels);
    uint32_t background = look_palette(app->look)->bg;
    for (size_t i = 0; i < count; i++)
        pixels[i] = background;
    assert(ui_scene_paint(window_ui_font(window), pixels, size.x, size.y, &scene));
    assert(png_write_xrgb(path, pixels, size, size.x));
    free(pixels);
    ui_scene_destroy(&scene);
}

static void editor_resize_test(struct window *window)
{
    struct ui_widget *input = find(window, "#font");
    int width = input->bounds.width;
    ui_surface_resize(window_ui_surface(window), (struct vector2i){ .x = 720, .y = 740 });
    assert(painted(window, "Font family"));
    assert(input->bounds.width > 0 && input->bounds.width < width);
    assert(input->bounds.x >= 0 && input->bounds.x + input->bounds.width <= 720);
    ui_surface_resize(window_ui_surface(window), (struct vector2i){ .x = 1400, .y = 740 });
    assert(painted(window, "Font family"));
    assert(input->bounds.width > width);
    ui_surface_resize(window_ui_surface(window), (struct vector2i){ .x = 980, .y = 740 });
    assert(painted(window, "Font family"));
}

int main(void)
{
    char path[] = "/tmp/wsap-settings-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("# Keep this note.\nfont-size = 10\nunknown-key = keep\n", file) >= 0);
    assert(!fclose(file));
    struct test_app app;
    test_app_init(&app, NULL, (struct vector2i){0});
    app.app->config_path = strdup(path);
    assert(app.app->config_path);
    struct window *window = open_settings(app.app);
    assert(window);
    assert(open_settings(app.app) == window);
    ui_surface_resize(window_ui_surface(window), (struct vector2i){ .x = 980, .y = 740 });
    ui_surface_prepare(window_ui_surface(window));
    assert(painted(window, "Settings"));
    editor_resize_test(window);
    snapshot(app.app, window, "settings");

    edit(window, "#settings-search", "font size");
    assert(painted(window, "Font size"));
    assert(!painted(window, "Foreground"));
    edit(window, "#font-size", "invalid");
    assert(painted(window, "Fix 1 invalid value."));
    assert(app.app->config.window.font_size == 10);
    edit(window, "#font-size", "14");
    click(window, "#settings-save");
    assert(app.app->config.window.font_size == 14);
    file = fopen(path, "r");
    assert(file);
    char contents[512] = {0};
    assert(fread(contents, 1, sizeof(contents) - 1, file) > 0);
    fclose(file);
    assert(strstr(contents, "# Keep this note.\n"));
    assert(strstr(contents, "font-size = 14\n"));
    assert(strstr(contents, "unknown-key = keep\n"));

    free((char *)app.app->config_path);
    app.app->config_path = strdup("/tmp");
    edit(window, "#font-size", "16");
    click(window, "#settings-save");
    assert(app.app->config.window.font_size == 14);
    assert(painted(window, "Save failed: Invalid argument"));
    free((char *)app.app->config_path);
    app.app->config_path = strdup(path);
    edit(window, "#font-size", "14");

    edit(window, "#settings-search", "cursor shape");
    struct ui_widget *dropdown = find(window, "#cursor-style");
    ui_surface_focus(window_ui_surface(window), dropdown->first_child);
    key(window, UI_KEY_ENTER, 0);
    assert(ui_surface_has_modal(window_ui_surface(window)));
    snapshot(app.app, window, "dropdown");
    struct ui_event text = ui_event_text("underline", 9, 0);
    assert(ui_surface_dispatch(window_ui_surface(window), &text));
    key(window, UI_KEY_ENTER, 0);
    assert(!ui_surface_has_modal(window_ui_surface(window)));
    assert(painted(window, "underline  v"));
    click(window, "#settings-save");
    assert(app.app->config.terminal.cursor_style == WSAP_VTCORE_CURSOR_UNDERLINE);

    edit(window, "#settings-search", "cursor shape");
    ui_surface_focus(window_ui_surface(window), find(window, "#cursor-style")->first_child);
    key(window, UI_KEY_ENTER, 0);
    assert(ui_surface_has_modal(window_ui_surface(window)));
    key(window, UI_KEY_ESCAPE, 0);
    assert(!ui_surface_has_modal(window_ui_surface(window)));
    assert(!window_closed(window));

    edit(window, "#settings-search", "no such option");
    assert(painted(window, "No matching settings. Try another search."));
    click(window, "#settings-reset");
    assert(app.app->config.window.font_size == 14);
    click(window, "#settings-cancel");
    window_host_reap(&app.app->host);
    assert(app.app->host.count == 0);
    window = open_settings(app.app);
    assert(window);
    ui_surface_resize(window_ui_surface(window), (struct vector2i){ .x = 720, .y = 520 });
    ui_surface_prepare(window_ui_surface(window));
    edit(window, "#settings-search", "font size");
    assert(painted(window, "14"));
    snapshot(app.app, window, "settings-small");
    struct input_event focus_search = { .kind = INPUT_KEY,
        .key = { .action = INPUT_PRESS, .codepoint = 'f', .mods = INPUT_MOD_CTRL } };
    window_ui_test_route(window, &focus_search);
    assert(ui_surface_focused(window_ui_surface(window)) == find(window, "#settings-search"));
    struct input_event escape = { .kind = INPUT_KEY,
        .key = { .action = INPUT_PRESS, .key = INPUT_KEY_ESCAPE } };
    window_ui_test_route(window, &escape);
    assert(window_closed(window));

    window_host_reap(&app.app->host);
    window = open_settings(app.app);
    assert(window);
    ui_surface_resize(window_ui_surface(window), (struct vector2i){ .x = 980, .y = 740 });
    ui_surface_prepare(window_ui_surface(window));
    edit(window, "#settings-search", "font size");
    edit(window, "#font-size", "18");
    assert(painted(window, "1 unsaved change."));
    struct app_config draft = {0};
    assert(opts_copy(&app_config_spec, &draft, &app.app->config));
    assert(opts_set(&app_config_spec, &draft, "font-size", "16"));
    assert(opts_set(&app_config_spec, &draft, "scrollback", "2500"));
    assert(app_apply_config(app.app, &draft));
    opts_destroy(&app_config_spec, &draft);
    assert(painted(window, "18"));
    assert(painted(window, "1 unsaved change."));
    edit(window, "#settings-search", "history");
    assert(painted(window, "2500"));
    click(window, "#settings-save");
    assert(app.app->config.window.font_size == 18);
    assert(app.app->config.terminal.scrollback == 2500);
    assert(painted(window, "Saved."));
    assert(ui_button_is_disabled((struct ui_button *)find(window, "#settings-save")));
    click(window, "#settings-cancel");
    window_host_reap(&app.app->host);
    assert(app.app->host.count == 0);

    window_host_reap(&app.app->host);
    window = open_settings(app.app);
    assert(window);
    struct input_event close_tab = { .kind = INPUT_SHORTCUT,
        .shortcut = { .action = INPUT_SHORTCUT_CLOSE_TAB } };
    window_ui_test_route(window, &close_tab);
    assert(window_closed(window));
    test_app_fini(&app);
    unlink(path);
    puts("PASS: settings window search, dropdowns, validation, save, reset, and cancel");
    return 0;
}
