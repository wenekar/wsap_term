#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui/damage.h"
#include "ui/debug.h"
#include "paint/scene.h"
#include "ui/ui.h"
#include "ui/widget_impl.h"
#include <wsap_vtcore/unicode/utf8.h>

#ifdef __APPLE__
#define TAB_BADGE "\xe2\x8c\x98"
#else
#define TAB_BADGE "^"
#endif
#define CLOSE_MARK "\xc3\x97"

#define MAX_ACTIONS 256
#define MAX_TABS 64

static const struct css_scale_context SCALE = {
    .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
};

/* A tab bar at the top of a root, at origin, as tall as it measures. The
 * rest of the surface is empty. */
struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_tabbar tb;
    struct ui_scene scene;
    struct vector2i origin;
    struct ui_action log[MAX_ACTIONS];
    int n_log;
    /* The handler takes a UI_ACTION_TEAR_OFF. */
    bool take_tear_off;
    struct ui_tab_label labels[MAX_TABS];
    char names[MAX_TABS][16];
};

static struct vector2i root_measure(const struct ui_widget *widget,
                                    struct ui_constraints constraints)
{
    struct fixture *f = widget->userdata;
    ui_widget_measure(ui_tabbar_widget(&f->tb),
                      (struct ui_constraints){ .max = constraints.max });
    return ui_constraints_clamp(constraints, constraints.max);
}

static void root_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct fixture *f = widget->userdata;
    struct ui_widget *bar = ui_tabbar_widget(&f->tb);
    int h = bar->measured_size.y;
    if (h > bounds.height - f->origin.y)
        h = bounds.height - f->origin.y;
    ui_widget_place(bar, (struct rect2i){
        .pos = vector2i_add(bounds.pos, f->origin),
        .size = { .x = bounds.width - f->origin.x, .y = h },
    });
}

static const struct ui_widget_ops ROOT_OPS = {
    .name = "tabbar-test-root",
    .measure = root_measure,
    .layout = root_layout,
};

static bool on_action(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    if (f->n_log < MAX_ACTIONS)
        f->log[f->n_log++] = *action;
    if (action->kind == UI_ACTION_TEAR_OFF)
        return f->take_tear_off;
    return true;
}

static void setup_with(struct fixture *f, const struct css_sheet *sheet,
                       struct vector2i viewport, struct vector2i origin)
{
    *f = (struct fixture){ .origin = origin };
    ui_widget_init(&f->root, &ROOT_OPS, f);
    ui_tabbar_init(&f->tb);
    assert(ui_widget_add_child(&f->root, ui_tabbar_widget(&f->tb)));
    ui_widget_set_action_handler(ui_tabbar_widget(&f->tb), on_action, f);
    ui_surface_set_sheet(&f->surface, sheet);
    ui_surface_set_scale(&f->surface, SCALE);
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, viewport);
    ui_surface_prepare(&f->surface);
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(&f->root);
    ui_scene_destroy(&f->scene);
}

static void set_tabs(struct fixture *f, const struct ui_tab_label *labels,
                     int n)
{
    ui_tabbar_set_tabs(&f->tb, labels, n);
    ui_surface_prepare(&f->surface);
}

/* Shows n tabs named "tab0", "tab1", ...; active is the active one or -1. */
static void show(struct fixture *f, int n, int active)
{
    assert(n <= MAX_TABS);
    for (int i = 0; i < n; i++) {
        snprintf(f->names[i], sizeof(f->names[i]), "tab%d", i);
        f->labels[i] = (struct ui_tab_label){
            .text = f->names[i], .active = i == active,
        };
    }
    set_tabs(f, f->labels, n);
}

/* A bar of n tabs in a surface width wide. */
static void setup(struct fixture *f, int n, int width)
{
    setup_with(f, NULL, (struct vector2i){ .x = width, .y = 300 },
               (struct vector2i){0});
    show(f, n, -1);
}

static bool tick(struct fixture *f, uint64_t now_ms)
{
    bool more = ui_surface_tick(&f->surface, now_ms);
    ui_surface_prepare(&f->surface);
    return more;
}

static bool pointer(struct fixture *f, enum ui_pointer_kind kind,
                    struct vector2i pos, int button, uint64_t time_ms)
{
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos, .button = button,
    }, time_ms);
    bool handled = ui_surface_dispatch(&f->surface, &e);
    ui_surface_prepare(&f->surface);
    return handled;
}

static void press(struct fixture *f, struct vector2i pos, int button,
                  uint64_t time_ms)
{
    pointer(f, UI_POINTER_PRESS, pos, button, time_ms);
}

static void release(struct fixture *f, struct vector2i pos, uint64_t time_ms)
{
    pointer(f, UI_POINTER_RELEASE, pos, 0, time_ms);
}

static void motion(struct fixture *f, struct vector2i pos, uint64_t time_ms)
{
    pointer(f, UI_POINTER_MOTION, pos, 0, time_ms);
}

static void click(struct fixture *f, struct vector2i pos, uint64_t time_ms)
{
    press(f, pos, 0, time_ms);
    release(f, pos, time_ms + 16);
}

static void clear_log(struct fixture *f)
{
    f->n_log = 0;
}

static int count(const struct fixture *f, enum ui_action_kind kind)
{
    int n = 0;
    for (int i = 0; i < f->n_log; i++)
        if (f->log[i].kind == kind)
            n++;
    return n;
}

static const struct ui_action *last(const struct fixture *f,
                                    enum ui_action_kind kind)
{
    for (int i = f->n_log - 1; i >= 0; i--)
        if (f->log[i].kind == kind)
            return &f->log[i];
    return NULL;
}

static bool logged_index(const struct fixture *f, int index)
{
    for (int i = 0; i < f->n_log; i++)
        if (f->log[i].index == index)
            return true;
    return false;
}

static struct ui_widget *bar(struct fixture *f)
{
    return ui_tabbar_widget(&f->tb);
}

static struct rect2i tab_rect(const struct fixture *f, int index)
{
    struct rect2i r;
    assert(ui_tabbar_tab_rect(&f->tb, index, &r));
    return r;
}

static struct rect2i close_rect(const struct fixture *f, int index)
{
    struct rect2i r;
    assert(ui_tabbar_close_rect(&f->tb, index, &r));
    return r;
}

static struct rect2i new_tab_rect(const struct fixture *f)
{
    struct rect2i r;
    assert(ui_tabbar_new_tab_rect(&f->tb, &r));
    return r;
}

static struct vector2i center(struct rect2i r)
{
    return (struct vector2i){ .x = r.x + r.width / 2, .y = r.y + r.height / 2 };
}

static struct vector2i at(struct rect2i r, int dx, int dy)
{
    return (struct vector2i){ .x = r.x + dx, .y = r.y + dy };
}

static const struct ui_scene *build(struct fixture *f)
{
    assert(ui_surface_build_scene(&f->surface, &f->scene));
    assert(ui_scene_is_valid(&f->scene));
    return &f->scene;
}

static int lint(struct fixture *f)
{
    struct ui_debug_view view = {
        .name = "tabbar", .root = &f->root, .surface = &f->surface,
        .scene = &f->scene,
    };
    struct ui_debug_lint findings[16];
    return ui_debug_lint_scene(&view, findings, 16);
}

static bool find_part(struct fixture *f, const char *name, int index,
                      struct rect2i *out)
{
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(bar(f), &parts);
    bool found = false;
    for (int i = 0; i < parts.n && !found; i++) {
        if (!strcmp(parts.items[i].name, name) &&
            parts.items[i].index == index) {
            *out = parts.items[i].rect;
            found = true;
        }
    }
    ui_debug_parts_free(&parts);
    return found;
}

static bool is_fill(const struct ui_scene_op *op)
{
    return op->kind == UI_SCENE_OP_RECT ||
           op->kind == UI_SCENE_OP_ROUNDED_RECT;
}

static uint32_t first_rect_color(const struct ui_scene *scene)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (is_fill(&scene->ops[i]))
            return scene->ops[i].color;
    return 0;
}

static bool scene_has_color(const struct ui_scene *scene, uint32_t color)
{
    for (int i = 0; i < scene->n_ops; i++)
        if ((is_fill(&scene->ops[i]) ||
             scene->ops[i].kind == UI_SCENE_OP_CIRCLE) &&
            scene->ops[i].color == color)
            return true;
    return false;
}

static const struct ui_scene_op *find_text(const struct ui_scene *scene,
                                           const char *s)
{
    size_t len = strlen(s);
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == len &&
            !memcmp(ui_scene_op_text(scene, op), s, len))
            return op;
    }
    return NULL;
}

static int count_ops(const struct ui_scene *scene, enum ui_scene_op_kind kind)
{
    int n = 0;
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].kind == kind)
            n++;
    return n;
}

static int count_separators(const struct ui_scene *scene, uint32_t color)
{
    int n = 0;
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].kind == UI_SCENE_OP_RECT &&
            scene->ops[i].color == color && scene->ops[i].rect.width == 1)
            n++;
    return n;
}

