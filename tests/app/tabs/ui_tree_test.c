#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "ui/surface.h"
#include "ui/yasl/client.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/testing.h"
#include "yasl/yasl.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "paint/scene.h"
#include "session/session.h"
#include "ui/internal/tree.h"

static void apply_test_scale(struct window *w, struct vector2i viewport)
{
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(w), viewport);
}

static struct window *open_terminal_window(struct test_app *a,
                                           struct vector2i viewport)
{
    test_app_init(a, "/bin/cat", viewport);
    struct window *w = test_terminal_window(a->app);
    assert(w);
    apply_test_scale(w, viewport);
    return w;
}

static void close_terminal_window(struct window *w, struct test_app *a)
{
    window_close(w);
    test_app_fini(a);
}

static void settle_tabbar(struct window *w, struct vector2i viewport)
{
    (void)viewport;
    uint64_t now = window_frame_time_ms(w);
    uint64_t end = now + 2000;
    while (now < end) {
        ui_surface_tick(window_ui_surface(w), now);
        ui_surface_prepare(window_ui_surface(w));
        uint64_t next = ui_surface_next_deadline(window_ui_surface(w));
        if (!next)
            break;
        now = next > now ? next : now + 16;
    }
}

static struct window *open_two_tabs(struct test_app *a, struct vector2i viewport)
{
    struct window *w = open_terminal_window(a, viewport);
    assert(app_tabs_new(w));
    assert(app_tabs_new(w));
    settle_tabbar(w, viewport);
    apply_test_scale(w, viewport);
    return w;
}

static void fill_scrollback(struct window *w)
{
    struct terminal *content = app_tabs_active(w);
    assert(content);
    for (int i = 0; i < 300; i++)
        wsap_vtcore_feed(terminal_session(content)->term, "line\r\n", 6, 0);
}

static int find_ops(const struct ui_scene *whole, const struct ui_scene *part);

static void open_menu(struct window *w, const ui_menu_item *items, int n,
                      struct vector2i at, ui_action_fn fn, void *userdata,
                      ui_menu_submenu_fn submenu)
{
    ui_menu_set_action_handler(window_ui_menu(w), fn, userdata);
    ui_menu_set_submenu_provider(window_ui_menu(w), submenu, NULL);
    assert(ui_menu_open(window_ui_menu(w), items, n));
    ui_menu_layer_set_origin(window_ui_menu_layer(w), at);
    ui_surface_prepare(window_ui_surface(w));
}

static struct vector2i center(struct rect2i r)
{
    return (struct vector2i){ .x = r.x + r.width / 2, .y = r.y + r.height / 2 };
}

/* The view holds the pointer while it selects text. A press that reaches the
 * view captures the pointer on the input widget. */
static bool view_has_pointer(struct window *w, struct terminal *content)
{
    return ui_surface_captured(window_ui_surface(w)) ==
           terminal_input_widget(content);
}

/* The selection spans of the snapshot that the content gives the frame. */
static const struct wsap_vtcore_span *selection_rows(struct terminal *content)
{
    static struct terminal_frame_source source;
    assert(terminal_frame_source(content, 0, &source));
    return wsap_vtcore_snapshot_selections(source.snapshot);
}

static struct ui_event make_pointer(enum ui_pointer_kind kind,
                                     struct vector2i pos, int button)
{
    return ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos, .button = button,
    }, 0);
}

static struct ui_list *menu_list(struct window *w, int depth)
{
    return &window_ui_menu(w)->levels[depth].list;
}

static void composed_root_exists_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_terminal_window(&a, viewport);

    assert(ui_surface_root(window_ui_surface(w)) == window_ui_root(w));

    close_terminal_window(w, &a);
}

static void layout_places_children_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_two_tabs(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    struct terminal *content = app_tabs_active(w);

    int tab_h = app_tabs_bar_height(w);
    assert(tab_h > 0);
    assert(tab_h == ui_tabbar_bar_height(&tabs->tabbar));

    struct ui_widget *tb = ui_tabbar_widget(&tabs->tabbar);
    assert(tb->bounds.x == 0 && tb->bounds.y == 0);
    assert(tb->bounds.width == viewport.x && tb->bounds.height == tab_h);

    struct rect2i want_content = {
        .pos = { .x = 0, .y = tab_h },
        .size = { .x = viewport.x,
                  .y = viewport.y - tab_h },
    };
    struct ui_widget *sv = terminal_widget(content);
    assert(sv->bounds.x == want_content.pos.x);
    assert(sv->bounds.y == want_content.pos.y);
    assert(sv->bounds.width == want_content.size.x);
    assert(sv->bounds.height == want_content.size.y);
    assert(rect2i_eq(terminal_input_widget(content)->bounds, want_content));

    close_terminal_window(w, &a);
}

