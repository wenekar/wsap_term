#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/widgets/scrollview.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "input/input.h"
#include "ui/surface.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "yasl/yasl.h"
#include <wsap_vtcore/wsap_vtcore.h>

/*
 * Paste and drop in the app window. The cases feed native events through
 * window_native_input(), as a backend does.
 *
 * Decision of the codebase owner (2026-11-19):
 * - A paste always goes to the focused widget.
 * - A drop release goes to the widget at its drop location, and it bubbles up
 *   from there. It never falls back to the focused widget. A text drop
 *   follows the same rule as a file drop.
 * - While a modal is open, a release outside the modal reaches nothing.
 * - Every release ends the drag, and it clears the drop highlight of every
 *   tab.
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

static int file_drops;
static bool hover_on;

static void wrap_drop_files(void *content, const char *data, size_t len)
{
    file_drops++;
    app_terminal_content_ops.drop_files(content, data, len);
}

/* The last drop highlight state of each content, by content. */
static struct { void *content; bool over; } highlights[8];

static bool highlighted(void *content)
{
    for (size_t i = 0; i < sizeof highlights / sizeof highlights[0]; i++)
        if (highlights[i].content == content)
            return highlights[i].over;
    return false;
}

static void wrap_drop_hover(void *content, bool over)
{
    size_t i = 0;
    while (i < sizeof highlights / sizeof highlights[0] - 1 &&
           highlights[i].content && highlights[i].content != content)
        i++;
    highlights[i].content = content;
    highlights[i].over = over;
    hover_on = over;
    app_terminal_content_ops.drop_hover(content, over);
}

static const struct vector2i viewport = { .x = 640, .y = 400 };
static const struct vector2i over_terminal = { .x = 320, .y = 300 };

/* Opens an app window with one terminal tab. */
static struct window *open_window(struct app *app)
{
    ops = app_terminal_content_ops;
    ops.paste = wrap_paste;
    ops.drop_files = wrap_drop_files;
    ops.drop_hover = wrap_drop_hover;
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
    file_drops = 0;
    hover_on = false;
    memset(highlights, 0, sizeof highlights);
    return w;
}

/* Runs the animations of the window to their end. */
static void settle(struct window *w)
{
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
}

/* Opens an app window with two terminal tabs, so the tab bar shows. */
static struct window *open_two_tabs(struct app *app)
{
    struct window *w = open_window(app);
    assert(app_tabs_new(w));
    settle(w);
    assert(app_tabs_bar_height(w) > 0);
    return w;
}

/* Returns the center of the tab bar. The hit test there finds the bar. */
static struct vector2i over_tab_bar(struct window *w)
{
    struct ui_widget *bar = ui_tabbar_widget(&app_tabs_of(w)->tabbar);
    struct vector2i pos = rect2i_center(bar->bounds);
    bool on_bar = false;
    for (struct ui_widget *x = ui_surface_hit_test(window_ui_surface(w), pos);
         x; x = x->parent)
        on_bar = on_bar || x == bar;
    assert(on_bar);
    return pos;
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

/* A text drop in the Wayland order: OVER, LEAVE, then the release. */
static void text_drop(struct window *w, const char *data, struct vector2i pos)
{
    drop_event(w, YASL_DROP_OVER, "text/plain", NULL, pos);
    drop_event(w, YASL_DROP_LEAVE, NULL, NULL, pos);
    drop_event(w, YASL_DROP_RELEASE, "text/plain;charset=utf-8", data, pos);
}

/* Sets the keyboard focus of the app window. 0 is no focus, 1 is the tab
 * bar, and 2 is the active terminal. */
static void focus_state(struct window *w, int state)
{
    struct ui_surface *s = window_ui_surface(w);
    struct ui_widget *widget = NULL;
    if (state == 1)
        widget = ui_tabbar_widget(&app_tabs_of(w)->tabbar);
    else if (state == 2)
        widget = terminal_input_widget(app_tabs_active(w));
    ui_surface_focus(s, widget);
    assert(ui_surface_focused(s) == widget);
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
 * into the active tab. The focus does not change this: no focus, the tab
 * bar, or the terminal. */
static void text_drop_on_the_terminal_reaches_it_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_two_tabs(a.app);

    for (int state = 0; state < 3; state++) {
        focus_state(w, state);
        pastes = 0;
        text_drop(w, "td", over_terminal);
        assert(pastes == 1 && !strcmp(last_paste, "td"));
    }

    window_close(w);
    test_app_fini(&a);
}

/* A text drop on the tab bar bubbles up to the tabs root, which pastes it
 * into the active tab, with any focus. */
static void text_drop_on_the_tab_bar_pastes_into_the_terminal_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_two_tabs(a.app);