/* Tab index of a settled bar of n tabs in a surface width wide. */
static struct rect2i settled_rect(int n, int width, int index)
{
    struct fixture f;
    setup(&f, n, width);
    struct rect2i r = tab_rect(&f, index);
    teardown(&f);
    return r;
}

/* Opens a tab at slot the way a host does: state, labels, animation. */
static void open_tab(struct fixture *f, int slot, int n_after, uint64_t now_ms)
{
    assert(ui_tabbar_insert_tab(&f->tb, slot));
    show(f, n_after, -1);
    assert(ui_tabbar_begin_open(&f->tb, slot, now_ms));
    ui_surface_prepare(&f->surface);
}

static bool near(int a, int b)
{
    return a >= b - 1 && a <= b + 1;
}

static void css_theme_drives_layout_and_scene(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".tab-bar { height: 24px; padding: 2px 6px; gap: 4px;"
        " background: #101820; border-color: #304050; }"
        ".tab { height: 18px; background: #203040; color: #90a0b0;"
        " border-radius: 3px; padding: 0 8px; }"
        ".tab.active { background: #405060; color: #ffffff; }"
        ".tab-close { width: 10px; height: 10px; color: #708090; }"
        ".tab-close:hover { background: #ff0000; }"));

    struct fixture f;
    setup_with(&f, &sheet, (struct vector2i){ .x = 220, .y = 100 },
               (struct vector2i){0});
    show(&f, 1, 0);
    assert(ui_tabbar_height(&f.tb) == 0);
    const struct ui_tab_label labels[] = {
        {.text = "one", .active = false}, {.text = "two", .active = true},
    };
    set_tabs(&f, labels, 2);
    assert(ui_tabbar_height(&f.tb) == 24);
    assert(bar(&f)->bounds.height == 24);

    struct rect2i first = tab_rect(&f, 0), second = tab_rect(&f, 1);
    assert(first.x == 6 && first.y == 3 && first.width == 90 && first.height == 18);
    assert(second.x == 100 && second.y == 3 && second.width == 90 && second.height == 18);
    struct rect2i close = close_rect(&f, 0);
    assert(close.width == 10 && close.height == 10);
    struct rect2i nr = new_tab_rect(&f);
    assert(nr.x == rect2i_end(second).x + 4);

    const struct ui_scene *scene = build(&f);
    assert(scene->n_ops > 4);
    assert(first_rect_color(scene) == 0xFF101820u);
    assert(scene_has_color(scene, 0xFF405060u));
    assert(scene_has_color(scene, 0xFF203040u));
    assert(find_text(scene, "one") && find_text(scene, "two"));
    assert(lint(&f) == 0);

    struct rect2i part;
    assert(find_part(&f, "bar", -1, &part));
    assert(rect2i_eq(part, (struct rect2i){ .size = { .x = 220, .y = 24 } }));
    assert(find_part(&f, "tab", 0, &part) && rect2i_eq(part, first));
    assert(find_part(&f, "tab", 1, &part) && rect2i_eq(part, second));
    assert(find_part(&f, "tab-close", 0, &part) && rect2i_eq(part, close));
    assert(find_part(&f, "tab-new", -1, &part) && rect2i_eq(part, nr));

    click(&f, at(close, 1, 1), 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_CLOSE &&
           f.log[0].index == 0);
    clear_log(&f);
    click(&f, at(second, 1, 1), 1100);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 1);
    clear_log(&f);
    click(&f, (struct vector2i){ .x = nr.x + 2, .y = second.y + 1 }, 1200);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].id == UI_TABBAR_NEW_TAB && f.log[0].index == -1);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void tabbar_edge_cases(void)
{
    struct fixture f;
    setup(&f, 0, 200);
    struct rect2i r;
    assert(ui_tabbar_height(&f.tb) == 0);
    assert(!ui_tabbar_tab_rect(&f.tb, 0, &r));
    assert(!ui_tabbar_close_rect(&f.tb, 0, &r));
    assert(!ui_tabbar_new_tab_rect(&f.tb, &r));

    show(&f, 1, 0);
    assert(ui_tabbar_height(&f.tb) == 0);
    click(&f, (struct vector2i){ .x = 50, .y = 10 }, 1000);
    assert(f.n_log == 0);

    show(&f, 2, 0);
    assert(!ui_tabbar_tab_rect(&f.tb, -1, &r));
    assert(!ui_tabbar_tab_rect(&f.tb, 2, &r));
    assert(!ui_tabbar_close_rect(&f.tb, -1, &r));
    assert(!ui_tabbar_close_rect(&f.tb, 2, &r));
    static const struct vector2i misses[] = {
        { .x = -5, .y = 10 }, { .x = 500, .y = 10 },
        { .x = 50, .y = -5 }, { .x = 50, .y = 100 },
    };
    for (size_t i = 0; i < sizeof(misses) / sizeof(misses[0]); i++)
        click(&f, misses[i], 1100);
    assert(f.n_log == 0);
    teardown(&f);

    setup(&f, 16, 320);
    show(&f, 16, 3);
    build(&f);
    for (int i = 0; i < 16; i++) {
        r = tab_rect(&f, i);
        assert(r.width > 0 && r.height > 0);
        clear_log(&f);
        click(&f, center(r), 2000 + (uint64_t)i * 100);
        assert(f.n_log == 1 && f.log[0].index == i);
        assert(f.log[0].kind == UI_ACTION_ACTIVATE ||
               f.log[0].kind == UI_ACTION_CLOSE);
    }
    teardown(&f);
}

static void tab_click_and_close_activation(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    click(&f, center(tab_rect(&f, 0)), 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 0);
    clear_log(&f);
    click(&f, center(close_rect(&f, 1)), 1100);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_CLOSE &&
           f.log[0].index == 1);
    teardown(&f);
}

static void tabbar_keyboard_navigation_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    show(&f, 3, 0);

    ui_surface_focus(&f.surface, &f.tb.widget);
    clear_log(&f);

    struct ui_event key_right = ui_event_key((struct ui_key_event){
        .key = UI_KEY_RIGHT,
        .action = UI_KEY_PRESS,
    }, 100);
    assert(ui_surface_dispatch(&f.surface, &key_right));
    assert(f.n_log == 0);

    struct ui_event key_enter = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
        .action = UI_KEY_PRESS,
    }, 200);
    assert(ui_surface_dispatch(&f.surface, &key_enter));
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 1);
    teardown(&f);
}

/* Arrows move the keyboard marker, and Escape drops the focus. Both change
 * what the bar paints, so each key asks for the frame. */
static void tabbar_key_requests_frame_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    show(&f, 3, 0);
    ui_surface_focus(&f.surface, &f.tb.widget);
    ui_surface_take_frame_request(&f.surface);

    struct ui_event key_right = ui_event_key((struct ui_key_event){
        .key = UI_KEY_RIGHT, .action = UI_KEY_PRESS,
    }, 100);
    assert(ui_surface_dispatch(&f.surface, &key_right));
    assert(ui_surface_frame_pending(&f.surface));
    ui_surface_take_frame_request(&f.surface);

    struct ui_event key_escape = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ESCAPE, .action = UI_KEY_PRESS,
    }, 200);
    assert(ui_surface_dispatch(&f.surface, &key_escape));
    assert(ui_surface_focused(&f.surface) == NULL);
    assert(ui_surface_frame_pending(&f.surface));
    teardown(&f);
}

static void close_hover_crossfade_is_visible(void)
{
    struct fixture f;
    setup(&f, 2, 220);
    show(&f, 2, 0);
    const struct ui_scene *scene = build(&f);
    assert(find_text(scene, TAB_BADGE "1"));
    assert(!find_text(scene, CLOSE_MARK));

    motion(&f, center(close_rect(&f, 0)), 1000);
    assert(last(&f, UI_ACTION_HIGHLIGHT)->index == 0);
    assert(tick(&f, 1075));
    assert(!tick(&f, 1200));
    scene = build(&f);
    assert(find_text(scene, CLOSE_MARK));
    assert(!find_text(scene, TAB_BADGE "1"));
    assert(scene_has_color(scene, 0xFFF38BA8u));

    motion(&f, (struct vector2i){ .x = 100, .y = 200 }, 2000);
    assert(last(&f, UI_ACTION_HIGHLIGHT)->index == -1);
    assert(tick(&f, 2075));
    assert(!tick(&f, 2200));
    scene = build(&f);
    assert(find_text(scene, TAB_BADGE "1"));
    assert(!find_text(scene, CLOSE_MARK));
    teardown(&f);
}

static void hover_and_active_styling_in_scene(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    const struct ui_tab_label labels[] = {
        {.text = "a", .active = true}, {.text = "b", .active = false},
    };
    set_tabs(&f, labels, 2);
    motion(&f, center(tab_rect(&f, 1)), 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_HIGHLIGHT &&
           f.log[0].index == 1);

    const struct ui_scene *scene = build(&f);
    assert(scene_has_color(scene, 0xFF45475Au));
    assert(scene_has_color(scene, 0xFF313244u));
    const struct ui_scene_op *a = find_text(scene, "a");
    const struct ui_scene_op *b = find_text(scene, "b");
    assert(a && (a->style.flags & UI_TEXT_BOLD));
    assert(b && !(b->style.flags & UI_TEXT_BOLD));
    teardown(&f);
}