static void tabbar_scene_in_chrome_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_two_tabs(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    int tab_h = app_tabs_bar_height(w);
    assert(tab_h > 0);

    static struct ui_scene composed, ref;
    assert(ui_surface_build_scene(window_ui_surface(w), &composed));
    assert(ui_widget_build_scene(ui_tabbar_widget(&tabs->tabbar), &ref));
    assert(ref.n_ops > 0);
    assert(find_ops(&composed, &ref) >= 0);
    for (int i = 0; i < ref.n_ops; i++)
        assert(ref.ops[i].rect.y < tab_h);

    close_terminal_window(w, &a);
}

static bool op_equal(const struct ui_scene *sa, const struct ui_scene_op *a,
                     const struct ui_scene *sb, const struct ui_scene_op *b)
{
    if (a->kind != b->kind || a->alpha != b->alpha || a->color != b->color ||
        a->radius != b->radius || !vector2i_eq(a->rect.pos, b->rect.pos) ||
        !vector2i_eq(a->rect.size, b->rect.size))
        return false;
    if (a->kind != UI_SCENE_OP_TEXT)
        return true;
    return a->text_len == b->text_len &&
           !memcmp(ui_scene_op_text(sa, a), ui_scene_op_text(sb, b),
                   a->text_len);
}

static int find_ops(const struct ui_scene *whole, const struct ui_scene *part)
{
    for (int base = 0; base + part->n_ops <= whole->n_ops; base++) {
        int i = 0;
        while (i < part->n_ops &&
               op_equal(whole, &whole->ops[base + i], part, &part->ops[i]))
            i++;
        if (i == part->n_ops)
            return base;
    }
    return -1;
}

static void scrollbar_scene_in_chrome_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    fill_scrollback(w);
    struct terminal *content = app_tabs_active(w);
    terminal_show_scrollbar(content);
    apply_test_scale(w, viewport);
    int tab_h = app_tabs_bar_height(w);

    struct ui_scene prepared = {0};
    assert(ui_surface_build_scene(window_ui_surface(w), &prepared));
    ui_scene_destroy(&prepared);
    static struct ui_scene composed, ref;
    assert(ui_surface_build_scene(window_ui_surface(w), &composed));
    struct ui_widget *sv = terminal_widget(content);
    assert(sv->bounds.y == tab_h);
    assert(ui_widget_build_scene(sv, &ref));
    assert(ref.n_ops > 0);
    assert(find_ops(&composed, &ref) >= 0);
    for (int i = 0; i < ref.n_ops; i++)
        assert(ref.ops[i].rect.y >= tab_h);

    close_terminal_window(w, &a);
}

static void palette_scene_at_rect_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    assert(app_tabs_new(w));

    struct ui_palette_cmd cmds[1] = { { .label = "Test Command", .action = 1 } };
    assert(ui_palette_open(&tabs->palette, cmds, 1));

    static struct ui_scene composed, ref;
    assert(ui_surface_build_scene(window_ui_surface(w), &composed));

    struct rect2i prect = ui_palette_widget(&tabs->palette)->bounds;
    assert(ui_widget_build_scene(ui_palette_widget(&tabs->palette), &ref));
    assert(ref.n_ops > 0);
    int base = find_ops(&composed, &ref);
    assert(base == composed.n_ops - ref.n_ops);
    assert(composed.ops[base].rect.x >= prect.x);
    assert(composed.ops[base].rect.y >= prect.y);

    close_terminal_window(w, &a);
}

