#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "ui/yasl/client.h"
#include "ui/yasl/testing.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "terminal/internal.h"
#include "ui/ui.h"

struct fixture {
    struct test_app app;
    struct window *w;
    struct terminal *tw;
};

static void open_window(struct fixture *f, int tabs)
{
    test_app_init(&f->app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });
    f->w = test_terminal_window(&f->app.app->host, &f->app.app->config.terminal);
    assert(f->w);
    ui_surface_set_scale(window_ui_surface(f->w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(f->w), (struct vector2i){ .x = 800, .y = 480 });
    f->tw = terminal_of(f->w);
    for (int i = 0; i < tabs; i++)
        assert(terminal_new_tab(f->w));
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
    terminal_finish_closing_tab(f->w);
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

static struct vector2i tab_center(struct fixture *f, int index)
{
    struct rect2i r;
    assert(ui_tabbar_tab_rect(&f->tw->tabbar, index, &r));
    return center(r);
}

static void tab_bar_opens_selects_and_closes_tabs_test(void)
{
    struct fixture f;
    open_window(&f, 2);
    settle(&f);

    struct rect2i plus;
    assert(ui_tabbar_new_tab_rect(&f.tw->tabbar, &plus));
    click(&f, center(plus), 0);
    assert(terminal_tab_count(f.w) == 3);
    assert(terminal_active_index(f.w) == 2);
    settle(&f);

    click(&f, tab_center(&f, 0), 0);
    assert(terminal_active_index(f.w) == 0);

    struct tab *doomed = f.tw->tabs[1];
    struct rect2i close;
    assert(ui_tabbar_close_rect(&f.tw->tabbar, 1, &close));
    click(&f, center(close), 0);
    assert(ui_tabbar_closing_tab(&f.tw->tabbar) == 1);
    settle(&f);
    assert(terminal_tab_count(f.w) == 2);
    assert(f.tw->tabs[0] != doomed && f.tw->tabs[1] != doomed);

    click(&f, tab_center(&f, 1), 1);
    settle(&f);
    assert(terminal_tab_count(f.w) == 1);
    close_window(&f);
}

static void dragging_a_tab_reorders_tabs_test(void)
{
    struct fixture f;
    open_window(&f, 3);
    settle(&f);
    struct tab *s0 = f.tw->tabs[0], *s1 = f.tw->tabs[1], *s2 = f.tw->tabs[2];

    struct vector2i from = tab_center(&f, 0);
    struct vector2i to = tab_center(&f, 2);
    pointer(&f, UI_POINTER_MOTION, from, 0);
    pointer(&f, UI_POINTER_PRESS, from, 0);
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = from.x + 20, .y = from.y }, 0);
    pointer(&f, UI_POINTER_MOTION, to, 0);
    pointer(&f, UI_POINTER_RELEASE, to, 0);
    assert(f.tw->tabs[0] == s1);
    assert(f.tw->tabs[1] == s2);
    assert(f.tw->tabs[2] == s0);
    assert(terminal_active_index(f.w) == 2);
    close_window(&f);
}

static void scrollbar_scrolls_the_active_tab_test(void)
{
    struct fixture f;
    open_window(&f, 1);
    struct wsap_vtcore *t = terminal_term(f.w);
    for (int i = 0; i < 300; i++)
        wsap_vtcore_feed(t, "line\r\n", 6, 0);
    int history = wsap_vtcore_scrollback(t).total;
    assert(history > 0);
    terminal_show_scrollbar(f.w);
    ui_surface_prepare(window_ui_surface(f.w));

    struct rect2i view = terminal_view_rect(f.w);
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

int main(void)
{
    tab_bar_opens_selects_and_closes_tabs_test();
    dragging_a_tab_reorders_tabs_test();
    scrollbar_scrolls_the_active_tab_test();
    puts("PASS: the terminal's tab bar and scrollbar act on its tabs");
    return 0;
}
