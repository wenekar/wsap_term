#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "paint/scene.h"
#include "paint/shadow.h"
#include "ui/ui.h"

static const ui_menu_item SUB_ITEMS[] = {
    { .label = "Dark", .action = 100 },
    { .label = "Light", .action = 101 },
};

static const ui_menu_item ITEMS[] = {
    { .label = "Copy", .action = 1 },
    { .label = NULL, .action = 0, .flags = UI_LIST_ITEM_SEPARATOR },
    { .label = "Paste", .action = 2, .flags = UI_LIST_ITEM_DISABLED },
    { .label = "Theme", .action = 10, .flags = UI_LIST_ITEM_SUBMENU },
    { .label = "Quit", .action = 5, .shortcut = "Ctrl+Q" },
};
#define N_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_menu menu;
    struct ui_menu_layer layer;
    struct ui_scene scene;
    int activated;
    int highlight;
    int closes;
    int submenus;
};

static bool on_action(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    switch (action->kind) {
    case UI_ACTION_ACTIVATE: f->activated = action->id; break;
    case UI_ACTION_HIGHLIGHT: f->highlight = action->id; break;
    case UI_ACTION_CLOSE: f->closes++; break;
    default: break;
    }
    return true;
}

static const ui_menu_item *on_submenu(void *userdata, int action, int *n)
{
    struct fixture *f = userdata;
    f->submenus++;
    if (action == 11) {
        static const ui_menu_item other[] = { { .label = "Png", .action = 200 } };
        *n = 1;
        return other;
    }
    *n = (int)(sizeof(SUB_ITEMS) / sizeof(SUB_ITEMS[0]));
    return SUB_ITEMS;
}

static void setup(struct fixture *f, const struct css_sheet *sheet)
{
    *f = (struct fixture){ .activated = -1, .highlight = -2 };
    ui_widget_init(&f->root, NULL, NULL);
    ui_menu_init(&f->menu);
    ui_menu_set_action_handler(&f->menu, on_action, f);
    ui_menu_set_submenu_provider(&f->menu, on_submenu, f);
    ui_menu_layer_init(&f->layer, &f->menu);
    ui_menu_layer_set_origin(&f->layer, (struct vector2i){ .x = 20, .y = 20 });
    assert(ui_widget_add_child(&f->root, ui_menu_layer_widget(&f->layer)));
    ui_surface_set_sheet(&f->surface, sheet);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 800, .y = 600 });
}

static void open(struct fixture *f, const ui_menu_item *items, int n)
{
    assert(ui_menu_open(&f->menu, items, n));
    ui_surface_prepare(&f->surface);
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(&f->root);
    ui_menu_destroy(&f->menu);
    ui_scene_destroy(&f->scene);
}

static void key(struct fixture *f, enum ui_key k)
{
    struct ui_event e = ui_event_key((struct ui_key_event){ .key = k }, 0);
    ui_surface_dispatch(&f->surface, &e);
    ui_surface_prepare(&f->surface);
}

static void pointer(struct fixture *f, enum pointer_kind kind,
                    struct vector2i pos, uint64_t time_ms)
{
    struct ui_event e = ui_event_pointer((struct pointer_event){
        .kind = kind, .pos = pos,
    }, time_ms);
    ui_surface_dispatch(&f->surface, &e);
    ui_surface_prepare(&f->surface);
}

static void tick(struct fixture *f, uint64_t now_ms)
{
    ui_surface_tick(&f->surface, now_ms);
    ui_surface_prepare(&f->surface);
}

static struct ui_list *level_list(struct fixture *f, int depth)
{
    return &f->menu.levels[depth].list;
}

static struct rect2i item_rect(struct fixture *f, int depth, int index)
{
    return ui_list_item_rect(level_list(f, depth), index);
}

static struct vector2i center(struct rect2i r)
{
    return (struct vector2i){ .x = r.x + r.width / 2, .y = r.y + r.height / 2 };
}

static bool close_again(struct ui_widget *source, const struct ui_action *action,
                        void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    if (action->kind == UI_ACTION_CLOSE) {
        f->closes++;
        ui_menu_close(&f->menu);
    }
    return true;
}