static void height_scales_with_dpi(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    int base = ui_tabbar_height(&f.tb);
    assert(base > 0 && bar(&f)->bounds.height == base);

    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32,
    });
    ui_surface_prepare(&f.surface);
    assert(ui_tabbar_height(&f.tb) > base);
    assert(bar(&f)->bounds.height == ui_tabbar_height(&f.tb));

    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".tab-bar { height: 10px; }"));
    ui_surface_set_sheet(&f.surface, &sheet);
    ui_surface_prepare(&f.surface);
    assert(ui_tabbar_height(&f.tb) == 20);
    assert(bar(&f)->bounds.height == 20);
    teardown(&f);
    css_sheet_clear(&sheet);
}

/* A new sheet or a new host background shows in the next scene, after the
 * bar has already been painted once. */
static void restyled_bar_repaints_test(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    show(&f, 2, 0);
    assert(first_rect_color(build(&f)) != 0xFF0A0B0Cu);

    ui_tabbar_set_bg_color(&f.tb, 0xFF0A0B0Cu);
    ui_surface_prepare(&f.surface);
    assert(first_rect_color(build(&f)) == 0xFF0A0B0Cu);

    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".tab-bar { background: #204060; }"
        ".tab { background: #304050; }"));
    ui_surface_set_sheet(&f.surface, &sheet);
    ui_surface_prepare(&f.surface);
    const struct ui_scene *scene = build(&f);
    assert(first_rect_color(scene) == 0xFF204060u);
    assert(scene_has_color(scene, 0xFF304050u));
    teardown(&f);
    css_sheet_clear(&sheet);
}

/* A host background change asks the host for a frame. A bar that is
 * already settled asks for none. */
static void bg_color_requests_frame_test(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    show(&f, 2, 0);
    build(&f);
    ui_surface_take_frame_request(&f.surface);
    assert(!ui_surface_frame_pending(&f.surface));

    ui_tabbar_set_bg_color(&f.tb, 0xFF0A0B0Cu);
    assert(ui_surface_frame_pending(&f.surface));
    ui_surface_take_frame_request(&f.surface);
    ui_surface_prepare(&f.surface);
    assert(first_rect_color(build(&f)) == 0xFF0A0B0Cu);
    teardown(&f);
}

/* A sheet swap that changes the bar height relays the bar out. The bar
 * and its tabs take the new height. */
static void sheet_swap_relayouts_bar_test(void)
{
    struct css_sheet tall = {0}, high = {0};
    /* A tab taller than the bar takes the bar's inner height. */
    assert(css_parse(&tall, ".tab-bar { height: 24px; } .tab { height: 100px; }"));
    assert(css_parse(&high, ".tab-bar { height: 40px; } .tab { height: 100px; }"));
    struct fixture f;
    setup_with(&f, &tall, (struct vector2i){ .x = 400, .y = 300 },
               (struct vector2i){0});
    show(&f, 2, 0);
    assert(bar(&f)->bounds.height == 24);
    assert(tab_rect(&f, 0).height == 16);

    ui_surface_set_sheet(&f.surface, &high);
    ui_surface_prepare(&f.surface);
    assert(ui_tabbar_height(&f.tb) == 40);
    assert(bar(&f)->bounds.height == 40);
    assert(tab_rect(&f, 0).height == 32);
    struct rect2i part;
    assert(find_part(&f, "bar", -1, &part) && part.height == 40);
    assert(lint(&f) == 0);
    teardown(&f);
    css_sheet_clear(&high);
    css_sheet_clear(&tall);
}

struct restyle_on_highlight {
    struct fixture *f;
    const struct css_sheet *next;
    struct rect2i tab_after;
};

/* The host restyles the bar from inside the highlight callback. */
static bool restyle_on_highlight(struct ui_widget *source,
                                 const struct ui_action *action,
                                 void *userdata)
{
    (void)source;
    struct restyle_on_highlight *r = userdata;
    if (action->kind == UI_ACTION_HIGHLIGHT && r->next) {
        ui_surface_set_sheet(&r->f->surface, r->next);
        r->next = NULL;
        bool ok = ui_tabbar_tab_rect(&r->f->tb, 1, &r->tab_after);
        assert(ok);
    }
    return true;
}

/* A pointer move that restyles the bar shorter, under the pointer. The
 * bar must not claim the move: the pointer is below the new bar. */
static void host_restyle_during_motion_test(void)
{
    struct css_sheet tall = {0}, low = {0};
    assert(css_parse(&tall, ".tab-bar { height: 24px; }"));
    assert(css_parse(&low, ".tab-bar { height: 10px; }"));
    struct fixture f;
    setup_with(&f, &tall, (struct vector2i){ .x = 400, .y = 300 },
               (struct vector2i){0});
    show(&f, 2, 0);
    struct restyle_on_highlight r = { .f = &f, .next = &low };
    ui_widget_set_action_handler(bar(&f), restyle_on_highlight, &r);

    bool handled = pointer(&f, UI_POINTER_MOTION, center(tab_rect(&f, 1)),
                           0, 1000);
    assert(r.next == NULL);
    assert(ui_tabbar_height(&f.tb) == 10);
    assert(r.tab_after.height > 0);
    assert(!handled);
    teardown(&f);
    css_sheet_clear(&low);
    css_sheet_clear(&tall);
}

static void last_tab_claims_trailing_space(void)
{
    struct fixture f;
    setup(&f, 2, 1200);
    struct rect2i last_tab = tab_rect(&f, 1);
    struct rect2i close = close_rect(&f, 1);
    assert(last_tab.width > 0);

    int y = last_tab.y + last_tab.height / 2;
    uint64_t t = 1000;
    for (int x = last_tab.x; x < rect2i_end(last_tab).x; x += 17) {
        clear_log(&f);
        click(&f, (struct vector2i){ .x = x, .y = y }, t += 100);
        assert(f.n_log == 1 && f.log[0].index == 1);
    }
    clear_log(&f);
    click(&f, center(close), t += 100);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_CLOSE &&
           f.log[0].index == 1);

    clear_log(&f);
    click(&f, (struct vector2i){ .x = rect2i_end(last_tab).x, .y = y }, t + 100);
    assert(!logged_index(&f, 1));
    teardown(&f);
}

static void text_runs_stay_on_codepoint_boundaries(void)
{
    const char *title = "\xe9\x85\x8d\xe7\xbd\xae \xc3\xa9" "diteur \xe2\x80\x94 log";
    const struct ui_tab_label labels[] = {
        {.text = title, .active = true}, {.text = "caf\xc3\xa9", .active = false},
    };
    struct fixture f;
    setup(&f, 0, 220);
    set_tabs(&f, labels, 2);
    motion(&f, center(close_rect(&f, 1)), 1000);
    assert(tick(&f, 1075));

    const struct ui_scene *scene = build(&f);
    int runs = 0;
    const struct ui_scene_op *label = NULL;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind != UI_SCENE_OP_TEXT)
            continue;
        if (!label)
            label = op;
        const char *text = ui_scene_op_text(scene, op);
        assert(text);
        for (uint32_t off = 0; off < op->text_len;) {
            uint32_t cp;
            int n = wsap_vtcore_utf8_decode_one(text + off, op->text_len - off, &cp);
            assert(n > 0);
            off += (uint32_t)n;
        }
        runs++;
    }
    assert(runs >= 3);
    assert(label && label->text_len > 0 && label->text_len < strlen(title));
    assert(!memcmp(ui_scene_op_text(scene, label), title, label->text_len));
    teardown(&f);
}

static void tabbar_drag_sort_detach_attach_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    show(&f, 3, 1);
    struct rect2i r0 = tab_rect(&f, 0), r1 = tab_rect(&f, 1);

    press(&f, at(r0, 10, 5), 0, 50);
    assert(count(&f, UI_ACTION_ACTIVATE) == 1);
    motion(&f, at(r0, 12, 6), 60);
    assert(!ui_tabbar_is_dragging(&f.tb));
    assert(count(&f, UI_ACTION_DRAG) == 0);

    motion(&f, at(r1, 10, 5), 70);
    assert(ui_tabbar_is_dragging(&f.tb));
    const struct ui_action *drag = last(&f, UI_ACTION_DRAG);
    assert(drag && drag->id == UI_TAB_DRAG_REORDER && drag->index == 0);
    assert(count(&f, UI_ACTION_TEAR_OFF) == 0);

    assert(tick(&f, 100));
    const struct ui_scene *scene = build(&f);
    bool displaced = false;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (is_fill(op) && op->rect.width == r1.width &&
            op->rect.height == r1.height &&
            op->rect.x < r1.x && op->rect.x >= r0.x)
            displaced = true;
    }
    assert(displaced);

    release(&f, at(r1, 10, 5), 120);
    const struct ui_action *move = last(&f, UI_ACTION_MOVE);
    assert(move && move->index == 0 && move->to == 1);
    assert(!ui_tabbar_is_dragging(&f.tb));
    assert(ui_surface_captured(&f.surface) == NULL);

    clear_log(&f);
    press(&f, at(r1, 10, 5), 0, 200);
    motion(&f, (struct vector2i){ .x = r1.x + 10, .y = 100 }, 210);
    assert(ui_tabbar_is_dragging(&f.tb));
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_DETACH);
    assert(last(&f, UI_ACTION_TEAR_OFF)->index == 1);
    release(&f, (struct vector2i){ .x = r1.x + 10, .y = 100 }, 220);
    move = last(&f, UI_ACTION_MOVE);
    assert(move && move->index == 1 && move->to == -1);

    ui_tabbar_set_external_drag(&f.tb, true,
        (struct vector2i){ .x = r1.x + 10, .y = 10 });
    assert(ui_tabbar_external_slot(&f.tb) >= 0);
    ui_tabbar_set_external_drag(&f.tb, true,
        (struct vector2i){ .x = r1.x + 10, .y = 100 });
    assert(ui_tabbar_external_slot(&f.tb) == -1);
    ui_tabbar_set_external_drag(&f.tb, true,
        (struct vector2i){ .x = -1, .y = 10 });
    assert(ui_tabbar_external_slot(&f.tb) == -1);
    ui_tabbar_set_external_drag(&f.tb, false, (struct vector2i){0});
    assert(ui_tabbar_external_slot(&f.tb) == -1);
    teardown(&f);
}

