#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/debug.h"
#include "paint/scene.h"
#include "ui/ui.h"

static const struct ui_list_item ITEMS[] = {
    { .label = "Copy", .shortcut = "Ctrl+C", .action = 1 },
    { .label = "Paste", .shortcut = "Ctrl+V", .action = 2 },
    { .label = "Cut", .action = 3 },
};
#define N_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_list list;
    struct ui_scene scene;
};

static void show(struct fixture *f, const struct ui_list_item *items, int n,
                 const struct css_sheet *sheet, struct css_scale_context scale)
{
    *f = (struct fixture){0};
    ui_widget_init(&f->root, NULL, NULL);
    ui_list_init(&f->list);
    ui_list_set_style_class(&f->list, "context-menu");
    assert(ui_list_set_items(&f->list, items, n));
    ui_widget_set_placement(ui_list_widget(&f->list),
                            (struct ui_placement){ .kind = UI_PLACE_AT });
    assert(ui_widget_add_child(&f->root, ui_list_widget(&f->list)));
    ui_surface_set_sheet(&f->surface, sheet);
    ui_surface_set_scale(&f->surface, scale);
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 800, .y = 600 });
}

static void show_default(struct fixture *f)
{
    show(f, ITEMS, N_ITEMS, NULL, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(&f->root);
    ui_scene_destroy(&f->scene);
}

static const struct ui_scene_op *build(struct fixture *f)
{
    assert(ui_surface_build_scene(&f->surface, &f->scene));
    return f->scene.ops;
}

static const struct ui_scene_op *find_text(const struct ui_scene *s,
                                           const char *t)
{
    size_t len = strlen(t);
    for (int i = 0; i < s->n_ops; i++) {
        const struct ui_scene_op *op = &s->ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == len &&
            !memcmp(ui_scene_op_text(s, op), t, len))
            return op;
    }
    return NULL;
}

static struct vector2i row_center(const struct ui_list *l, int index)
{
    struct rect2i r = ui_list_item_rect(l, index);
    return (struct vector2i){ .x = r.x + r.width / 2, .y = r.y + r.height / 2 };
}

static bool pointer(struct ui_surface *s, enum ui_pointer_kind kind,
                    struct vector2i pos)
{
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos,
    }, 0);
    return ui_surface_dispatch(s, &e);
}

static bool key(struct ui_surface *s, enum ui_key k)
{
    struct ui_event e = ui_event_key((struct ui_key_event){ .key = k }, 0);
    return ui_surface_dispatch(s, &e);
}

static int action_count;
static struct ui_action last_action;

static bool record_action(struct ui_widget *source,
                          const struct ui_action *action, void *userdata)
{
    (void)source;
    (void)userdata;
    action_count++;
    last_action = *action;
    return false;
}

static void label_and_shortcut_do_not_overlap_test(void)
{
    struct fixture f;
    show_default(&f);
    build(&f);
    const struct ui_scene_op *label = find_text(&f.scene, "Copy");
    const struct ui_scene_op *shortcut = find_text(&f.scene, "Ctrl+C");
    assert(label && shortcut);
    assert(rect2i_is_empty(rect2i_intersect(label->rect, shortcut->rect)));
    teardown(&f);
}

static void click_activates_item_test(void)
{
    struct fixture f;
    show_default(&f);
    ui_widget_set_action_handler(ui_list_widget(&f.list), record_action, NULL);
    action_count = 0;
    struct vector2i p = row_center(&f.list, 1);
    assert(pointer(&f.surface, UI_POINTER_PRESS, p));
    assert(ui_list_hovered(&f.list) == 1);
    assert(pointer(&f.surface, UI_POINTER_RELEASE, p));
    assert(last_action.kind == UI_ACTION_ACTIVATE);
    assert(last_action.id == 2 && last_action.index == 1);
    assert(ui_list_selected(&f.list) == 1);
    teardown(&f);
}

static void hover_follows_pointer_test(void)
{
    struct fixture f;
    show_default(&f);
    ui_widget_set_action_handler(ui_list_widget(&f.list), record_action, NULL);
    pointer(&f.surface, UI_POINTER_MOTION, row_center(&f.list, 0));
    assert(ui_list_hovered(&f.list) == 0);
    assert(last_action.kind == UI_ACTION_HIGHLIGHT && last_action.id == 1);
    pointer(&f.surface, UI_POINTER_MOTION, row_center(&f.list, 1));
    assert(ui_list_hovered(&f.list) == 1);
    pointer(&f.surface, UI_POINTER_MOTION, (struct vector2i){ .x = 700, .y = 500 });
    assert(ui_list_hovered(&f.list) == -1);
    assert(last_action.kind == UI_ACTION_HIGHLIGHT && last_action.index == -1);
    teardown(&f);
}