static void close_handler_can_close_again_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    ui_menu_set_action_handler(&f.menu, close_again, &f);
    open(&f, ITEMS, N_ITEMS);
    ui_menu_close(&f.menu);
    assert(f.closes == 1 && !ui_menu_is_open(&f.menu));
    teardown(&f);
}

static void closed_menu_does_nothing_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    assert(!ui_menu_is_open(&f.menu));
    assert(ui_menu_level_count(&f.menu) == 0);
    ui_menu_close(&f.menu);
    assert(f.closes == 0);
    assert(ui_menu_tick(&f.menu, 1000) == 0);
    assert(ui_widget_is_hidden(ui_menu_layer_widget(&f.layer)));
    assert(!ui_surface_has_modal(&f.surface));
    teardown(&f);
}

static void open_and_keyboard_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    open(&f, ITEMS, N_ITEMS);
    assert(ui_surface_has_modal(&f.surface));
    struct rect2i r = ui_menu_level_widget(&f.menu, 0)->bounds;
    assert(r.x == 20 && r.y == 20 && r.width > 0 && r.height > 0);

    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_DOWN);
    assert(f.highlight == 10);
    key(&f, UI_KEY_RIGHT);
    assert(ui_menu_level_count(&f.menu) == 2 && f.submenus == 1);
    assert(f.highlight == 100);
    key(&f, UI_KEY_ENTER);
    assert(f.activated == 100 && f.closes == 1);
    assert(!ui_menu_is_open(&f.menu));
    assert(!ui_surface_has_modal(&f.surface));

    open(&f, ITEMS, N_ITEMS);
    key(&f, UI_KEY_ESCAPE);
    assert(!ui_menu_is_open(&f.menu) && f.closes == 2);

    open(&f, ITEMS, N_ITEMS);
    struct ui_event q = ui_event_text("q", 1, 0);
    ui_surface_dispatch(&f.surface, &q);
    assert(f.activated == 5);
    teardown(&f);
}

static void menu_keys_request_frame_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    open(&f, ITEMS, N_ITEMS);
    ui_surface_take_frame_request(&f.surface);

    key(&f, UI_KEY_DOWN);
    assert(ui_surface_frame_pending(&f.surface));
    ui_surface_take_frame_request(&f.surface);

    key(&f, UI_KEY_ESCAPE);
    assert(!ui_menu_is_open(&f.menu));
    assert(ui_surface_frame_pending(&f.surface));
    teardown(&f);
}

static void submenu_first_row_aligns_with_trigger_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    open(&f, ITEMS, N_ITEMS);
    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_RIGHT);
    assert(ui_menu_level_count(&f.menu) == 2);
    struct rect2i trigger = item_rect(&f, 0, 3);
    struct rect2i first = item_rect(&f, 1, 0);
    assert(first.y == trigger.y && first.height == trigger.height);
    assert(first.x >= rect2i_end(trigger).x);
    teardown(&f);
}

static void closing_submenu_reports_parent_highlight_test(void)
{
    static const enum ui_key keys[] = {
        UI_KEY_ESCAPE, UI_KEY_LEFT, UI_KEY_BACKSPACE,
    };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        struct fixture f;
        setup(&f, NULL);
        open(&f, ITEMS, N_ITEMS);
        key(&f, UI_KEY_DOWN);
        key(&f, UI_KEY_DOWN);
        key(&f, UI_KEY_RIGHT);
        assert(f.highlight == 100);
        key(&f, keys[k]);
        assert(ui_menu_level_count(&f.menu) == 1);
        assert(f.highlight == 10);
        teardown(&f);
    }
}

static void pointer_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    open(&f, ITEMS, N_ITEMS);
    struct vector2i p = center(item_rect(&f, 0, 0));
    pointer(&f, PTR_MOTION, p, 1);
    assert(f.highlight == 1);
    pointer(&f, PTR_PRESS, p, 2);
    pointer(&f, PTR_RELEASE, p, 3);
    assert(f.activated == 1 && f.closes == 1);

    open(&f, ITEMS, N_ITEMS);
    struct vector2i off = center(item_rect(&f, 0, 2));
    pointer(&f, PTR_PRESS, off, 4);
    pointer(&f, PTR_RELEASE, off, 5);
    assert(ui_menu_is_open(&f.menu));
    pointer(&f, PTR_PRESS, (struct vector2i){ .x = 700, .y = 500 }, 6);
    assert(!ui_menu_is_open(&f.menu) && f.closes == 2);
    teardown(&f);
}