/* A tab dragged straight across, out of the side of the window, detaches:
 * that is the way to another window beside this one. */
static void sideways_exit_detaches_test(void)
{
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 600, .y = 400 },
               (struct vector2i){0});
    show(&f, 3, 0);
    struct vector2i grab = at(tab_rect(&f, 2), 10, 5);
    press(&f, grab, 0, 1000);

    motion(&f, (struct vector2i){ .x = 595, .y = grab.y }, 1010);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_REORDER);
    motion(&f, (struct vector2i){ .x = 640, .y = grab.y }, 1020);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_DETACH);
    motion(&f, (struct vector2i){ .x = 595, .y = grab.y }, 1030);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_REORDER);
    motion(&f, (struct vector2i){ .x = -40, .y = grab.y }, 1040);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_DETACH);
    ui_surface_cancel_capture(&f.surface);
    teardown(&f);
}

static void external_drag_reveals_single_tab_target_test(void)
{
    struct fixture f;
    setup(&f, 1, 400);
    assert(ui_tabbar_steady_height(&f.tb) == 0);
    assert(ui_tabbar_height(&f.tb) == 0);

    ui_tabbar_set_external_drag(&f.tb, true,
        (struct vector2i){ .x = 200, .y = 10 });
    ui_surface_prepare(&f.surface);
    assert(ui_tabbar_external_slot(&f.tb) >= 0);
    int full = ui_tabbar_steady_height(&f.tb);
    assert(full > 0);
    tick(&f, 1000);
    tick(&f, 1300);
    assert(ui_tabbar_height(&f.tb) == full);
    assert(bar(&f)->bounds.height == full);
    struct rect2i drop;
    assert(find_part(&f, "drop-preview", -1, &drop));
    assert(drop.width > 0 && drop.height > 0);

    ui_tabbar_set_external_drag(&f.tb, false, (struct vector2i){0});
    tick(&f, 1400);
    assert(ui_tabbar_external_slot(&f.tb) == -1);
    assert(ui_tabbar_steady_height(&f.tb) == 0);
    assert(ui_tabbar_height(&f.tb) == 0);
    teardown(&f);
}

static void incoming_tab_has_translucent_drop_preview(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    ui_tabbar_set_external_drag(&f.tb, true,
        (struct vector2i){ .x = 380, .y = 10 });
    assert(ui_tabbar_external_slot(&f.tb) == 2);

    const struct ui_scene *scene = build(&f);
    struct rect2i drop;
    assert(find_part(&f, "drop-preview", -1, &drop));
    bool preview = false;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind != UI_SCENE_OP_ROUNDED_RECT ||
            (op->color & 0xFFFFFFu) != 0x89B4FAu)
            continue;
        assert((op->color >> 24) > 0 && (op->color >> 24) < 255);
        assert(op->rect.width > 0 && op->rect.x > 200);
        assert(rect2i_eq(op->rect, drop));
        preview = true;
    }
    assert(preview);

    ui_tabbar_set_external_drag(&f.tb, false, (struct vector2i){0});
    scene = build(&f);
    assert(!find_part(&f, "drop-preview", -1, &drop));
    for (int i = 0; i < scene->n_ops; i++)
        assert((scene->ops[i].color & 0xFFFFFFu) != 0x89B4FAu);
    teardown(&f);
}

/* Rects, debug parts, and input all use surface space, wherever the bar
 * sits. */
static void bar_away_from_the_origin_uses_surface_space(void)
{
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 430, .y = 300 },
               (struct vector2i){ .x = 30, .y = 40 });
    show(&f, 3, 1);
    struct fixture g;
    setup(&g, 3, 400);
    for (int i = 0; i < 3; i++) {
        struct rect2i r = tab_rect(&f, i), part;
        assert(rect2i_eq(r, rect2i_translate(tab_rect(&g, i),
                                             f.origin)));
        assert(find_part(&f, "tab", i, &part) && rect2i_eq(part, r));
        assert(find_part(&f, "tab-close", i, &part) &&
               rect2i_eq(part, close_rect(&f, i)));
    }
    teardown(&g);

    click(&f, center(tab_rect(&f, 2)), 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 2);
    clear_log(&f);
    click(&f, center(close_rect(&f, 0)), 1100);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_CLOSE &&
           f.log[0].index == 0);
    clear_log(&f);
    click(&f, center(new_tab_rect(&f)), 1200);
    assert(f.n_log == 1 && f.log[0].id == UI_TABBAR_NEW_TAB);

    const struct ui_scene *scene = build(&f);
    struct rect2i bar_rect = {
        .pos = f.origin, .size = { .x = 400, .y = ui_tabbar_height(&f.tb) },
    };
    struct rect2i part;
    assert(find_part(&f, "bar", -1, &part) && rect2i_eq(part, bar_rect));
    const struct ui_scene_op *bg = NULL;
    for (int i = 0; i < scene->n_ops && !bg; i++)
        if (is_fill(&scene->ops[i]))
            bg = &scene->ops[i];
    assert(bg && rect2i_eq(bg->rect, bar_rect));
    teardown(&f);
}

static void tabbar_build_drag_preview_test(void)
{
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 430, .y = 240 },
               (struct vector2i){ .x = 30, .y = 40 });
    show(&f, 3, 1);
    struct rect2i tab = tab_rect(&f, 1);
    press(&f, at(tab, 10, 5), 0, 1000);

    struct ui_scene preview = {0};
    struct vector2i size, hotspot;
    assert(ui_tabbar_build_drag_scene(&f.tb, 1, NULL, &preview, &size,
                                      &hotspot));
    assert(ui_scene_is_valid(&preview));
    assert(vector2i_eq(size, tab.size));
    assert(vector2i_eq(hotspot, (struct vector2i){ .x = 10, .y = 5 }));
    assert(find_text(&preview, "tab1"));
    ui_scene_destroy(&preview);

    uint32_t pixels[20 * 20] = {0};
    struct ui_image img = { .pixels = pixels, .size = { .x = 20, .y = 20 } };
    struct ui_scene with_image = {0};
    struct vector2i size_img, hotspot_img;
    assert(ui_tabbar_build_drag_scene(&f.tb, 1, &img, &with_image, &size_img,
                                      &hotspot_img));
    assert(ui_scene_is_valid(&with_image));
    assert(size_img.y > size.y);
    assert(count_ops(&with_image, UI_SCENE_OP_IMAGE) == 1);
    ui_scene_destroy(&with_image);
    release(&f, at(tab, 10, 5), 1016);
    teardown(&f);
}

/* The cross-window drag image draws the hover card, so it takes the card's
 * colors and metrics from the same rule. A theme that restyles
 * ".tab-preview" used to get the hover card in the new style and the drag
 * image in the default one. */
static void drag_image_card_follows_the_sheet_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".tab-preview { background: #7f849c; border-radius: 3px; "
        "padding: 2px; gap: 3px; box-shadow: 0 1px 2px #000000ff; }"));
    struct fixture f;
    setup_with(&f, &sheet, (struct vector2i){ .x = 800, .y = 300 },
               (struct vector2i){0});
    show(&f, 2, 0);
    struct rect2i tab;
    assert(ui_tabbar_tab_rect(&f.tb, 1, &tab));

    uint32_t pixels[20 * 20] = {0};
    struct ui_image img = { .pixels = pixels, .size = { .x = 20, .y = 20 } };
    struct ui_scene scene = {0};
    struct vector2i size, hotspot;
    assert(ui_tabbar_build_drag_scene(&f.tb, 1, &img, &scene, &size, &hotspot));

    /* The card under the tab takes the sheet background, radius and
     * padding: a 20px picture plus 2px of padding each way. */
    bool card = false, shadow = false;
    for (int i = 0; i < scene.n_ops; i++) {
        const struct ui_scene_op *op = &scene.ops[i];
        if (op->kind == UI_SCENE_OP_ROUNDED_RECT && op->radius == 3 &&
            (op->color & 0xFFFFFFu) == 0x7F849Cu) {
            assert(op->rect.pos.y == tab.size.y + 3);   /* sheet gap */
            assert(op->rect.size.x == 24 && op->rect.size.y == 24);
            card = true;
        }
        if (op->kind == UI_SCENE_OP_SHADOW && op->color == 0xFF000000u &&
            op->blur == 2)                             /* sheet blur */
            shadow = true;
    }
    assert(card && shadow);
    ui_scene_destroy(&scene);
    teardown(&f);
    css_sheet_clear(&sheet);
}