static void dispatch_reaches_tabbar_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_two_tabs(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    app_tabs_select(w, 1);
    assert(app_tabs_active_index(w) == 1);

    int tab_h = app_tabs_bar_height(w);
    assert(tab_h > 0);
    struct rect2i tab0_rect;
    assert(ui_tabbar_tab_rect(&tabs->tabbar, 0, &tab0_rect));

    struct ui_event ev = make_pointer(UI_POINTER_PRESS, center(tab0_rect), 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(app_tabs_active_index(w) == 0);

    close_terminal_window(w, &a);
}

static void dispatch_reaches_term_view_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    apply_test_scale(w, viewport);
    struct terminal *content = app_tabs_active(w);
    assert(!view_has_pointer(w, content));

    struct ui_event ev = make_pointer(UI_POINTER_PRESS,
                                       (struct vector2i){ .x = 10, .y = 100 }, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(view_has_pointer(w, content));

    close_terminal_window(w, &a);
}

static void primary_grid_press_selects_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    apply_test_scale(w, viewport);
    struct terminal *content = app_tabs_active(w);

    /* Mouse reporting is off, so a plain press selects text. */
    struct ui_event ev = make_pointer(UI_POINTER_PRESS,
                                       (struct vector2i){ .x = 10, .y = 100 }, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(view_has_pointer(w, content));
    assert(ui_surface_cursor(window_ui_surface(w)) == UI_CURSOR_TEXT);

    close_terminal_window(w, &a);
}

static void terminal_pointer_shape_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    apply_test_scale(w, viewport);
    struct wsap_vtcore *t = terminal_session(app_tabs_active(w))->term;
    static const struct {
        const char *sequence;
        enum ui_cursor cursor;
    } cases[] = {
        { "", UI_CURSOR_TEXT },
        { "\033]22;pointer\033\\", UI_CURSOR_POINTER },
        { "\033]22;default\033\\", UI_CURSOR_DEFAULT },
        { "\033]22;\033\\", UI_CURSOR_TEXT },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        wsap_vtcore_feed(t, cases[i].sequence, strlen(cases[i].sequence), 0);
        struct ui_event ev = make_pointer(UI_POINTER_MOTION,
            (struct vector2i){ .x = 10, .y = 100 }, 0);
        ui_surface_dispatch(window_ui_surface(w), &ev);
        assert(ui_surface_cursor(window_ui_surface(w)) == cases[i].cursor);
    }
    close_terminal_window(w, &a);
}

static bool menu_test_activate(struct ui_widget *source,
                              const struct ui_action *action, void *ud)
{
    (void)source;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    *(int *)ud = action->id;
    return true;
}

static void palette_text_event_updates_query_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    assert(app_tabs_new(w));

    struct ui_palette_cmd cmds[2] = {
        { .label = "Alpha", .action = 1 },
        { .label = "Beta", .action = 2 },
    };
    assert(ui_palette_open(&tabs->palette, cmds, 2));
    struct ui_widget *input = ui_input_widget(&tabs->palette.input);
    ui_surface_prepare(window_ui_surface(w));
    assert(ui_surface_focused(window_ui_surface(w)) == input);

    struct ui_event ev = ui_event_text("z", 1, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(!strcmp(ui_palette_query(&tabs->palette), "z"));

    close_terminal_window(w, &a);
}

static void focused_menu_level_key_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));

    ui_menu_item items[2] = {
        { .label = "Copy", .action = 1 },
        { .label = "Paste", .action = 2 },
    };
    open_menu(w, items, 2, (struct vector2i){ .x = 10, .y = 10 }, NULL, NULL,
              NULL);
    struct ui_widget *lvl = ui_menu_level_widget(window_ui_menu(w), 0);
    assert(ui_surface_focused(window_ui_surface(w)) == lvl);

    struct ui_event ev = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_DOWN }, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(ui_list_selected(menu_list(w, 0)) == 1);

    close_terminal_window(w, &a);
}

static void menu_level_pointer_consumed_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));

    static int activated;
    activated = -1;
    ui_menu_item items[1] = { { .label = "Copy", .action = 7 } };
    open_menu(w, items, 1, (struct vector2i){ .x = 10, .y = 10 },
              menu_test_activate, &activated, NULL);
    struct vector2i at = center(ui_list_item_rect(menu_list(w, 0), 0));
    struct ui_event press = make_pointer(UI_POINTER_PRESS, at, 0);
    struct ui_event release = make_pointer(UI_POINTER_RELEASE, at, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &press));
    assert(ui_surface_dispatch(window_ui_surface(w), &release));
    assert(activated == 7);

    close_terminal_window(w, &a);
}