static void hover_delay_and_aim_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    open(&f, ITEMS, N_ITEMS);
    struct rect2i trigger = item_rect(&f, 0, 3);
    pointer(&f, PTR_MOTION, center(trigger), 1000);
    assert(f.highlight == 10);
    tick(&f, 1200);
    assert(ui_menu_level_count(&f.menu) == 1);
    assert(ui_surface_next_deadline(&f.surface) == 1400);
    tick(&f, 1450);
    assert(ui_menu_level_count(&f.menu) == 2);
    assert(f.highlight != 100);
    assert(ui_list_selected(level_list(&f, 1)) == -1);

    struct rect2i quit = item_rect(&f, 0, 4);
    pointer(&f, PTR_MOTION,
            (struct vector2i){ .x = rect2i_end(quit).x - 2, .y = quit.y + 2 },
            1460);
    assert(ui_menu_level_count(&f.menu) == 2);
    tick(&f, 1600);
    assert(ui_menu_level_count(&f.menu) == 2);
    tick(&f, 1800);
    assert(ui_menu_level_count(&f.menu) == 1);

    pointer(&f, PTR_MOTION, center(trigger), 2000);
    tick(&f, 2500);
    assert(ui_menu_level_count(&f.menu) == 2);
    pointer(&f, PTR_MOTION, center(item_rect(&f, 1, 0)), 2510);
    assert(f.highlight == 100);
    pointer(&f, PTR_MOTION, center(trigger), 2520);
    pointer(&f, PTR_MOTION, center(item_rect(&f, 0, 0)), 2530);
    assert(ui_menu_level_count(&f.menu) == 1);
    assert(ui_surface_next_deadline(&f.surface) == 0 ||
           ui_menu_level_count(&f.menu) == 1);
    teardown(&f);
}

static const ui_menu_item TWO_SUBMENUS[] = {
    { .label = "Theme", .action = 10, .flags = UI_LIST_ITEM_SUBMENU },
    { .label = "Export", .action = 11, .flags = UI_LIST_ITEM_SUBMENU },
};

static void sibling_submenu_replaces_the_level_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    open(&f, TWO_SUBMENUS, 2);
    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_RIGHT);
    assert(ui_menu_level_count(&f.menu) == 2);
    assert(!strcmp(ui_list_item(level_list(&f, 1), 0)->label, "Dark"));
    uint32_t gen = ui_menu_generation(&f.menu);
    key(&f, UI_KEY_LEFT);
    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_RIGHT);
    assert(ui_menu_level_count(&f.menu) == 2);
    assert(ui_menu_generation(&f.menu) != gen);
    assert(!strcmp(ui_list_item(level_list(&f, 1), 0)->label, "Png"));
    struct rect2i first = item_rect(&f, 1, 0);
    assert(first.y == item_rect(&f, 0, 1).y);
    teardown(&f);
}

static bool scene_has_color(const struct ui_scene *s, uint32_t argb)
{
    for (int i = 0; i < s->n_ops; i++)
        if (s->ops[i].kind != UI_SCENE_OP_TEXT && s->ops[i].color == argb)
            return true;
    return false;
}