/* The drag between windows starts once the tab detaches, and pictures
 * the tab from the bar: a two-tab bar must still lay the tab out then. */
static void detaching_tab_still_has_drag_image_test(void)
{
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 800, .y = 600 },
               (struct vector2i){0});
    show(&f, 2, 0);
    struct rect2i r1 = tab_rect(&f, 1);
    press(&f, at(r1, 5, 5), 0, 1000);
    motion(&f, (struct vector2i){ .x = 500, .y = 400 }, 1010);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_DETACH);

    struct ui_scene scene = {0};
    struct vector2i image, hotspot;
    assert(ui_tabbar_build_drag_scene(&f.tb, 1, NULL, &scene, &image,
                                      &hotspot));
    assert(vector2i_eq(image, r1.size));
    ui_scene_destroy(&scene);
    teardown(&f);
}

static void detach_drag_chip_bounds_test(void)
{
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 1000, .y = 700 },
               (struct vector2i){0});
    const struct ui_tab_label labels[] = {
        {.text = "/mnt/HDD/GitHub/clernen_pi - fish", .active = true},
        {.text = "tab1", .active = false},
        {.text = "tab2", .active = false},
    };
    set_tabs(&f, labels, 3);
    int bar_h = ui_tabbar_height(&f.tb);
    assert(bar_h > 0);
    struct rect2i r1 = tab_rect(&f, 1);

    press(&f, at(r1, 10, 5), 0, 50);
    motion(&f, (struct vector2i){ .x = 500, .y = 400 }, 60);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_DETACH);
    tick(&f, 100);

    const struct ui_scene *scene = build(&f);
    int below = 0;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_PUSH_CLIP ||
            op->kind == UI_SCENE_OP_POP_CLIP || op->rect.y < bar_h)
            continue;
        assert(op->rect.height <= r1.height + 8);
        assert(op->rect.width <= r1.width + 8);
        below++;
    }
    assert(below > 0);
    teardown(&f);
}

static void title_and_close_do_not_overlap(void)
{
    const struct ui_tab_label labels[] = {
        {.text = "a-very-long-tab-title-that-gets-truncated", .active = true},
        {.text = "second", .active = false},
    };
    struct fixture f;
    setup(&f, 0, 220);
    set_tabs(&f, labels, 2);

    const struct ui_scene *scene = build(&f);
    struct rect2i boxes[2] = {0};
    int found = 0;
    for (int i = 0; i < scene->n_ops && found < 2; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len >= 4 &&
            !memcmp(ui_scene_op_text(scene, op), labels[found].text, 4))
            boxes[found++] = ui_scene_text_box(scene, op);
    }
    assert(found == 2);
    for (int i = 0; i < 2; i++)
        assert(rect2i_is_empty(rect2i_intersect(boxes[i], close_rect(&f, i))));

    click(&f, at(boxes[0], 2, boxes[0].height / 2), 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 0);
    teardown(&f);
}

static void new_tab_hover_animates(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    struct rect2i nr = new_tab_rect(&f);
    assert(nr.width > 0 && nr.height > 0);
    int idle_circles = count_ops(build(&f), UI_SCENE_OP_CIRCLE);

    motion(&f, center(nr), 1000);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_POINTER);
    assert(tick(&f, 1050));
    const struct ui_scene *scene = build(&f);
    bool partial = false;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_CIRCLE && op->alpha > 0 && op->alpha < 255)
            partial = true;
    }
    assert(partial);

    assert(!tick(&f, 1200));
    assert(count_ops(build(&f), UI_SCENE_OP_CIRCLE) > idle_circles);

    motion(&f, (struct vector2i){ .x = 200, .y = 200 }, 2000);
    assert(tick(&f, 2050));
    assert(!tick(&f, 2200));
    assert(count_ops(build(&f), UI_SCENE_OP_CIRCLE) == idle_circles);
    teardown(&f);
}

static void zeroed_tabbar_is_idle(void)
{
    struct ui_tabbar *tb = calloc(1, sizeof(*tb));
    assert(tb);
    struct rect2i r;
    assert(ui_tabbar_closing_tab(tb) == -1);
    assert(!ui_tabbar_closing_done(tb));
    assert(!ui_tabbar_is_closing(tb, 0));
    assert(!ui_tabbar_is_dragging(tb));
    assert(ui_tabbar_external_slot(tb) == -1);
    assert(ui_tabbar_height(tb) == 0 && ui_tabbar_steady_height(tb) == 0);
    assert(!ui_tabbar_tab_rect(tb, 0, &r));
    assert(!ui_tabbar_new_tab_rect(tb, &r));
    ui_widget_destroy(&tb->widget);
    free(tb);
}

static void closing_tab_tracks_through_remove(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    assert(ui_tabbar_begin_close(&f.tb, 2, 1000));
    assert(ui_tabbar_closing_tab(&f.tb) == 2);
    assert(ui_tabbar_is_closing(&f.tb, 2) && !ui_tabbar_is_closing(&f.tb, 0));

    assert(ui_tabbar_remove_tab(&f.tb, 0));
    show(&f, 2, -1);
    assert(ui_tabbar_closing_tab(&f.tb) == 1);
    assert(!ui_tabbar_closing_done(&f.tb));
    tick(&f, 1200);
    assert(ui_tabbar_closing_done(&f.tb));
    teardown(&f);
}

static void open_tab_grows_in(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    struct rect2i full = tab_rect(&f, 2);
    assert(ui_tabbar_begin_open(&f.tb, 2, 1000));

    assert(tick(&f, 1040));
    struct rect2i mid = tab_rect(&f, 2);
    assert(mid.width < full.width);
    assert(mid.x > full.x);

    assert(!tick(&f, 1200));
    assert(rect2i_eq(tab_rect(&f, 2), full));
    teardown(&f);
}

static void open_tab_reflows_siblings(void)
{
    struct rect2i settle0 = settled_rect(3, 400, 0);
    struct fixture f;
    setup(&f, 2, 400);
    tick(&f, 1000);
    struct rect2i before0 = tab_rect(&f, 0);
    assert(before0.width > settle0.width);

    open_tab(&f, 2, 3, 1000);
    assert(tab_rect(&f, 0).width == before0.width);

    assert(tick(&f, 1040));
    struct rect2i r0 = tab_rect(&f, 0);
    assert(r0.width < before0.width && r0.width > settle0.width);

    assert(!tick(&f, 1250));
    r0 = tab_rect(&f, 0);
    assert(r0.x == settle0.x && r0.width == settle0.width);
    teardown(&f);
}

static void new_tab_button_works_at(struct fixture *f, uint64_t t)
{
    struct rect2i nr = new_tab_rect(f);
    assert(nr.width > 0 && nr.height > 0);
    assert(rect2i_end(nr).x <= ui_surface_viewport(&f->surface).x);
    clear_log(f);
    click(f, center(nr), t);
    assert(f->n_log == 1 && f->log[0].kind == UI_ACTION_ACTIVATE &&
           f->log[0].id == UI_TABBAR_NEW_TAB && f->log[0].index == -1);
}

static void open_tab_keeps_new_chip_hittable(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    open_tab(&f, 2, 3, 1000);
    for (int step = 0; step <= 6; step++) {
        uint64_t t = 1000 + (uint64_t)step * 40;
        tick(&f, t);
        new_tab_button_works_at(&f, t);
    }
    teardown(&f);
}

static void close_tab_keeps_new_chip_hittable(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    assert(ui_tabbar_begin_close(&f.tb, 0, 1000));
    for (int step = 0; step <= 6; step++) {
        uint64_t t = 1000 + (uint64_t)step * 40;
        tick(&f, t);
        new_tab_button_works_at(&f, t);
    }
    teardown(&f);
}

static void close_tab_reflows_siblings(void)
{
    struct rect2i s2 = settled_rect(2, 400, 0);
    struct fixture f;
    setup(&f, 3, 400);
    tick(&f, 1000);
    struct rect2i s3 = tab_rect(&f, 1);
    assert(s2.width > s3.width);

    assert(ui_tabbar_begin_close(&f.tb, 0, 1000));
    assert(tick(&f, 1100));
    struct rect2i mid1 = tab_rect(&f, 1);
    assert(mid1.width > s3.width && mid1.width < s2.width);

    tick(&f, 1400);
    assert(ui_tabbar_closing_done(&f.tb));
    struct rect2i pre = tab_rect(&f, 1);
    assert(pre.width == s2.width);

    assert(ui_tabbar_remove_tab(&f.tb, 0));
    show(&f, 2, -1);
    struct rect2i post = tab_rect(&f, 0);
    assert(post.x == pre.x && post.width == pre.width);
    teardown(&f);
}