static void cursor_names_the_hovered_row_test(void)
{
    struct fixture f;
    show_default(&f);
    pointer(&f.surface, UI_POINTER_MOTION, row_center(&f.list, 0));
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_POINTER);

    pointer(&f.surface, UI_POINTER_MOTION, (struct vector2i){ .x = 700, .y = 500 });
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_DEFAULT);
    teardown(&f);
}

static void cursor_follows_the_sheet_test(void)
{
    struct css_sheet sheet = { 0 };
    assert(css_parse(&sheet, ".menu-item { cursor: text; }"));
    struct fixture f;
    show(&f, ITEMS, N_ITEMS, &sheet, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    pointer(&f.surface, UI_POINTER_MOTION, row_center(&f.list, 1));
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_TEXT);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void keys_move_and_activate_test(void)
{
    const struct ui_list_item items[] = {
        { .label = "One", .action = 1 },
        { .flags = UI_LIST_ITEM_SEPARATOR },
        { .label = "Off", .action = 2, .flags = UI_LIST_ITEM_DISABLED },
        { .label = "Two", .action = 3 },
    };
    struct fixture f;
    show(&f, items, 4, NULL, (struct css_scale_context){0});
    ui_widget_set_action_handler(ui_list_widget(&f.list), record_action, NULL);
    ui_surface_focus(&f.surface, ui_list_widget(&f.list));
    assert(key(&f.surface, UI_KEY_DOWN));
    assert(ui_list_selected(&f.list) == 0);
    assert(key(&f.surface, UI_KEY_DOWN));
    assert(ui_list_selected(&f.list) == 3);
    assert(key(&f.surface, UI_KEY_DOWN));
    assert(ui_list_selected(&f.list) == 0);
    assert(key(&f.surface, UI_KEY_END));
    assert(ui_list_selected(&f.list) == 3);
    assert(key(&f.surface, UI_KEY_ENTER));
    assert(last_action.kind == UI_ACTION_ACTIVATE && last_action.id == 3);
    teardown(&f);
}

static void replacing_items_keeps_attachment_and_handlers_test(void)
{
    struct fixture f;
    show_default(&f);
    struct ui_widget *control = ui_list_widget(&f.list);
    ui_widget_set_action_handler(control, record_action, NULL);
    ui_surface_focus(&f.surface, control);

    const struct ui_list_item replacement[] = {
        { .label = "Open", .action = 9 },
        { .label = "Close", .action = 10 },
    };
    assert(ui_list_set_items(&f.list, replacement, 2));
    assert(control->parent == &f.root);
    assert(ui_widget_surface(control) == &f.surface);
    assert(ui_surface_focused(&f.surface) == control);

    action_count = 0;
    ui_list_select(&f.list, 0);
    assert(action_count == 1);
    assert(last_action.kind == UI_ACTION_CHANGE);
    assert(last_action.id == 9 && last_action.index == 0);
    teardown(&f);
}

static bool destroy_on_change(struct ui_widget *source,
                              const struct ui_action *action, void *userdata)
{
    int *activations = userdata;
    if (action->kind == UI_ACTION_CHANGE)
        ui_widget_destroy(source);
    else if (action->kind == UI_ACTION_ACTIVATE)
        (*activations)++;
    return false;
}

static void handler_can_destroy_list_during_release_test(void)
{
    struct fixture f;
    show_default(&f);
    int activations = 0;
    ui_widget_set_action_handler(ui_list_widget(&f.list), destroy_on_change,
                                 &activations);
    struct vector2i p = row_center(&f.list, 0);
    pointer(&f.surface, UI_POINTER_PRESS, p);
    pointer(&f.surface, UI_POINTER_RELEASE, p);
    assert(activations == 0);
    assert(ui_widget_child_count(&f.root) == 0);
    teardown(&f);
}

static void hover_fill_stays_inside_rounded_corner_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".context-menu { background: #141618; border-radius: 4px; }"
        ".menu-item:hover { background: #3DAEE9; }"));
    struct fixture f;
    show(&f, ITEMS, N_ITEMS, &sheet, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32,
    });
    pointer(&f.surface, UI_POINTER_MOTION, row_center(&f.list, 0));
    build(&f);
    const struct ui_scene_op *panel = NULL, *row_fill = NULL;
    for (int i = 0; i < f.scene.n_ops; i++) {
        const struct ui_scene_op *op = &f.scene.ops[i];
        if (op->kind == UI_SCENE_OP_ROUNDED_RECT && op->radius > 0 &&
            (op->color & 0x00FFFFFFu) == 0x141618u)
            panel = op;
        if ((op->kind == UI_SCENE_OP_RECT ||
             op->kind == UI_SCENE_OP_ROUNDED_RECT) &&
            (op->color & 0x00FFFFFFu) == 0x3DAEE9u)
            row_fill = op;
    }
    assert(panel && row_fill);
    assert(row_fill->rect.y >= panel->rect.y + panel->radius);
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void rows_center_and_inset_text(struct css_scale_context scale)
{
    const struct ui_list_item items[] = {
        { .label = "Copy", .shortcut = "Ctrl+C", .action = 1 },
        { .flags = UI_LIST_ITEM_SEPARATOR },
        { .label = "Paste", .action = 2 },
    };
    struct fixture f;
    show(&f, items, 3, NULL, scale);
    build(&f);
    struct ui_debug_view view = { .root = &f.root, .scene = &f.scene,
                                  .surface = &f.surface };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);

    struct rect2i row = ui_list_item_rect(&f.list, 0);
    struct rect2i label = ui_debug_text_box(&f.scene, find_text(&f.scene, "Copy"));
    struct rect2i shortcut = ui_debug_text_box(&f.scene,
                                               find_text(&f.scene, "Ctrl+C"));
    int left = label.x - row.x;
    int right = rect2i_end(row).x - rect2i_end(shortcut).x;
    assert(left > 0 && left == right);

    struct rect2i sep = ui_list_item_rect(&f.list, 1);
    int lines = 0;
    for (int i = 0; i < f.scene.n_ops; i++) {
        const struct ui_scene_op *op = &f.scene.ops[i];
        if (op->kind != UI_SCENE_OP_RECT || !rect2i_contains(sep, op->rect.pos))
            continue;
        lines++;
        assert(op->rect.height < sep.height);
        int above = op->rect.y - sep.y;
        int below = rect2i_end(sep).y - rect2i_end(op->rect).y;
        assert(abs(above - below) <= 1);
    }
    assert(lines == 1);
    teardown(&f);
}

