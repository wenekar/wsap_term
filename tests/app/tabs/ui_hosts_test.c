#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "ui/yasl/client.h"
#include "ui/yasl/testing.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "app/tabs/tabs.h"
#include "ui/debug.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/widget.h"
#include "ui/ui.h"

struct fixture {
    struct test_app app;
    struct window *w;
};

static void open_window(struct fixture *f, int tabs)
{
    test_app_init(&f->app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });
    f->w = test_terminal_window(f->app.app);
    assert(f->w);
    ui_surface_set_scale(window_ui_surface(f->w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(f->w), (struct vector2i){ .x = 800, .y = 480 });
    for (int i = 0; i < tabs; i++)
        assert(app_tabs_new(f->w));
}

static void close_window(struct fixture *f)
{
    window_close(f->w);
    test_app_fini(&f->app);
}

static void settle(struct fixture *f)
{
    struct ui_surface *ui = window_ui_surface(f->w);
    uint64_t now = window_frame_time_ms(f->w);
    uint64_t end = now + 2000;
    while (now < end) {
        ui_surface_tick(ui, now);
        ui_surface_prepare(ui);
        uint64_t next = ui_surface_next_deadline(ui);
        if (!next)
            break;
        now = next > now ? next : now + 16;
    }
    app_tabs_prepare(f->w, window_frame_time_ms(f->w) + APP_TABS_CLOSE_MS);
    ui_surface_prepare(ui);
}

static void pointer(struct fixture *f, enum ui_pointer_kind kind,
                    struct vector2i pos, int button)
{
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos, .button = button,
    }, window_frame_time_ms(f->w));
    ui_surface_dispatch(window_ui_surface(f->w), &e);
}

static void click(struct fixture *f, struct vector2i pos, int button)
{
    pointer(f, UI_POINTER_MOTION, pos, button);
    pointer(f, UI_POINTER_PRESS, pos, button);
    pointer(f, UI_POINTER_RELEASE, pos, button);
}

static struct vector2i center(struct rect2i r)
{
    return (struct vector2i){ .x = r.x + r.width / 2, .y = r.y + r.height / 2 };
}

static struct rect2i bar_part(struct fixture *f, const char *name, int index)
{
    const struct ui_widget *bar = ui_debug_find(window_ui_root(f->w), "tabbar");
    assert(bar);
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(bar, &parts);
    struct rect2i rect = {0};
    bool found = false;
    for (int i = 0; i < parts.n; i++) {
        if (parts.items[i].index == index &&
            !strcmp(parts.items[i].name, name)) {
            rect = parts.items[i].rect;
            found = true;
            break;
        }
    }
    ui_debug_parts_free(&parts);
    assert(found);
    return rect;
}

static struct vector2i tab_center(struct fixture *f, int index)
{
    return center(bar_part(f, "tab", index));
}

static void tab_bar_opens_selects_and_closes_tabs_test(void)
{
    struct fixture f;
    open_window(&f, 2);
    settle(&f);

    app_tabs_select(f.w, 0);
    struct terminal *first = app_tabs_active(f.w);
    click(&f, center(bar_part(&f, "tab-new", -1)), 0);
    assert(app_tabs_count(f.w) == 3);
    assert(app_tabs_active_index(f.w) == 2);
    struct terminal *third = app_tabs_active(f.w);
    settle(&f);

    click(&f, tab_center(&f, 0), 0);
    assert(app_tabs_active(f.w) == first);

    click(&f, center(bar_part(&f, "tab-close", 1)), 0);
    assert(app_tabs_count(f.w) == 2);
    settle(&f);
    assert(app_tabs_count(f.w) == 2);
    assert(app_tabs_active(f.w) == first);
    app_tabs_select(f.w, 1);
    assert(app_tabs_active(f.w) == third);
    app_tabs_select(f.w, 0);

    click(&f, tab_center(&f, 1), 1);
    settle(&f);
    assert(app_tabs_count(f.w) == 1);
    close_window(&f);
}