static void open_close_widths_are_continuous(void)
{
    const int width = 1000;
    struct rect2i landing = settled_rect(3, width, 0);
    struct fixture f;
    setup(&f, 2, width);
    tick(&f, 1000);
    struct rect2i before = tab_rect(&f, 0);
    assert(landing.width < before.width);

    open_tab(&f, 2, 3, 2000);
    assert(near(tab_rect(&f, 0).width, before.width));
    assert(tick(&f, 2016));
    assert(near(tab_rect(&f, 0).width, before.width));
    for (int step = 1; step <= 5; step++) {
        tick(&f, 2000 + (uint64_t)step * 40);
        struct rect2i r = tab_rect(&f, 0);
        assert(r.width >= landing.width - 1 && r.width <= before.width + 1);
    }
    assert(!tick(&f, 2300));
    for (int i = 0; i < 3; i++) {
        struct rect2i landed = tab_rect(&f, i);
        struct rect2i settled = settled_rect(3, width, i);
        assert(near(landed.width, settled.width) && near(landed.x, settled.x));
    }
    teardown(&f);

    struct rect2i s2 = settled_rect(2, width, 1);
    setup(&f, 3, width);
    int prev_w = tab_rect(&f, 2).width;
    assert(s2.width > prev_w);
    assert(ui_tabbar_begin_close(&f.tb, 0, 2000));
    for (int step = 1; step <= 6; step++) {
        tick(&f, 2000 + (uint64_t)step * 40);
        struct rect2i r = tab_rect(&f, 2);
        assert(r.width >= prev_w - 1 && r.width <= s2.width + 1);
        prev_w = r.width;
    }
    tick(&f, 2400);
    assert(ui_tabbar_closing_done(&f.tb));
    assert(near(tab_rect(&f, 2).width, s2.width));
    teardown(&f);
}

static void vertical_padding_is_clamped(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".tab-bar { height: 24px; padding: 30px 4px; }"));
    struct fixture f;
    setup_with(&f, &sheet, (struct vector2i){ .x = 220, .y = 100 },
               (struct vector2i){0});
    show(&f, 2, 0);
    assert(ui_tabbar_height(&f.tb) == 24);
    for (int i = 0; i < 2; i++) {
        struct rect2i r = tab_rect(&f, i);
        assert(r.y >= 0 && r.y + r.height <= 24);
    }
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void chrome_glyphs_are_vertically_centered(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    show(&f, 2, 0);
    struct rect2i close = close_rect(&f, 0);
    int close_cy = close.y + close.height / 2;

    const struct ui_scene *scene = build(&f);
    const struct ui_scene_op *badge = find_text(scene, TAB_BADGE "1");
    assert(badge);
    assert(badge->rect.y + badge->rect.height / 2 == close_cy);
    assert(lint(&f) == 0);

    motion(&f, center(close), 1000);
    assert(!tick(&f, 1200));
    scene = build(&f);
    const struct ui_scene_op *mark = find_text(scene, CLOSE_MARK);
    assert(mark);
    assert(mark->rect.y + mark->rect.height / 2 == close_cy);
    assert(lint(&f) == 0);
    teardown(&f);
}

static void separators_hide_next_to_hot_tabs(void)
{
    const uint32_t sep = 0xFF123456u;
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".tab-separator { color: #123456; }"));
    struct fixture f;
    setup_with(&f, &sheet, (struct vector2i){ .x = 600, .y = 100 },
               (struct vector2i){0});
    show(&f, 3, -1);
    assert(count_separators(build(&f), sep) == 2);

    motion(&f, center(tab_rect(&f, 1)), 1000);
    assert(count_separators(build(&f), sep) == 0);
    motion(&f, (struct vector2i){ .x = 300, .y = 80 }, 1100);
    assert(count_separators(build(&f), sep) == 2);

    show(&f, 3, 1);
    assert(count_separators(build(&f), sep) == 0);
    show(&f, 3, 0);
    assert(count_separators(build(&f), sep) == 1);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void neighbor_slide_eases_on_drag(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    tick(&f, 1000);
    struct rect2i r0 = tab_rect(&f, 0), r1 = tab_rect(&f, 1);
    struct rect2i r2 = tab_rect(&f, 2);

    press(&f, at(r0, 10, 5), 0, 1000);
    motion(&f, at(r2, 10, 5), 1001);
    assert(last(&f, UI_ACTION_DRAG)->id == UI_TAB_DRAG_REORDER);

    assert(tick(&f, 1050));
    struct rect2i mid = tab_rect(&f, 1);
    assert(mid.x < r1.x && mid.x > r0.x);

    motion(&f, at(r0, 10, 5), 1100);
    tick(&f, 1200);
    tick(&f, 1400);
    assert(tab_rect(&f, 1).x == r1.x);

    motion(&f, at(r2, 10, 5), 1450);
    for (int step = 0; step < 20; step++)
        tick(&f, 1500 + (uint64_t)step * 30);
    assert(near(tab_rect(&f, 1).x, r0.x));

    release(&f, at(r2, 10, 5), 2200);
    const struct ui_action *move = last(&f, UI_ACTION_MOVE);
    assert(move && move->index == 0 && move->to == 2);
    teardown(&f);
}