static void captured_pointer_translation_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_two_tabs(&a, viewport);
    int tab_h = app_tabs_bar_height(w);
    assert(tab_h > 0);
    struct terminal *content = app_tabs_active(w);

    int y0 = tab_h + 8, y1 = tab_h + 24;
    struct ui_event press = make_pointer(UI_POINTER_PRESS,
                                          (struct vector2i){ .x = 10, .y = y0 }, 0);
    struct ui_event move = make_pointer(UI_POINTER_MOTION,
                                         (struct vector2i){ .x = 10, .y = y1 }, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &press));
    assert(view_has_pointer(w, content));
    assert(ui_surface_dispatch(window_ui_surface(w), &move));
    const struct wsap_vtcore_span *spans = selection_rows(content);
    assert(spans);
    assert(spans[0].end > spans[0].begin);
    assert(spans[1].end > spans[1].begin);

    close_terminal_window(w, &a);
}

static const ui_menu_item *menu_test_submenu(void *ud, int action, int *n)
{
    (void)ud; (void)action;
    static const ui_menu_item items[] = {
        { .label = "Sub A", .action = 10 },
        { .label = "Sub B", .action = 11 },
    };
    *n = 2;
    return items;
}

static void menu_hover_reaches_parent_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));

    ui_menu_item items[2] = {
        { .label = "Copy", .action = 1 },
        { .label = "More", .action = 2, .flags = UI_LIST_ITEM_SUBMENU },
    };
    open_menu(w, items, 2, (struct vector2i){ .x = 10, .y = 10 }, NULL, NULL,
              menu_test_submenu);
    ui_list_select(menu_list(w, 0), 1);
    struct ui_event right = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_RIGHT }, 0);
    ui_surface_dispatch(window_ui_surface(w), &right);
    ui_surface_prepare(window_ui_surface(w));
    assert(ui_menu_level_count(window_ui_menu(w)) == 2);

    struct ui_event mv = make_pointer(UI_POINTER_MOTION,
                                       center(ui_list_item_rect(menu_list(w, 0), 0)), 0);
    ui_surface_dispatch(window_ui_surface(w), &mv);
    assert(ui_list_hovered(menu_list(w, 0)) == 0);

    close_terminal_window(w, &a);
}

static void inline_submenu_holds_focus_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));

    ui_menu_item items[2] = {
        { .label = "Copy", .action = 1 },
        { .label = "More", .action = 2, .flags = UI_LIST_ITEM_SUBMENU },
    };
    open_menu(w, items, 2, (struct vector2i){ .x = 10, .y = 10 }, NULL, NULL,
              menu_test_submenu);
    ui_list_select(menu_list(w, 0), 1);
    struct ui_surface *keys = window_ui_key_surface(w);
    assert(keys == window_ui_surface(w));
    struct ui_event right = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_RIGHT }, 0);
    assert(ui_surface_dispatch(keys, &right));
    window_menu_sync(w);
    ui_surface_prepare(window_ui_surface(w));
    assert(ui_menu_level_count(window_ui_menu(w)) == 2);
    assert(window_ui_menu_popups(w) == 0);

    struct ui_widget *top = ui_menu_level_widget(window_ui_menu(w), 1);
    assert(top);
    assert(ui_widget_surface(top) == window_ui_surface(w));
    assert(ui_widget_has_focus(ui_menu_level_widget(window_ui_menu(w), 0)));
    struct ui_event down = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_DOWN }, 0);
    assert(ui_surface_dispatch(keys, &down));
    assert(ui_list_selected(menu_list(w, 1)) == 1);

    window_menu_close(w);
    ui_surface_prepare(window_ui_surface(w));
    assert(ui_surface_focused(window_ui_surface(w)) == NULL);

    close_terminal_window(w, &a);
}