static void rows_center_and_inset_text_test(void)
{
    rows_center_and_inset_text((struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 17 });
    rows_center_and_inset_text((struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 34 });
}

static void check_mark_does_not_outlive_its_item_test(void)
{
    const struct ui_list_item checked[] = {
        { .label = "Alpha", .action = 1, .flags = UI_LIST_ITEM_CHECKED },
    };
    const struct ui_list_item plain[] = { { .label = "Beta", .action = 2 } };
    struct fixture f;
    show(&f, checked, 1, NULL, (struct css_scale_context){0});
    build(&f);
    assert(find_text(&f.scene, "\xe2\x9c\x93"));
    assert(ui_list_set_items(&f.list, plain, 1));
    build(&f);
    assert(!find_text(&f.scene, "\xe2\x9c\x93"));
    teardown(&f);
}

static void many_long_items_are_kept_test(void)
{
    enum { N = 300 };
    static struct ui_list_item items[N];
    static char labels[N][160];
    for (int i = 0; i < N; i++) {
        snprintf(labels[i], sizeof(labels[i]),
                 "item %03d with a label far longer than any fixed buffer "
                 "the list used to copy it into, padded to 150 chars....", i);
        items[i] = (struct ui_list_item){
            .label = labels[i], .action = i,
            .shortcut = "Ctrl+Shift+Alt+Super+F12",
        };
    }
    struct ui_list l;
    ui_list_init(&l);
    assert(ui_list_set_items(&l, items, N));
    assert(ui_list_count(&l) == N);
    const struct ui_list_item *last = ui_list_item(&l, N - 1);
    assert(last && !strcmp(last->label, labels[N - 1]));
    assert(!strcmp(last->shortcut, "Ctrl+Shift+Alt+Super+F12"));
    ui_widget_destroy(ui_list_widget(&l));
}