    for (int state = 0; state < 3; state++) {
        focus_state(w, state);
        pastes = 0;
        text_drop(w, "tb", over_tab_bar(w));
        assert(pastes == 1 && !strcmp(last_paste, "tb"));
    }

    window_close(w);
    test_app_fini(&a);
}

/* The terminal has scrollback, so its scroll bar shows on the right edge. A
 * text drop or a file drop on the bar reaches the active terminal. */
static void drop_on_the_scroll_bar_reaches_the_terminal_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);
    struct ui_surface *s = window_ui_surface(w);
    struct terminal *term = app_tabs_active(w);
    for (int i = 0; i < 200; i++)
        wsap_vtcore_feed(terminal_session(term)->term, "line\r\n", 6, 0);
    struct ui_scrollview *sv = (struct ui_scrollview *)terminal_widget(term);
    ui_scrollview_flash(sv);
    ui_surface_prepare(s);
    assert(ui_scrollview_is_shown(sv));
    struct vector2i on_bar = rect2i_center(sv->bar.bounds);
    assert(ui_surface_hit_test(s, on_bar) == &sv->bar);

    for (int state = 0; state < 3; state += 2) {
        focus_state(w, state);
        pastes = 0;
        text_drop(w, "sb", on_bar);
        assert(pastes == 1 && !strcmp(last_paste, "sb"));
    }
    file_drops = 0;
    drop_event(w, YASL_DROP_OVER, "text/uri-list", NULL, on_bar);
    drop_event(w, YASL_DROP_LEAVE, NULL, NULL, on_bar);
    drop_event(w, YASL_DROP_RELEASE, "text/uri-list", "file:///tmp/a\r\n",
               on_bar);
    assert(file_drops == 1);

    window_close(w);
    test_app_fini(&a);
}

/* The palette is open. A text drop on its input goes to the query. A drop
 * outside the palette reaches nothing: no paste and no file drop reach the
 * terminal under it. */
static void drop_with_the_palette_open_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);
    struct app_tabs *tabs = app_tabs_of(w);
    struct ui_palette_cmd cmds[2] = {
        { .label = "Alpha", .action = 1 }, { .label = "Beta", .action = 2 },
    };
    assert(ui_palette_open(&tabs->palette, cmds, 2));
    ui_surface_prepare(window_ui_surface(w));
    struct ui_widget *palette = ui_palette_widget(&tabs->palette);
    assert(!rect2i_contains(palette->bounds, over_terminal));

    text_drop(w, "pi", rect2i_center(ui_input_widget(&tabs->palette.input)
                                     ->bounds));
    assert(!strcmp(ui_palette_query(&tabs->palette), "pi"));
    assert(pastes == 0);

    text_drop(w, "out", over_terminal);
    drop_event(w, YASL_DROP_RELEASE, "text/uri-list", "file:///tmp/a\r\n",
               over_terminal);
    assert(!strcmp(ui_palette_query(&tabs->palette), "pi"));
    assert(ui_palette_is_open(&tabs->palette));
    assert(pastes == 0 && file_drops == 0);

    ui_palette_close(&tabs->palette);
    window_close(w);
    test_app_fini(&a);
}