static void close_tab_releases_scrollbar_capture_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    assert(app_tabs_new(w));
    fill_scrollback(w);
    settle_tabbar(w, viewport);
    struct terminal *content = app_tabs_active(w);
    terminal_show_scrollbar(content);
    apply_test_scale(w, viewport);
    int tab_h = app_tabs_bar_height(w);
    assert(tab_h > 0);

    ui_surface_prepare(window_ui_surface(w));
    struct ui_widget *sv = terminal_widget(content);
    assert(sv->bounds.y == tab_h);
    struct ui_event press = make_pointer(UI_POINTER_PRESS,
        (struct vector2i){ .x = rect2i_end(sv->bounds).x - 2,
                           .y = sv->bounds.y + sv->bounds.height / 2 }, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &press));
    assert(ui_surface_captured(window_ui_surface(w)) == sv);

    app_tabs_close(w, app_tabs_active_index(w));
    assert(ui_surface_captured(window_ui_surface(w)) == NULL);
    settle_tabbar(w, viewport);
    app_tabs_service(&a.app->tabs, window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    assert(app_tabs_count(w) == 1);
    assert(ui_surface_captured(window_ui_surface(w)) == NULL);

    struct ui_event move = make_pointer(UI_POINTER_MOTION,
                                         (struct vector2i){ .x = 10, .y = 100 }, 0);
    ui_surface_dispatch(window_ui_surface(w), &move);

    close_terminal_window(w, &a);
}

static void closed_window_frees_queued_paste_test(void)
{
    struct test_app a;
    struct window *w = open_terminal_window(&a, (struct vector2i){ .x = 640, .y = 400 });
    window_native_input(w, &(struct yasl_event){
        .type = YASL_EVENT_CLIPBOARD, .action = YASL_TRANSFER_OK,
        .text = "queued paste", .length = 12,
    });
    close_terminal_window(w, &a);
}

static void right_press_reaches_term_view_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));

    struct ui_event press = make_pointer(UI_POINTER_PRESS,
                                          (struct vector2i){ .x = 10, .y = 100 }, 2);
    assert(ui_surface_dispatch(window_ui_surface(w), &press));

    close_terminal_window(w, &a);
}

static void surface_tick_reaches_tabbar_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_terminal_window(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    assert(app_tabs_new(w));
    assert(app_tabs_new(w));

    uint64_t now = window_frame_time_ms(w);
    int steady = ui_tabbar_steady_height(&tabs->tabbar);
    assert(steady > 0);
    assert(ui_surface_tick(window_ui_surface(w), now + UI_TABBAR_RESIZE_ANIM_MS / 2));
    int growing = ui_tabbar_height(&tabs->tabbar);
    assert(growing > 0 && growing < steady);
    ui_surface_tick(window_ui_surface(w), now + UI_TABBAR_RESIZE_ANIM_MS);
    assert(ui_tabbar_height(&tabs->tabbar) == steady);

    close_terminal_window(w, &a);
}

static void menu_level_fresh_bounds_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);

    ui_menu_item items[1] = { { .label = "Copy", .action = 1 } };
    open_menu(w, items, 1, (struct vector2i){ .x = 100, .y = 100 }, NULL, NULL,
              NULL);
    struct ui_widget *lvl = ui_menu_level_widget(window_ui_menu(w), 0);
    assert(lvl);
    struct rect2i want = lvl->bounds;
    assert(want.x == 100 && want.y == 100);
    assert(want.size.x > 0 && want.size.y > 0);
    struct ui_event ev = make_pointer(UI_POINTER_PRESS, center(want), 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &ev));
    assert(lvl->bounds.pos.x == want.pos.x && lvl->bounds.pos.y == want.pos.y);
    assert(lvl->bounds.size.x == want.size.x &&
           lvl->bounds.size.y == want.size.y);
    assert(!view_has_pointer(w, content));
    ui_menu_close(window_ui_menu(w));

    close_terminal_window(w, &a);
}

static struct ui_widget *rebuild_child(struct ui_widget *parent, struct window *w,
                                       size_t index)
{
    static struct ui_scene scene;
    assert(ui_surface_build_scene(window_ui_surface(w), &scene));
    return ui_widget_child_at(parent, index);
}