static void open_menu_keeps_its_geometry_test(void)
{
    static struct css_sheet roomy;
    assert(css_parse(&roomy, ".menu-item { padding: 12px 20px; }"
                             ".menu-list { padding: 10px; }"
                             ".context-menu { background: #654321; }"
                             ".menu-item:hover { background: #123456; }"));
    struct fixture f;
    setup(&f, NULL);
    open(&f, ITEMS, N_ITEMS);
    struct rect2i bounds = ui_menu_level_widget(&f.menu, 0)->bounds;
    struct rect2i item = item_rect(&f, 0, 0);
    pointer(&f, PTR_MOTION, center(item), 1);
    assert(ui_list_hovered(level_list(&f, 0)) == 0);

    ui_surface_set_sheet(&f.surface, &roomy);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(rect2i_eq(ui_menu_level_widget(&f.menu, 0)->bounds, bounds));
    assert(rect2i_eq(item_rect(&f, 0, 0), item));
    assert(ui_list_hovered(level_list(&f, 0)) == 0);
    assert(scene_has_color(&f.scene, 0xFF654321u));
    assert(scene_has_color(&f.scene, 0xFF123456u));

    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_DOWN);
    key(&f, UI_KEY_RIGHT);
    assert(item_rect(&f, 1, 0).height == item.height);

    ui_menu_close(&f.menu);
    ui_surface_prepare(&f.surface);
    open(&f, ITEMS, N_ITEMS);
    assert(item_rect(&f, 0, 0).height > item.height);
    teardown(&f);
}

static void panel_decoration_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".context-menu { background: #112233; border-width: 2px; "
        "border-color: #445566; border-radius: 8px; "
        "box-shadow: 2px 3px 4px #10203080; }"));
    struct fixture f;
    setup(&f, &sheet);
    open(&f, ITEMS, N_ITEMS);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    struct vector2i at = ui_menu_level_widget(&f.menu, 0)->bounds.pos;
    struct insets2i shadow = ui_menu_level_shadow(&f.menu, 0);
    assert(shadow.l > 0 && shadow.b > shadow.t);

    int extent = ui_shadow_extent(4);
    int shadow_ops = 0, first_panel_op = -1;
    bool border = false, fill = false;
    for (int i = 0; i < f.scene.n_ops; i++) {
        const struct ui_scene_op *op = &f.scene.ops[i];
        if (first_panel_op < 0 && (op->kind == UI_SCENE_OP_RECT ||
                                   op->kind == UI_SCENE_OP_ROUNDED_RECT ||
                                   op->kind == UI_SCENE_OP_SHADOW))
            first_panel_op = op->kind == UI_SCENE_OP_SHADOW ? 0 : 1;
        if (op->kind == UI_SCENE_OP_SHADOW) {
            shadow_ops++;
            assert((op->color & 0x00FFFFFFu) == 0x102030u);
            assert(op->radius == 8 && op->blur == 4);
            assert(op->rect.x == at.x + 2 - extent &&
                   op->rect.y == at.y + 3 - extent);
        } else if (op->kind == UI_SCENE_OP_ROUNDED_RECT) {
            if (op->color == 0xFF445566u) {
                border = true;
                assert(op->radius == 8);
            } else if (op->color == 0xFF112233u) {
                fill = true;
                assert(op->radius == 6);
            }
        }
    }
    assert(shadow_ops == 1 && first_panel_op == 0);
    assert(border && fill);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void menu_show_desc_test(void)
{
    struct fixture f;
    setup(&f, NULL);

    const ui_menu_item items[] = {
        UI_MENU_ITEM("One", 1),
        UI_MENU_SEPARATOR,
        UI_MENU_ITEM("Two", 2),
    };

    assert(ui_menu_show(&f.menu, &f.layer, &(struct ui_menu_desc){
        .pos = (struct vector2i){ .x = 50, .y = 60 },
        .items = items,
        .count = 3,
        .on_action = on_action,
        .userdata = &f,
    }));
    assert(ui_menu_is_open(&f.menu));
    assert(ui_menu_level_count(&f.menu) == 1);

    ui_menu_close(&f.menu);
    assert(!ui_menu_is_open(&f.menu));
    assert(f.closes >= 1);

    teardown(&f);
}

int main(void)
{
    close_handler_can_close_again_test();
    closed_menu_does_nothing_test();
    open_and_keyboard_test();
    menu_keys_request_frame_test();
    submenu_first_row_aligns_with_trigger_test();
    closing_submenu_reports_parent_highlight_test();
    pointer_test();
    hover_delay_and_aim_test();
    sibling_submenu_replaces_the_level_test();
    open_menu_keeps_its_geometry_test();
    panel_decoration_test();
    menu_show_desc_test();
    puts("PASS: menu tests");
    return 0;
}