/* A file drag over the terminal turns on its drop highlight. The release
 * comes with no LEAVE, as when the LEAVE is lost. The release ends the drag,
 * so the highlight turns off. */
static void release_with_no_leave_clears_the_highlight_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);

    drop_event(w, YASL_DROP_OVER, "text/uri-list", NULL, over_terminal);
    assert(hover_on);
    drop_event(w, YASL_DROP_RELEASE, "text/uri-list", "file:///tmp/a\r\n",
               over_terminal);
    assert(file_drops == 1);
    assert(!hover_on);

    window_close(w);
    test_app_fini(&a);
}

/* A file drag over the first tab highlights it. The active tab changes
 * during the drag, and the release comes with no LEAVE. No tab keeps its
 * highlight. */
static void tab_switch_during_a_drag_clears_every_highlight_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_two_tabs(a.app);
    struct app_tabs *tabs = app_tabs_of(w);
    int first = tabs->active;
    void *first_content = app_tabs_active(w);

    drop_event(w, YASL_DROP_OVER, "text/uri-list", NULL, over_terminal);
    assert(highlighted(first_content));
    app_tabs_select(w, first == 0 ? 1 : 0);
    void *second_content = app_tabs_active(w);
    assert(second_content != first_content);
    drop_event(w, YASL_DROP_RELEASE, "text/uri-list", "file:///tmp/a\r\n",
               over_terminal);
    assert(!highlighted(first_content));
    assert(!highlighted(second_content));

    window_close(w);
    test_app_fini(&a);
}

/* Two stand-ins for native surfaces: the window surface and a menu popup.
 * The test never draws on them. */
static char fake_root_surface, fake_popup_surface;
#define FAKE_ROOT ((struct yasl_surface *)&fake_root_surface)
#define FAKE_POPUP ((struct yasl_surface *)&fake_popup_surface)

/* Sends one native drop event on the fake popup, then routes what it
 * queued. The fake surfaces are gone before the route. */
static void popup_drop_event(struct window *w, enum yasl_drop_action action,
                             const char *data)
{
    assert(window_native_add(w, FAKE_ROOT));
    assert(window_native_add(w, FAKE_POPUP));
    struct yasl_event e = {
        .type = YASL_EVENT_DROP, .action = action, .x = 10, .y = 10,
        .surface = FAKE_POPUP, .text = data,
        .length = data ? strlen(data) : 0,
    };
    snprintf(e.drop.mime, sizeof e.drop.mime, "text/uri-list");
    window_native_input(w, &e);
    window_native_remove(w, FAKE_POPUP);
    window_native_remove(w, FAKE_ROOT);
    drain(w);
}

/* A file drag over the terminal moves onto a menu popup and lands there, as
 * X11 and Cocoa report it. The highlight turns off, and no file drop
 * reaches the terminal. */
static void drop_on_a_popup_clears_the_highlight_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", viewport);
    struct window *w = open_window(a.app);

    drop_event(w, YASL_DROP_OVER, "text/uri-list", NULL, over_terminal);
    assert(hover_on);
    popup_drop_event(w, YASL_DROP_OVER, NULL);
    assert(!hover_on);
    popup_drop_event(w, YASL_DROP_RELEASE, "file:///tmp/a\r\n");
    assert(!hover_on);
    assert(file_drops == 0);

    window_close(w);
    test_app_fini(&a);
}

int main(void)
{
    palette_paste_after_a_lost_drag_test();
    plain_paste_reaches_the_terminal_test();
    text_drop_on_the_terminal_reaches_it_test();
    text_drop_on_the_tab_bar_pastes_into_the_terminal_test();
    drop_on_the_scroll_bar_reaches_the_terminal_test();
    drop_with_the_palette_open_test();
    release_with_no_leave_clears_the_highlight_test();
    tab_switch_during_a_drag_clears_every_highlight_test();
    drop_on_a_popup_clears_the_highlight_test();
    return 0;
}