static void chrome_is_in_widget_tree_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    assert(app_tabs_new(w));
    assert(app_tabs_new(w));
    settle_tabbar(w, viewport);
    struct terminal *content = app_tabs_active(w);

    struct ui_widget *root = &tabs->root;
    struct ui_widget *palette = ui_palette_widget(&tabs->palette);
    assert(rebuild_child(window_ui_root(w), w, 0) == root);
    struct ui_widget *sv = terminal_widget(content);
    assert(rebuild_child(root, w, 0) == sv);
    assert(rebuild_child(root, w, 1) == ui_tabbar_widget(&tabs->tabbar));
    assert(rebuild_child(sv, w, 0) == terminal_input_widget(content));
    assert(rebuild_child(root, w, 2) == ui_tab_preview_widget(&tabs->preview));
    assert(rebuild_child(root, w, 3) == palette);
    assert(ui_widget_child_count(root) == 4);
    assert(ui_widget_is_hidden(palette));
    struct ui_palette_cmd cmd = { .label = "Test", .action = 1 };
    assert(ui_palette_open(&tabs->palette, &cmd, 1));
    assert(rebuild_child(root, w, 3) == palette && !ui_widget_is_hidden(palette));
    ui_palette_close(&tabs->palette);
    assert(ui_widget_is_hidden(rebuild_child(root, w, 3)));

    app_tabs_select(w, 0);
    struct terminal *first = app_tabs_active(w);
    assert(first != content);
    assert(rebuild_child(sv, w, 0) == terminal_input_widget(content));
    struct ui_widget *swapped = rebuild_child(root, w, 0);
    assert(swapped == terminal_widget(first));
    assert(swapped != sv);

    struct ui_widget *layer = ui_menu_layer_widget(window_ui_menu_layer(w));
    assert(rebuild_child(window_ui_root(w), w, 1) == layer);
    assert(ui_widget_is_hidden(layer));
    ui_menu_item items[1] = { { .label = "Copy", .action = 1 } };
    open_menu(w, items, 1, (struct vector2i){ .x = 10, .y = 10 }, NULL, NULL,
              NULL);
    struct ui_widget *level = ui_menu_level_widget(window_ui_menu(w), 0);
    assert(rebuild_child(layer, w, 0) == level && !ui_widget_is_hidden(layer));

    ui_menu_close(window_ui_menu(w));
    assert(rebuild_child(window_ui_root(w), w, 0) == root);
    assert(ui_widget_child_count(layer) == 0 && ui_widget_is_hidden(layer));

    close_terminal_window(w, &a);
}

static void idle_window_requests_no_frame(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_terminal_window(&a, viewport);
    assert(app_tabs_new(w));
    settle_tabbar(w, viewport);

    static struct ui_scene scene;
    assert(ui_surface_build_scene(window_ui_surface(w), &scene));
    (void)ui_surface_take_frame_request(window_ui_surface(w));
    for (int i = 0; i < 3; i++) {
        assert(ui_surface_build_scene(window_ui_surface(w), &scene));
        assert(!ui_surface_frame_pending(window_ui_surface(w)));
    }
    assert(ui_surface_build_scene(window_ui_surface(w), &scene));
    assert(!ui_surface_frame_pending(window_ui_surface(w)));

    assert(app_tabs_new(w));
    settle_tabbar(w, viewport);
    assert(ui_surface_build_scene(window_ui_surface(w), &scene));
    assert(ui_surface_frame_pending(window_ui_surface(w)));
    close_terminal_window(w, &a);
}

static struct window *open_scrolled_terminal(struct test_app *a,
                                             struct vector2i viewport)
{
    struct window *w = open_terminal_window(a, viewport);
    assert(app_tabs_new(w));
    fill_scrollback(w);
    ui_surface_prepare(window_ui_surface(w));
    return w;
}

static bool send_pointer(struct window *w, enum ui_pointer_kind kind,
                         struct vector2i pos, int button, unsigned mods)
{
    struct ui_event ev = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos, .button = button, .mods = mods,
    }, 0);
    bool handled = ui_surface_dispatch(window_ui_surface(w), &ev);
    ui_surface_prepare(window_ui_surface(w));
    return handled;
}

static struct vector2i bar_point(struct window *w)
{
    struct terminal *content = app_tabs_active(w);
    struct rect2i sv = terminal_widget(content)->bounds;
    return (struct vector2i){ .x = rect2i_end(sv).x - 4,
                              .y = sv.y + sv.height / 2 };
}

