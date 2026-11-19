#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "input/input.h"
#include "ui/surface.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "yasl/yasl.h"

/*
 * Paste and drop in the app window. The cases feed native events through
 * window_native_input(), as a backend does.
 *
 * Decision of the codebase owner (2026-11-19):
 * - A paste always goes to the focused widget.
 * - A drop release goes to the widget at its drop location, and it bubbles up
 *   from there. It never falls back to the focused widget.
 * - Every release ends the drag.
 *
 * The terminal content is wrapped, so the test sees what reaches the
 * terminal.
 */

static int pastes;
static char last_paste[64];
static struct app_tab_content_ops ops;

static void wrap_paste(void *content, const char *text, size_t len)
{
    pastes++;
    snprintf(last_paste, sizeof last_paste, "%.*s", (int)len, text);
    app_terminal_content_ops.paste(content, text, len);
}

static const struct vector2i viewport = { .x = 640, .y = 400 };
static const struct vector2i over_terminal = { .x = 320, .y = 300 };

/* Opens an app window with one terminal tab. */
static struct window *open_window(struct app *app)
{
    ops = app_terminal_content_ops;
    ops.paste = wrap_paste;
    struct window *w = window_create(&app->host, &(struct window_spec){0});
    assert(w);
    assert(app_tabs_mount(w, &(struct app_tabs_spec){
        .model = &app->tabs, .create_content = test_terminal_content,
        .create_ud = app, .content_ops = &ops, .empty = true }));
    app_tabs_set_theme(w, &app->palette);
    window_ui_set_style(w, app_style_sheet(), look_sheet(app->look),
                        app->palette.bg);
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(w), viewport);
    assert(app_tabs_new(w));
    ui_surface_prepare(window_ui_surface(w));
    pastes = 0;
    last_paste[0] = 0;
    return w;
}

static void drain(struct window *w)
{
    struct input_event ev;
    while (input_queue_take(&w->input, &ev)) {
        window_ui_route(w, &ev);
        input_event_release(&ev);
    }
}

/* Sends one native drop event at `pos`. A NULL `data` sends no payload. */
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
    drain(w);
}

static void clipboard(struct window *w, const char *text)
{
    window_native_input(w, &(struct yasl_event){
        .type = YASL_EVENT_CLIPBOARD, .action = YASL_TRANSFER_OK,
        .text = text, .length = strlen(text),
    });
    drain(w);
}

/* A file drag over the terminal loses its LEAVE, as when its source dies.
 * Then the palette opens. A paste goes to the palette, which has the focus,
 * and not to the terminal under the old drag. */
static void palette_paste_after_a_lost_drag_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);
    struct app_tabs *tabs = app_tabs_of(w);

    drop_event(w, YASL_DROP_OVER, "text/uri-list", NULL, over_terminal);

    struct ui_palette_cmd cmds[1] = { { .label = "Alpha", .action = 1 } };
    assert(ui_palette_open(&tabs->palette, cmds, 1));
    ui_surface_prepare(window_ui_surface(w));
    assert(ui_surface_focused(window_ui_surface(w)) ==
           ui_input_widget(&tabs->palette.input));

    clipboard(w, "zz");
    assert(!strcmp(ui_palette_query(&tabs->palette), "zz"));
    assert(pastes == 0);

    window_close(w);
    test_app_fini(&a);
}

/* With no drag, a paste reaches the terminal. */
static void plain_paste_reaches_the_terminal_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);

    clipboard(w, "p1");
    assert(pastes == 1 && !strcmp(last_paste, "p1"));

    window_close(w);
    test_app_fini(&a);
}

/* A text drop on the terminal bubbles up to the tabs root, which pastes it
 * into the active tab. */
static void text_drop_on_the_terminal_reaches_it_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);

    drop_event(w, YASL_DROP_OVER, "text/plain", NULL, over_terminal);
    drop_event(w, YASL_DROP_LEAVE, NULL, NULL, over_terminal);
    drop_event(w, YASL_DROP_RELEASE, "text/plain", "td", over_terminal);
    assert(pastes == 1 && !strcmp(last_paste, "td"));

    window_close(w);
    test_app_fini(&a);
}

/* A text drop on the tab bar lands outside the focused terminal. Nothing
 * takes it, and the focused terminal is no fallback. */
static void text_drop_on_the_tab_bar_reaches_nothing_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);
    /* A second tab shows the tab bar. The bar slides in, so the test runs
     * its animation to the end. */
    assert(app_tabs_new(w));
    struct ui_surface *surface = window_ui_surface(w);
    uint64_t now = window_frame_time_ms(w);
    for (uint64_t end = now + 2000; now < end;) {
        ui_surface_tick(surface, now);
        ui_surface_prepare(surface);
        uint64_t next = ui_surface_next_deadline(surface);
        if (!next)
            break;
        now = next > now ? next : now + 16;
    }

    struct ui_widget *bar = ui_tabbar_widget(&app_tabs_of(w)->tabbar);
    struct vector2i over_bar = rect2i_center(bar->bounds);
    assert(app_tabs_bar_height(w) > 0);
    bool on_bar = false;
    for (struct ui_widget *x = ui_surface_hit_test(surface, over_bar); x;
         x = x->parent)
        on_bar = on_bar || x == bar;
    assert(on_bar);
    drop_event(w, YASL_DROP_RELEASE, "text/plain", "tb", over_bar);
    assert(pastes == 0);

    window_close(w);
    test_app_fini(&a);
}

int main(void)
{
    palette_paste_after_a_lost_drag_test();
    plain_paste_reaches_the_terminal_test();
    text_drop_on_the_terminal_reaches_it_test();
    text_drop_on_the_tab_bar_reaches_nothing_test();
    return 0;
}
