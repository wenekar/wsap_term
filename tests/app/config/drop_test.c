#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/config/config.h"
#include "app/config/settings.h"
#include "app/style.h"
#include "helpers/app_fixture.h"
#include "input/input.h"
#include "theme/look.h"
#include "ui/debug.h"
#include "ui/surface.h"
#include "ui/widgets/input.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"

/*
 * Text drops in the settings window. A text drop goes to the input at its
 * drop location, whatever has the focus. The cases feed native events
 * through window_native_input(), as a backend does.
 */

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
    struct window *w = window_create(&app->host, &(struct window_spec){
        .title = "Settings", .size = { .x = 980, .y = 740 }, .windowed = true,
    });
    assert(w);
    assert(settings_mount(w, &(struct settings_host){
        .config = &app->config, .config_path = app->config_path,
        .look = app->look, .apply = settings_apply, .save = settings_save,
        .ud = app,
    }));
    window_ui_set_style(w, app_style_sheet(), look_sheet(app->look),
                        app->palette.bg);
    ui_surface_resize(window_ui_surface(w),
                      (struct vector2i){ .x = 980, .y = 740 });
    ui_surface_prepare(window_ui_surface(w));
    return w;
}

static struct ui_input *find_input(struct window *w, const char *selector)
{
    struct ui_widget *widget =
        (struct ui_widget *)ui_debug_find(window_ui_root(w), selector);
    assert(widget);
    return (struct ui_input *)widget;
}

static struct vector2i center_of(struct window *w, struct ui_input *input)
{
    ui_surface_prepare(window_ui_surface(w));
    return rect2i_center(ui_input_widget(input)->bounds);
}

static void drop_event(struct window *w, enum yasl_drop_action action,
                       const char *mime, const char *data, struct vector2i pos)
{
    struct yasl_event e = {
        .type = YASL_EVENT_DROP, .action = action, .x = pos.x, .y = pos.y,
        .text = data, .length = data ? strlen(data) : 0,
    };
    if (mime)
        snprintf(e.drop.mime, sizeof e.drop.mime, "%s", mime);
    window_native_input(w, &e);
    struct input_event ev;
    while (input_queue_take(&w->input, &ev)) {
        window_ui_route(w, &ev);
        input_event_release(&ev);
    }
}

/* A text drop in the Wayland order: OVER, LEAVE, then the release. */
static void text_drop(struct window *w, const char *data, struct vector2i pos)
{
    drop_event(w, YASL_DROP_OVER, "text/plain", NULL, pos);
    drop_event(w, YASL_DROP_LEAVE, NULL, NULL, pos);
    drop_event(w, YASL_DROP_RELEASE, "text/plain;charset=utf-8", data, pos);
}

/* Nothing has the focus. A text drop on the search input goes to it. */
static void text_drop_on_search_with_no_focus_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", (struct vector2i){ .x = 640, .y = 400 });
    struct window *w = open_settings(a.app);
    struct ui_input *search = find_input(w, "#settings-search");
    ui_surface_focus(window_ui_surface(w), NULL);

    text_drop(w, "font", center_of(w, search));
    assert(!strcmp(ui_input_text(search), "font"));

    window_close(w);
    test_app_fini(&a);
}

/* The search filters the list to the font settings, so the font size input
 * is in view. Then the search input has the focus. A text drop on the font
 * size input goes to that input, not to the search. Then the font size
 * input has the focus, and a text drop on the search goes to the search. */
static void text_drop_goes_to_the_input_at_its_location_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", (struct vector2i){ .x = 640, .y = 400 });
    struct window *w = open_settings(a.app);
    struct ui_surface *s = window_ui_surface(w);
    struct ui_input *search = find_input(w, "#settings-search");
    struct ui_input *size = find_input(w, "#font-size");
    char want[80];
    snprintf(want, sizeof want, "%s9", ui_input_text(size));

    ui_surface_focus(s, ui_input_widget(search));
    text_drop(w, "font size", center_of(w, search));
    assert(!strcmp(ui_input_text(search), "font size"));
    struct vector2i on_size = center_of(w, size);
    assert(ui_surface_hit_test(s, on_size) == ui_input_widget(size));

    text_drop(w, "9", on_size);
    assert(!strcmp(ui_input_text(size), want));
    assert(!strcmp(ui_input_text(search), "font size"));

    ui_surface_focus(s, ui_input_widget(size));
    text_drop(w, "!", center_of(w, search));
    assert(!strcmp(ui_input_text(search), "font size!"));
    assert(!strcmp(ui_input_text(size), want));

    window_close(w);
    test_app_fini(&a);
}

int main(void)
{
    text_drop_on_search_with_no_focus_test();
    text_drop_goes_to_the_input_at_its_location_test();
    return 0;
}