static void hover_reveals_bar_in_every_mode_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct terminal *content = app_tabs_active(w);
    struct wsap_vtcore *t = terminal_session(content)->term;
    struct vector2i on_bar = bar_point(w);
    assert(!terminal_scrollbar_visible(content));

    send_pointer(w, UI_POINTER_MOTION, on_bar, 0, 0);
    assert(terminal_scrollbar_visible(content));
    assert(ui_surface_cursor(window_ui_surface(w)) == UI_CURSOR_DEFAULT);
    send_pointer(w, UI_POINTER_LEAVE, on_bar, 0, 0);
    terminal_hide_scrollbar(content);

    const char mouse_any[] = "\x1b[?1003h\x1b[?1006h";
    wsap_vtcore_feed(t, mouse_any, strlen(mouse_any), 0);
    assert(wsap_vtcore_modes(t).mouse != WSAP_VTCORE_MOUSE_OFF);
    send_pointer(w, UI_POINTER_MOTION, on_bar, 0, 0);
    assert(terminal_scrollbar_visible(content));
    send_pointer(w, UI_POINTER_LEAVE, on_bar, 0, 0);
    terminal_hide_scrollbar(content);

    send_pointer(w, UI_POINTER_MOTION, on_bar, 0, UI_MOD_SHIFT);
    assert(terminal_scrollbar_visible(content));

    close_terminal_window(w, &a);
}

static void hidden_bar_press_reaches_view_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct terminal *content = app_tabs_active(w);
    struct ui_widget *view = terminal_input_widget(content);
    assert(!terminal_scrollbar_visible(content));

    assert(send_pointer(w, UI_POINTER_PRESS, bar_point(w), 0, 0));
    assert(ui_surface_captured(window_ui_surface(w)) == view);
    assert(view_has_pointer(w, content));
    assert(!terminal_scrollbar_visible(content));
    assert(wsap_vtcore_scrollback(terminal_session(content)->term).offset == 0);
    send_pointer(w, UI_POINTER_RELEASE, bar_point(w), 0, 0);

    close_terminal_window(w, &a);
}

static void shown_bar_press_scrolls_view_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct terminal *content = app_tabs_active(w);
    struct wsap_vtcore *t = terminal_session(content)->term;
    terminal_show_scrollbar(content);
    ui_surface_prepare(window_ui_surface(w));
    assert(terminal_scrollbar_visible(content));
    assert(wsap_vtcore_scrollback(t).offset == 0);

    struct vector2i top = { .x = bar_point(w).x,
                            .y = terminal_widget(content)->bounds.y + 2 };
    assert(send_pointer(w, UI_POINTER_PRESS, top, 0, 0));
    assert(ui_surface_captured(window_ui_surface(w)) == terminal_widget(content));
    assert(wsap_vtcore_scrollback(t).offset > 0);
    assert(terminal_scrollbar_visible(content));
    send_pointer(w, UI_POINTER_RELEASE, top, 0, 0);

    close_terminal_window(w, &a);
}

static void shown_bar_shift_and_secondary_press_reach_view_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct terminal *content = app_tabs_active(w);
    struct ui_widget *view = terminal_input_widget(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    terminal_show_scrollbar(content);
    ui_surface_prepare(window_ui_surface(w));

    assert(send_pointer(w, UI_POINTER_PRESS, bar_point(w), 0, UI_MOD_SHIFT));
    assert(ui_surface_captured(window_ui_surface(w)) == view);
    assert(view_has_pointer(w, content));
    assert(wsap_vtcore_scrollback(term).offset == 0);
    send_pointer(w, UI_POINTER_RELEASE, bar_point(w), 0, UI_MOD_SHIFT);
    assert(ui_surface_captured(window_ui_surface(w)) == NULL);

    assert(send_pointer(w, UI_POINTER_PRESS, bar_point(w), 2, 0));
    assert(ui_surface_captured(window_ui_surface(w)) != terminal_widget(content));
    assert(wsap_vtcore_scrollback(term).offset == 0);
    send_pointer(w, UI_POINTER_RELEASE, bar_point(w), 2, 0);
    window_menu_close(w);
    ui_surface_prepare(window_ui_surface(w));

    const char mouse_on[] = "\x1b[?1000h\x1b[?1006h";
    wsap_vtcore_feed(term, mouse_on, strlen(mouse_on), 0);
    /* The mouse is reported to the program, so no local text selection. */
    assert(send_pointer(w, UI_POINTER_PRESS, (struct vector2i){ .x = 10, .y = 100 }, 0, 0));
    assert(!view_has_pointer(w, content));
    send_pointer(w, UI_POINTER_RELEASE, (struct vector2i){ .x = 10, .y = 100 }, 0, 0);

    close_terminal_window(w, &a);
}