static void fifty_tabs_frame_time_sub_half_ms(void)
{
    enum { N_INITIAL_TABS = 50, N_TOTAL_TABS = N_INITIAL_TABS + 1 };
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 1920, .y = 600 },
               (struct vector2i){0});
    show(&f, N_INITIAL_TABS, -1);
    tick(&f, 500);

    open_tab(&f, N_TOTAL_TABS - 1, N_TOTAL_TABS, 1000);
    f.labels[N_TOTAL_TABS - 1].active = true;
    set_tabs(&f, f.labels, N_TOTAL_TABS);
    for (int it = 0; it < 10; it++) {
        tick(&f, 1000 + (uint64_t)it * 16);
        build(&f);
    }
    assert(ui_tabbar_remove_tab(&f.tb, N_TOTAL_TABS - 1));
    set_tabs(&f, f.labels, N_INITIAL_TABS);
    tick(&f, 1500);

    enum { ITERATIONS = UI_TABBAR_RESIZE_ANIM_MS };
    const uint64_t start_ms = 2000;
    assert(ui_tabbar_insert_tab(&f.tb, N_TOTAL_TABS - 1));
    set_tabs(&f, f.labels, N_TOTAL_TABS);
    assert(ui_tabbar_begin_open(&f.tb, N_TOTAL_TABS - 1, start_ms));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int it = 0; it < ITERATIONS; it++) {
        assert(ui_surface_tick(&f.surface, start_ms + (uint64_t)it));
        assert(ui_surface_build_scene(&f.surface, &f.scene));
        assert(f.scene.n_ops > 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    assert(!tick(&f, start_ms + UI_TABBAR_RESIZE_ANIM_MS));

    double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9 +
                        (double)(end.tv_nsec - start.tv_nsec);
    double per_frame_ms = elapsed_ns / ITERATIONS / 1e6;
    fprintf(stderr, "50 tabs open animation frame: %.4f ms/frame "
            "(%d iterations)\n", per_frame_ms, ITERATIONS);
/* Shadow memory slows every load and store, so each sanitizer gets the
 * looser bound. A build with no sanitizer keeps the tight one. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_MEMORY__) || \
    defined(__SANITIZE_THREAD__)
    assert(per_frame_ms < 5.0);
#else
    assert(per_frame_ms < 0.5);
#endif
    teardown(&f);
}

static void bar_reveal_animates_on_boundary(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    int full = ui_tabbar_height(&f.tb);
    assert(full > 0);
    teardown(&f);

    setup(&f, 1, 400);
    assert(ui_tabbar_height(&f.tb) == 0);
    assert(ui_tabbar_steady_height(&f.tb) == 0);

    open_tab(&f, 1, 2, 1000);
    int h0 = ui_tabbar_height(&f.tb);
    assert(h0 < full);
    assert(ui_tabbar_steady_height(&f.tb) == full);

    assert(tick(&f, 1100));
    int hmid = ui_tabbar_height(&f.tb);
    assert(hmid > h0 && hmid < full);
    assert(bar(&f)->bounds.height == hmid);

    assert(!tick(&f, 1300));
    assert(ui_surface_next_deadline(&f.surface) == 0);
    assert(ui_tabbar_height(&f.tb) == full);
    assert(bar(&f)->bounds.height == full);

    assert(ui_tabbar_begin_close(&f.tb, 1, 2000));
    assert(tick(&f, 2100));
    int hclose = ui_tabbar_height(&f.tb);
    assert(hclose > 0 && hclose < full);
    assert(ui_tabbar_steady_height(&f.tb) == full);

    tick(&f, 2300);
    assert(ui_tabbar_closing_done(&f.tb));
    assert(ui_tabbar_remove_tab(&f.tb, 1));
    show(&f, 1, 0);
    assert(ui_tabbar_height(&f.tb) == 0);
    assert(bar(&f)->bounds.height == 0);

    show(&f, 3, 0);
    assert(ui_tabbar_height(&f.tb) == full);
    teardown(&f);
}

static void opening_chip_is_opaque_and_clipped(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    show(&f, 3, 0);
    assert(ui_tabbar_begin_open(&f.tb, 2, 1000));
    tick(&f, 1100);
    struct rect2i chip = tab_rect(&f, 2);
    assert(chip.width > 0);

    const struct ui_scene *scene = build(&f);
    bool saw_clip = false, saw_opaque_chip = false;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_PUSH_CLIP)
            saw_clip = true;
        if (is_fill(op) && op->rect.x == chip.x &&
            op->rect.height == chip.height && (op->color >> 24) == 0xFF)
            saw_opaque_chip = true;
    }
    assert(saw_clip && saw_opaque_chip);
    teardown(&f);
}

static void press_on_tab_starts_drag_and_captures_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, at(tab_rect(&f, 1), 5, 5), 0, 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 1);
    assert(!ui_tabbar_is_dragging(&f.tb));
    assert(ui_surface_captured(&f.surface) == bar(&f));
    teardown(&f);
}

static void removal_cancels_active_drag_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, at(tab_rect(&f, 1), 5, 5), 0, 1000);
    assert(ui_surface_captured(&f.surface) == bar(&f));
    assert(ui_tabbar_remove_tab(&f.tb, 0));
    show(&f, 2, -1);
    assert(!ui_tabbar_is_dragging(&f.tb));
    assert(ui_surface_captured(&f.surface) == NULL);
    release(&f, at(tab_rect(&f, 0), 5, 5), 1016);
    assert(count(&f, UI_ACTION_MOVE) == 0);
    teardown(&f);
}

static void motion_while_captured_reports_detach_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    struct vector2i grab = at(tab_rect(&f, 1), 5, 5);
    press(&f, grab, 0, 1000);
    clear_log(&f);

    struct vector2i far = { .x = 500, .y = 900 };
    motion(&f, far, 1010);
    assert(ui_tabbar_is_dragging(&f.tb));
    const struct ui_action *tear = last(&f, UI_ACTION_TEAR_OFF);
    assert(tear && tear->index == 1);
    const struct ui_action *drag = last(&f, UI_ACTION_DRAG);
    assert(drag && drag->index == 1 && drag->id == UI_TAB_DRAG_DETACH);
    assert(vector2i_eq(drag->pos, far));
    assert(ui_surface_captured(&f.surface) == bar(&f));
    teardown(&f);
}

static void motion_reorder_reports_drag_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, at(tab_rect(&f, 0), 5, 5), 0, 1000);
    struct vector2i to = at(tab_rect(&f, 1), 5, 5);
    motion(&f, to, 1010);
    const struct ui_action *drag = last(&f, UI_ACTION_DRAG);
    assert(drag && drag->index == 0 && drag->id == UI_TAB_DRAG_REORDER);
    assert(vector2i_eq(drag->pos, to));
    assert(count(&f, UI_ACTION_TEAR_OFF) == 0);
    teardown(&f);
}

static void release_reorder_commits_and_releases_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, at(tab_rect(&f, 0), 5, 5), 0, 1000);
    struct vector2i to = at(tab_rect(&f, 1), 5, 5);
    motion(&f, to, 1010);
    release(&f, to, 1020);
    assert(count(&f, UI_ACTION_MOVE) == 1);
    const struct ui_action *move = last(&f, UI_ACTION_MOVE);
    assert(move->index == 0 && move->to == 1);
    assert(ui_surface_captured(&f.surface) == NULL);
    assert(!ui_tabbar_is_dragging(&f.tb));
    teardown(&f);
}

static void release_detach_commits_and_releases_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    struct rect2i r1 = tab_rect(&f, 1);
    press(&f, at(r1, 5, 5), 0, 1000);
    struct vector2i far = { .x = 500, .y = 900 };
    motion(&f, far, 1010);
    release(&f, far, 1020);
    assert(count(&f, UI_ACTION_MOVE) == 1);
    const struct ui_action *move = last(&f, UI_ACTION_MOVE);
    assert(move->index == 1 && move->to == -1);
    /* pos.x is where the pointer held the tab. */
    assert(move->pos.x == 5);
    assert(ui_surface_captured(&f.surface) == NULL);
    teardown(&f);
}

static void press_close_button_closes_without_drag_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, center(close_rect(&f, 1)), 0, 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_CLOSE &&
           f.log[0].index == 1);
    assert(ui_surface_captured(&f.surface) == NULL);
    teardown(&f);
}

static void press_new_tab_requests_new_tab_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, center(new_tab_rect(&f)), 0, 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].id == UI_TABBAR_NEW_TAB && f.log[0].index == -1);
    assert(ui_surface_captured(&f.surface) == NULL);
    teardown(&f);
}

static void middle_click_closes_tab_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, at(tab_rect(&f, 2), 5, 5), 1, 1000);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_CLOSE &&
           f.log[0].index == 2);
    assert(ui_surface_captured(&f.surface) == NULL);
    teardown(&f);
}

static void release_without_movement_cancels_and_releases_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    struct vector2i p = at(tab_rect(&f, 0), 5, 5);
    press(&f, p, 0, 1000);
    assert(ui_surface_captured(&f.surface) == bar(&f));
    release(&f, p, 1016);
    assert(count(&f, UI_ACTION_MOVE) == 0 && count(&f, UI_ACTION_DRAG) == 0);
    assert(ui_surface_captured(&f.surface) == NULL);
    teardown(&f);
}

/* Regression: a handler that took the torn-off tab left the bar holding
 * the pointer, so the release still ended the bar's own drag. */
static void taken_tear_off_ends_the_gesture_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    f.take_tear_off = true;
    struct rect2i r0 = tab_rect(&f, 0);
    press(&f, at(tab_rect(&f, 1), 5, 5), 0, 1000);
    clear_log(&f);

    struct vector2i far = { .x = 200, .y = 250 };
    motion(&f, far, 1010);
    assert(count(&f, UI_ACTION_TEAR_OFF) == 1);
    assert(count(&f, UI_ACTION_DRAG) == 0);
    assert(!ui_tabbar_is_dragging(&f.tb));
    assert(ui_surface_captured(&f.surface) == NULL);

    motion(&f, (struct vector2i){ .x = 210, .y = 260 }, 1020);
    release(&f, far, 1030);
    assert(count(&f, UI_ACTION_MOVE) == 0);
    assert(count(&f, UI_ACTION_DRAG) == 0);
    assert(count(&f, UI_ACTION_TEAR_OFF) == 1);

    clear_log(&f);
    press(&f, at(r0, 5, 5), 0, 1100);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_ACTIVATE &&
           f.log[0].index == 0);
    assert(ui_surface_captured(&f.surface) == bar(&f));
    release(&f, at(r0, 5, 5), 1116);
    assert(count(&f, UI_ACTION_MOVE) == 0);
    teardown(&f);
}

static void capture_lost_cancels_the_drag_once_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    press(&f, at(tab_rect(&f, 0), 5, 5), 0, 1000);
    motion(&f, at(tab_rect(&f, 1), 5, 5), 1010);
    assert(ui_tabbar_is_dragging(&f.tb));
    clear_log(&f);

    ui_surface_cancel_capture(&f.surface);
    assert(f.n_log == 1 && f.log[0].kind == UI_ACTION_DRAG &&
           f.log[0].id == UI_TAB_DRAG_NONE && f.log[0].index == 0);
    assert(!ui_tabbar_is_dragging(&f.tb));
    ui_surface_cancel_capture(&f.surface);
    assert(f.n_log == 1);

    /* A press that never moved was never reported, so nothing cancels. */
    press(&f, at(tab_rect(&f, 2), 5, 5), 0, 1100);
    clear_log(&f);
    ui_surface_cancel_capture(&f.surface);
    assert(f.n_log == 0);
    teardown(&f);
}

/* A host may set the same labels on every prepare. That must not keep the
 * window from going idle. */
/* A bar that grew in once holds its target at full height with the clock
 * stopped. Closing a tab without the close animation and attaching another
 * one in that slot asks begin_open to grow the bar again, and the target it
 * sets is already the one the bar holds. */
static void reopen_after_a_close_animates_again(void)
{
    struct fixture f;
    setup(&f, 1, 400);
    open_tab(&f, 1, 2, 1000);
    uint64_t t = 1000;
    while (tick(&f, t))
        t += 16;
    assert(ui_tabbar_height(&f.tb) == ui_tabbar_steady_height(&f.tb));

    assert(ui_tabbar_remove_tab(&f.tb, 1));
    show(&f, 1, -1);
    while (tick(&f, t))
        t += 16;

    assert(ui_tabbar_insert_tab(&f.tb, 1));
    show(&f, 2, -1);
    assert(ui_tabbar_begin_open(&f.tb, 1, t));
    /* Still growing, so it is not at its steady height yet. */
    assert(ui_tabbar_height(&f.tb) < ui_tabbar_steady_height(&f.tb));
    while (tick(&f, t))
        t += 16;
    assert(ui_tabbar_height(&f.tb) == ui_tabbar_steady_height(&f.tb));
    teardown(&f);
}