static void pinned_geometry_survives_theme_change_test(void)
{
    struct css_sheet roomy = {0};
    assert(css_parse(&roomy, ".menu-list .menu-item { padding: 10px 20px; }"
                             ".menu-item:hover { background: #102030; }"));
    struct fixture f;
    show_default(&f);
    ui_list_pin_metrics(&f.list, NULL);
    build(&f);
    struct rect2i before = ui_list_item_rect(&f.list, 1);
    ui_surface_set_sheet(&f.surface, &roomy);
    pointer(&f.surface, UI_POINTER_MOTION, row_center(&f.list, 1));
    build(&f);
    assert(rect2i_eq(ui_list_item_rect(&f.list, 1), before));
    bool repainted = false;
    for (int i = 0; i < f.scene.n_ops; i++)
        repainted |= (f.scene.ops[i].color & 0xFFFFFFu) == 0x102030u;
    assert(repainted);
    teardown(&f);
    css_sheet_clear(&roomy);
}

static bool row_filled_with(struct fixture *f, int index, uint32_t rgb)
{
    struct rect2i row = ui_list_item_rect(&f->list, index);
    for (int i = 0; i < f->scene.n_ops; i++) {
        const struct ui_scene_op *op = &f->scene.ops[i];
        if ((op->kind == UI_SCENE_OP_RECT ||
             op->kind == UI_SCENE_OP_ROUNDED_RECT) &&
            (op->color & 0x00FFFFFFu) == rgb &&
            rect2i_contains(row, op->rect.pos))
            return true;
    }
    return false;
}

static void item_flags_pick_their_class_rule_test(void)
{
    const struct ui_list_item items[] = {
        { .label = "Plain", .action = 1 },
        { .label = "More", .action = 2, .flags = UI_LIST_ITEM_SUBMENU },
        { .label = "Off", .action = 3, .flags = UI_LIST_ITEM_DISABLED },
        { .label = "Both", .action = 4,
          .flags = UI_LIST_ITEM_SUBMENU | UI_LIST_ITEM_DISABLED },
    };
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".menu-item.submenu { background: #AA0001; }"
        ".menu-item.disabled { background: #0000BB; }"
        ".menu-item.submenu.disabled { background: #00AA00; }"));
    struct fixture f;
    show(&f, items, 4, &sheet, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    build(&f);
    assert(!row_filled_with(&f, 0, 0xAA0001u));
    assert(!row_filled_with(&f, 0, 0x0000BBu));
    assert(!row_filled_with(&f, 0, 0x00AA00u));
    assert(row_filled_with(&f, 1, 0xAA0001u));
    assert(!row_filled_with(&f, 1, 0x0000BBu));
    assert(row_filled_with(&f, 2, 0x0000BBu));
    assert(!row_filled_with(&f, 2, 0xAA0001u));
    assert(row_filled_with(&f, 3, 0x00AA00u));
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void non_focusable_list_does_not_take_focus_test(void)
{
    struct fixture f;
    show_default(&f);
    ui_list_set_focusable(&f.list, false);
    const struct ui_list_item items[] = {
        { .label = "One", .action = 1 },
        { .label = "Two", .action = 2 },
    };
    assert(ui_list_set_items(&f.list, items, 2));
    assert(ui_surface_focused(&f.surface) == NULL);

    pointer(&f.surface, UI_POINTER_PRESS, row_center(&f.list, 0));
    assert(ui_surface_focused(&f.surface) == NULL);

    assert(!ui_surface_focus_next(&f.surface));
    assert(ui_surface_focused(&f.surface) == NULL);

    teardown(&f);
}

int main(void)
{
    label_and_shortcut_do_not_overlap_test();
    click_activates_item_test();
    hover_follows_pointer_test();
    cursor_names_the_hovered_row_test();
    cursor_follows_the_sheet_test();
    keys_move_and_activate_test();
    replacing_items_keeps_attachment_and_handlers_test();
    handler_can_destroy_list_during_release_test();
    hover_fill_stays_inside_rounded_corner_test();
    rows_center_and_inset_text_test();
    check_mark_does_not_outlive_its_item_test();
    many_long_items_are_kept_test();
    pinned_geometry_survives_theme_change_test();
    item_flags_pick_their_class_rule_test();
    non_focusable_list_does_not_take_focus_test();
    puts("PASS: list tests");
    return 0;
}