static void bar_press_keeps_tab_focus_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct app_tabs *tabs = app_tabs_of(w);
    assert(app_tabs_new(w));
    settle_tabbar(w, viewport);
    app_tabs_select(w, 0);
    ui_surface_prepare(window_ui_surface(w));
    struct ui_widget *bar = ui_tabbar_widget(&tabs->tabbar);
    struct terminal *content = app_tabs_active(w);

    ui_surface_focus(window_ui_surface(w), bar);
    terminal_show_scrollbar(content);
    ui_surface_prepare(window_ui_surface(w));
    assert(send_pointer(w, UI_POINTER_PRESS, bar_point(w), 0, 0));
    assert(ui_surface_focused(window_ui_surface(w)) == bar);
    send_pointer(w, UI_POINTER_RELEASE, bar_point(w), 0, 0);

    terminal_hide_scrollbar(content);
    assert(send_pointer(w, UI_POINTER_PRESS, bar_point(w), 0, 0));
    assert(ui_surface_focused(window_ui_surface(w)) == bar);
    send_pointer(w, UI_POINTER_RELEASE, bar_point(w), 0, 0);

    assert(send_pointer(w, UI_POINTER_PRESS, (struct vector2i){ .x = 10, .y = 100 }, 0, 0));
    assert(ui_surface_focused(window_ui_surface(w)) == NULL);
    send_pointer(w, UI_POINTER_RELEASE, (struct vector2i){ .x = 10, .y = 100 }, 0, 0);

    close_terminal_window(w, &a);
}

static void wheel_over_bar_scrolls_view_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct terminal *content = app_tabs_active(w);
    struct wsap_vtcore *t = terminal_session(content)->term;
    assert(!terminal_scrollbar_visible(content));

    struct ui_event wheel = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_SCROLL, .pos = bar_point(w), .scroll_lines = 3,
    }, 0);
    assert(ui_surface_dispatch(window_ui_surface(w), &wheel));
    assert(wsap_vtcore_scrollback(t).offset == 3);
    assert(terminal_scrollbar_visible(content));

    close_terminal_window(w, &a);
}

/* The scrollbar belongs to the content. A tab change shows the bar state of
 * the content that becomes active. */
static void tab_change_hides_bar_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 640, .y = 400 };
    struct window *w = open_scrolled_terminal(&a, viewport);
    struct terminal *first = app_tabs_active(w);
    terminal_show_scrollbar(first);
    assert(terminal_scrollbar_visible(first));

    assert(app_tabs_new(w));
    struct terminal *second = app_tabs_active(w);
    assert(second != first);
    assert(!terminal_scrollbar_visible(second));
    terminal_show_scrollbar(second);
    assert(terminal_scrollbar_visible(second));

    app_tabs_select(w, 0);
    assert(app_tabs_active(w) == first);
    assert(!terminal_scrollbar_visible(second));

    close_terminal_window(w, &a);
}

int main(void)
{
    idle_window_requests_no_frame();
    composed_root_exists_test();
    chrome_is_in_widget_tree_test();
    layout_places_children_test();
    tabbar_scene_in_chrome_test();
    scrollbar_scene_in_chrome_test();
    palette_scene_at_rect_test();
    dispatch_reaches_tabbar_test();
    dispatch_reaches_term_view_test();
    primary_grid_press_selects_test();
    terminal_pointer_shape_test();
    surface_tick_reaches_tabbar_test();
    menu_level_fresh_bounds_test();
    palette_text_event_updates_query_test();
    focused_menu_level_key_test();
    menu_level_pointer_consumed_test();
    captured_pointer_translation_test();
    menu_hover_reaches_parent_test();
    inline_submenu_holds_focus_test();
    close_tab_releases_scrollbar_capture_test();
    closed_window_frees_queued_paste_test();
    right_press_reaches_term_view_test();
    hover_reveals_bar_in_every_mode_test();
    hidden_bar_press_reaches_view_test();
    shown_bar_press_scrolls_view_test();
    shown_bar_shift_and_secondary_press_reach_view_test();
    bar_press_keeps_tab_focus_test();
    wheel_over_bar_scrolls_view_test();
    tab_change_hides_bar_test();
    puts("PASS: window ui tree (composed root, popup surface lifecycle)");
    return 0;
}