/* The host calls set_external_drag on every pointer motion while a
 * foreign tab is over the window. A motion that moves nothing must not ask
 * for a frame, or the window can never go idle. */
static void still_external_drag_requests_nothing(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    struct rect2i tab;
    assert(ui_tabbar_tab_rect(&f.tb, 0, &tab));
    struct vector2i at_slot = center(tab);

    ui_tabbar_set_external_drag(&f.tb, true, at_slot);
    ui_surface_prepare(&f.surface);
    ui_surface_build_scene(&f.surface, &f.scene);
    ui_surface_take_frame_request(&f.surface);

    ui_tabbar_set_external_drag(&f.tb, true, at_slot);
    assert(!ui_surface_take_frame_request(&f.surface));

    /* The flag flipping does need the bar to resize. */
    ui_tabbar_set_external_drag(&f.tb, false, at_slot);
    assert(ui_surface_take_frame_request(&f.surface));
    teardown(&f);
}

static void settled_bar_goes_idle_test(void)
{
    struct fixture f;
    setup(&f, 2, 400);
    open_tab(&f, 2, 3, 1000);
    motion(&f, center(close_rect(&f, 0)), 1000);
    uint64_t t = 1000;
    while (tick(&f, t))
        t += 16;
    assert(t < 2000);
    assert(ui_surface_next_deadline(&f.surface) == 0);
    build(&f);
    ui_surface_take_frame_request(&f.surface);

    struct ui_tab_label same[3];
    char names[3][16];
    for (int i = 0; i < 3; i++) {
        snprintf(names[i], sizeof(names[i]), "tab%d", i);
        same[i] = (struct ui_tab_label){ .text = names[i] };
    }
    set_tabs(&f, same, 3);
    build(&f);
    assert(!ui_surface_take_frame_request(&f.surface));
    assert(!tick(&f, t + 16));
    assert(ui_surface_next_deadline(&f.surface) == 0);
    teardown(&f);
}

static void cursor_marks_buttons_test(void)
{
    struct fixture f;
    setup(&f, 3, 400);
    motion(&f, center(close_rect(&f, 1)), 1000);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_POINTER);
    motion(&f, at(tab_rect(&f, 1), 3, 3), 1010);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_DEFAULT);
    motion(&f, center(new_tab_rect(&f)), 1020);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_POINTER);
    teardown(&f);
}

static void press_ripple_ticks_until_done_and_uses_css(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".tab { ripple-color: #11223344; transition-duration: 200ms; }"));
    struct fixture f;
    setup_with(&f, &sheet, (struct vector2i){ .x = 400, .y = 100 },
               (struct vector2i){0});
    show(&f, 2, 0);
    tick(&f, 900);

    click(&f, at(tab_rect(&f, 0), 5, 5), 1000);
    assert(tick(&f, 1100));
    const struct ui_scene *scene = build(&f);
    bool found = false;
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].kind == UI_SCENE_OP_CIRCLE &&
            scene->ops[i].color == 0x44112233u)
            found = true;
    assert(found);

    assert(!tick(&f, 1200));
    assert(count_ops(build(&f), UI_SCENE_OP_CIRCLE) == 0);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void transparent_ripple_color_disables_ripple(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".tab { ripple-color: #00000000; }"));
    struct fixture f;
    setup_with(&f, &sheet, (struct vector2i){ .x = 400, .y = 100 },
               (struct vector2i){0});
    show(&f, 2, 0);
    click(&f, at(tab_rect(&f, 0), 5, 5), 1000);
    assert(!tick(&f, 1100));
    assert(count_ops(build(&f), UI_SCENE_OP_CIRCLE) == 0);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static bool damage_touches(const struct ui_damage *d, struct rect2i r)
{
    if (d->all)
        return true;
    for (int i = 0; i < d->n; i++)
        if (!rect2i_is_empty(rect2i_intersect(d->rects[i], r)))
            return true;
    return false;
}

/* Regression: the tab bar was one damage unit, so a new title in one tab
 * repainted the whole bar. Only the tab whose title changed is damage. */
static void title_change_damages_only_its_tab(void)
{
    struct fixture f;
    setup_with(&f, NULL, (struct vector2i){ .x = 1000, .y = 600 },
               (struct vector2i){0});
    struct ui_tab_label labels[] = {
        { .text = "bash", .active = false },
        { .text = "vim src/ui/scene.c", .active = true },
        { .text = "htop", .active = false },
    };
    struct ui_damage_tracker t = {0};
    struct ui_damage d;
    set_tabs(&f, labels, 3);
    ui_damage_track(&t, build(&f), &d);

    labels[1].text = "bash";
    set_tabs(&f, labels, 3);
    ui_damage_track(&t, build(&f), &d);

    assert(!d.all && d.n > 0);
    assert(damage_touches(&d, tab_rect(&f, 1)));
    assert(!damage_touches(&d, tab_rect(&f, 0)));
    assert(!damage_touches(&d, tab_rect(&f, 2)));
    ui_damage_tracker_destroy(&t);
    teardown(&f);
}

/* The bar copies its labels. More tabs than it held before, fewer, and
 * none must all paint from its own copy. */
static void labels_grow_and_shrink(void)
{
    struct fixture f;
    setup(&f, 8, 1600);
    char scratch[16];
    struct ui_tab_label borrowed[] = { { .text = scratch, .active = true } };
    snprintf(scratch, sizeof(scratch), "borrowed");
    show(&f, 9, 0);
    build(&f);
    show(&f, 3, 0);
    build(&f);
    show(&f, 12, 0);
    assert(find_text(build(&f), "tab11"));

    set_tabs(&f, borrowed, 1);
    show(&f, 2, 1);
    f.labels[0] = borrowed[0];
    set_tabs(&f, f.labels, 2);
    memset(scratch, 'x', sizeof(scratch) - 1);
    assert(find_text(build(&f), "borrowed"));

    /* A tab without a title shows a placeholder. */
    set_tabs(&f, NULL, 2);
    assert(find_text(build(&f), "shell"));

    set_tabs(&f, NULL, 0);
    assert(ui_tabbar_height(&f.tb) == 0);
    assert(count_ops(build(&f), UI_SCENE_OP_TEXT) == 0);
    teardown(&f);
}

int main(void)
{
    title_change_damages_only_its_tab();
    labels_grow_and_shrink();
    css_theme_drives_layout_and_scene();
    tabbar_edge_cases();
    tab_click_and_close_activation();
    tabbar_keyboard_navigation_test();
    tabbar_key_requests_frame_test();
    close_hover_crossfade_is_visible();
    hover_and_active_styling_in_scene();
    height_scales_with_dpi();
    restyled_bar_repaints_test();
    bg_color_requests_frame_test();
    sheet_swap_relayouts_bar_test();
    host_restyle_during_motion_test();
    last_tab_claims_trailing_space();
    text_runs_stay_on_codepoint_boundaries();
    tabbar_drag_sort_detach_attach_test();
    sideways_exit_detaches_test();
    external_drag_reveals_single_tab_target_test();
    incoming_tab_has_translucent_drop_preview();
    bar_away_from_the_origin_uses_surface_space();
    tabbar_build_drag_preview_test();
    drag_image_card_follows_the_sheet_test();
    detaching_tab_still_has_drag_image_test();
    detach_drag_chip_bounds_test();
    title_and_close_do_not_overlap();
    new_tab_hover_animates();
    zeroed_tabbar_is_idle();
    closing_tab_tracks_through_remove();
    open_tab_grows_in();
    open_tab_reflows_siblings();
    open_tab_keeps_new_chip_hittable();
    close_tab_keeps_new_chip_hittable();
    close_tab_reflows_siblings();
    open_close_widths_are_continuous();
    vertical_padding_is_clamped();
    chrome_glyphs_are_vertically_centered();
    separators_hide_next_to_hot_tabs();
    neighbor_slide_eases_on_drag();
    bar_reveal_animates_on_boundary();
    opening_chip_is_opaque_and_clipped();
    fifty_tabs_frame_time_sub_half_ms();
    press_on_tab_starts_drag_and_captures_test();
    removal_cancels_active_drag_test();
    motion_while_captured_reports_detach_test();
    motion_reorder_reports_drag_test();
    release_reorder_commits_and_releases_test();
    release_detach_commits_and_releases_test();
    press_close_button_closes_without_drag_test();
    press_new_tab_requests_new_tab_test();
    middle_click_closes_tab_test();
    release_without_movement_cancels_and_releases_test();
    taken_tear_off_ends_the_gesture_test();
    capture_lost_cancels_the_drag_once_test();
    settled_bar_goes_idle_test();
    reopen_after_a_close_animates_again();
    still_external_drag_requests_nothing();
    cursor_marks_buttons_test();
    press_ripple_ticks_until_done_and_uses_css();
    transparent_ripple_color_disables_ripple();
    puts("PASS: tabbar tests");
    return 0;
}