static void dragging_a_tab_reorders_tabs_test(void)
{
    struct fixture f;
    open_window(&f, 3);
    settle(&f);
    app_tabs_select(f.w, 0);
    struct terminal *s0 = app_tabs_active(f.w);
    app_tabs_select(f.w, 1);
    struct terminal *s1 = app_tabs_active(f.w);
    app_tabs_select(f.w, 2);
    struct terminal *s2 = app_tabs_active(f.w);

    struct vector2i from = tab_center(&f, 0);
    struct vector2i to = tab_center(&f, 2);
    pointer(&f, UI_POINTER_MOTION, from, 0);
    pointer(&f, UI_POINTER_PRESS, from, 0);
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = from.x + 20, .y = from.y }, 0);
    pointer(&f, UI_POINTER_MOTION, to, 0);
    pointer(&f, UI_POINTER_RELEASE, to, 0);
    assert(app_tabs_active_index(f.w) == 2);
    assert(app_tabs_active(f.w) == s0);
    app_tabs_select(f.w, 0);
    assert(app_tabs_active(f.w) == s1);
    app_tabs_select(f.w, 1);
    assert(app_tabs_active(f.w) == s2);
    close_window(&f);
}

static void attach_during_close_keeps_live_tabs_clickable_test(void)
{
    struct fixture f;
    open_window(&f, 3);
    settle(&f);
    app_tabs_select(f.w, 1);
    struct terminal *second = app_tabs_active(f.w);
    app_tabs_select(f.w, 0);
    app_tabs_close(f.w, 2);

    struct window *donor = test_terminal_window(f.app.app);
    assert(donor && app_tabs_new(donor));
    assert(app_tabs_move(donor, 0, f.w, 1));
    assert(window_closed(donor));
    assert(app_tabs_count(f.w) == 3);
    app_tabs_select(f.w, 0);
    settle(&f);

    click(&f, tab_center(&f, 2), 0);
    assert(app_tabs_active(f.w) == second);
    close_window(&f);
}

static void scrollbar_scrolls_the_active_tab_test(void)
{
    struct fixture f;
    open_window(&f, 1);
    struct wsap_vtcore *t = terminal_session(app_tabs_active(f.w))->term;
    for (int i = 0; i < 300; i++)
        wsap_vtcore_feed(t, "line\r\n", 6, 0);
    int history = wsap_vtcore_scrollback(t).total;
    assert(history > 0);
    terminal_show_scrollbar(app_tabs_active(f.w));
    ui_surface_prepare(window_ui_surface(f.w));

    struct rect2i view = terminal_widget(app_tabs_active(f.w))->bounds;
    struct vector2i track = { .x = rect2i_end(view).x - 2, .y = view.y + view.height / 2 };
    pointer(&f, UI_POINTER_MOTION, track, 0);
    pointer(&f, UI_POINTER_PRESS, track, 0);
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = track.x, .y = -100 }, 0);
    assert(wsap_vtcore_scrollback(t).offset == history);
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = track.x, .y = 2000 }, 0);
    assert(wsap_vtcore_scrollback(t).offset == 0);
    pointer(&f, UI_POINTER_RELEASE, (struct vector2i){ .x = track.x, .y = 2000 }, 0);
    close_window(&f);
}

static void window_background_matches_terminal_theme_test(void)
{
    struct fixture f;
    open_window(&f, 1);
    struct wsap_vtcore_theme theme;
    wsap_vtcore_theme_default(&theme);
    theme.bg = 0xFF123456u;
    app_tabs_set_theme(f.w, &theme);
    settle(&f);
    assert(window_background(f.w) == 0xFF123456u);

    theme.bg = 0xFF654321u;
    app_tabs_set_theme(f.w, &theme);
    settle(&f);
    assert(window_background(f.w) == 0xFF654321u);
    close_window(&f);
}

int main(void)
{
    tab_bar_opens_selects_and_closes_tabs_test();
    dragging_a_tab_reorders_tabs_test();
    attach_during_close_keeps_live_tabs_clickable_test();
    scrollbar_scrolls_the_active_tab_test();
    window_background_matches_terminal_theme_test();
    puts("PASS: the app tab bar and terminal scrollbar handle input");
    return 0;
}
