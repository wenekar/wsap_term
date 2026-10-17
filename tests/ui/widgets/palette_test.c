#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/debug.h"
#include "paint/scene.h"
#include "ui/ui.h"

static const struct ui_palette_cmd CMDS[] = {
    { .label = "New Tab", .shortcut = "Ctrl+Shift+T", .action = 1 },
    { .label = "Close Tab", .action = 2 },
    { .label = "New Window", .action = 3 },
    { .label = "Paste", .shortcut = "Ctrl+Shift+V", .action = 4 },
};
#define N_CMDS ((int)(sizeof(CMDS) / sizeof(CMDS[0])))

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_button behind;
    struct ui_palette palette;
    struct ui_scene scene;
    int activated;
    int closed;
};

static bool on_action(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    if (action->kind == UI_ACTION_ACTIVATE)
        f->activated = action->id;
    else if (action->kind == UI_ACTION_CLOSE)
        f->closed++;
    return true;
}

static void setup(struct fixture *f, const struct css_sheet *sheet,
                  struct css_scale_context scale)
{
    *f = (struct fixture){ .activated = -1 };
    ui_widget_init(&f->root, NULL, NULL);
    ui_button_init(&f->behind);
    ui_palette_init(&f->palette);
    ui_widget_set_action_handler(ui_palette_widget(&f->palette), on_action, f);
    assert(ui_widget_add_child(&f->root, ui_button_widget(&f->behind)));
    assert(ui_widget_add_child(&f->root, ui_palette_widget(&f->palette)));
    ui_surface_set_sheet(&f->surface, sheet);
    ui_surface_set_scale(&f->surface, scale);
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 800, .y = 600 });
    ui_surface_focus(&f->surface, ui_button_widget(&f->behind));
    assert(!ui_palette_is_open(&f->palette));
    assert(ui_palette_open(&f->palette, CMDS, N_CMDS));
    assert(ui_palette_is_open(&f->palette));
    ui_surface_prepare(&f->surface);
}

static void open_palette(struct fixture *f)
{
    setup(f, NULL, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(&f->root);
    ui_scene_destroy(&f->scene);
}

static void type(struct fixture *f, const char *text)
{
    struct ui_event e = ui_event_text(text, strlen(text), 0);
    ui_surface_dispatch(&f->surface, &e);
    ui_surface_prepare(&f->surface);
}

static bool key(struct fixture *f, enum ui_key k, unsigned mods, uint32_t ch)
{
    struct ui_event e = ui_event_key((struct ui_key_event){
        .key = k, .mods = mods, .text = ch,
    }, 0);
    bool handled = ui_surface_dispatch(&f->surface, &e);
    ui_surface_prepare(&f->surface);
    return handled;
}

static void click(struct fixture *f, struct vector2i pos)
{
    struct ui_event press = ui_event_pointer((struct pointer_event){
        .kind = PTR_PRESS, .pos = pos }, 0);
    struct ui_event release = ui_event_pointer((struct pointer_event){
        .kind = PTR_RELEASE, .pos = pos }, 0);
    ui_surface_dispatch(&f->surface, &press);
    ui_surface_dispatch(&f->surface, &release);
    ui_surface_prepare(&f->surface);
}

static const char *row_label(struct fixture *f, int index)
{
    return ui_list_item(&f->palette.list, index)->label;
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

static void filter_and_activate_test(void)
{
    struct fixture f;
    open_palette(&f);
    assert(ui_surface_focused(&f.surface) == ui_input_widget(&f.palette.input));
    assert(ui_list_count(&f.palette.list) == N_CMDS);
    type(&f, "win");
    assert(ui_list_count(&f.palette.list) == 1);
    assert(!strcmp(row_label(&f, 0), "New Window"));
    assert(key(&f, UI_KEY_ENTER, 0, 0));
    assert(f.activated == 3 && f.closed == 1);
    assert(!ui_palette_is_open(&f.palette));
    assert(ui_surface_focused(&f.surface) == ui_button_widget(&f.behind));
    teardown(&f);
}

static void backspace_widens_the_filter_test(void)
{
    struct fixture f;
    open_palette(&f);
    type(&f, "nd");
    assert(ui_list_count(&f.palette.list) == 1);
    key(&f, UI_KEY_BACKSPACE, 0, 0);
    assert(!strcmp(ui_palette_query(&f.palette), "n"));
    assert(ui_list_count(&f.palette.list) == 2);
    teardown(&f);
}

static void paste_goes_into_the_query_test(void)
{
    struct fixture f;
    open_palette(&f);
    struct ui_event e = ui_event_paste("paste", 5, 0);
    assert(ui_surface_dispatch(&f.surface, &e));
    ui_surface_prepare(&f.surface);
    assert(!strcmp(ui_palette_query(&f.palette), "paste"));
    assert(ui_list_count(&f.palette.list) == 1);
    teardown(&f);
}

static void escape_and_ctrl_c_close_test(void)
{
    struct fixture f;
    open_palette(&f);
    assert(key(&f, UI_KEY_ESCAPE, 0, 0));
    assert(!ui_palette_is_open(&f.palette));
    assert(f.activated == -1 && f.closed == 1);
    assert(ui_palette_open(&f.palette, CMDS, N_CMDS));
    ui_surface_prepare(&f.surface);
    assert(key(&f, UI_KEY_CHAR, UI_MOD_CTRL, 'c'));
    assert(!ui_palette_is_open(&f.palette) && f.closed == 2);
    teardown(&f);
}

static void keys_move_the_selection_test(void)
{
    struct fixture f;
    open_palette(&f);
    assert(ui_list_selected(&f.palette.list) == 0);
    key(&f, UI_KEY_DOWN, 0, 0);
    assert(ui_list_selected(&f.palette.list) == 1);
    key(&f, UI_KEY_TAB, UI_MOD_SHIFT, 0);
    assert(ui_list_selected(&f.palette.list) == 0);
    assert(key(&f, UI_KEY_TAB, 0, 0));
    assert(ui_list_selected(&f.palette.list) == 0);
    assert(ui_surface_focused(&f.surface) == ui_input_widget(&f.palette.input));
    key(&f, UI_KEY_PAGE_DOWN, 0, 0);
    key(&f, UI_KEY_DOWN, 0, 0);
    assert(key(&f, UI_KEY_ENTER, 0, 0));
    assert(f.activated != -1);
    teardown(&f);
}

static void palette_keys_request_frame_test(void)
{
    struct fixture f;
    open_palette(&f);
    ui_surface_take_frame_request(&f.surface);

    type(&f, "n");
    assert(ui_surface_frame_pending(&f.surface));
    ui_surface_take_frame_request(&f.surface);

    key(&f, UI_KEY_DOWN, 0, 0);
    assert(ui_surface_frame_pending(&f.surface));
    ui_surface_take_frame_request(&f.surface);

    key(&f, UI_KEY_ESCAPE, 0, 0);
    assert(!ui_palette_is_open(&f.palette));
    assert(ui_surface_frame_pending(&f.surface));
    teardown(&f);
}

static void pointer_test(void)
{
    struct fixture f;
    open_palette(&f);
    struct rect2i row = ui_list_item_rect(&f.palette.list, 1);
    click(&f, (struct vector2i){ .x = row.x + 4, .y = row.y + row.height / 2 });
    assert(f.activated == 2 && f.closed == 1);

    assert(ui_palette_open(&f.palette, CMDS, N_CMDS));
    ui_surface_prepare(&f.surface);
    struct rect2i panel = ui_palette_widget(&f.palette)->bounds;
    click(&f, (struct vector2i){ .x = panel.x + 1, .y = rect2i_end(panel).y - 1 });
    assert(ui_palette_is_open(&f.palette));
    click(&f, (struct vector2i){ .x = 2, .y = 2 });
    assert(!ui_palette_is_open(&f.palette) && f.closed == 2);
    assert(f.activated == 2);
    teardown(&f);
}

static void paints_inside_its_panel_test(void)
{
    struct fixture f;
    open_palette(&f);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    struct rect2i panel = ui_palette_widget(&f.palette)->bounds;
    bool placeholder = false;
    for (int i = 0; i < f.scene.n_ops; i++) {
        const struct ui_scene_op *op = &f.scene.ops[i];
        if (op->kind == UI_SCENE_OP_PUSH_CLIP || op->kind == UI_SCENE_OP_POP_CLIP)
            continue;
        if (!rect2i_contains(panel, op->rect.pos))
            continue;
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == 14 &&
            !memcmp(ui_scene_op_text(&f.scene, op), "Type a command", 14))
            placeholder = true;
    }
    assert(placeholder);
    teardown(&f);
}

static void follows_parent_resize_test(void)
{
    struct fixture f;
    open_palette(&f);
    struct rect2i before = ui_palette_widget(&f.palette)->bounds;
    assert(before.width == 400);
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 1200, .y = 900 });
    struct rect2i after = ui_palette_widget(&f.palette)->bounds;
    assert(after.width == 600);
    assert(after.x == (1200 - 600) / 2);
    teardown(&f);
}

static void themed_scaled_and_empty_query_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".command-palette { background: #112233; border-color: #445566; }"
        ".menu-item { padding: 4px 1ch; }"));
    struct fixture f;
    setup(&f, &sheet, (struct css_scale_context){
        .dpi_scale = 1.5f, .cell_w = 10, .cell_h = 20,
    });
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    bool themed = false;
    for (int i = 0; i < f.scene.n_ops; i++)
        themed |= f.scene.ops[i].color == 0xff112233u;
    assert(themed);
    struct ui_debug_view view = {
        .root = &f.root, .scene = &f.scene, .surface = &f.surface,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);

    type(&f, "no-such-command");
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    assert(ui_list_count(&f.palette.list) == 0);
    const struct ui_scene_op *empty = find_text(&f.scene, "No matching commands");
    assert(empty);
    struct rect2i box = ui_debug_text_box(&f.scene, empty);
    struct rect2i panel = ui_palette_widget(&f.palette)->bounds;
    assert(rect2i_contains(panel, box.pos));
    assert(rect2i_contains(panel, (struct vector2i){
        .x = rect2i_end(box).x - 1, .y = rect2i_end(box).y - 1 }));
    teardown(&f);
    css_sheet_clear(&sheet);
}

static void query_lines_up_with_labels_test(void)
{
    struct fixture f;
    open_palette(&f);
    type(&f, "Paste");
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    const struct ui_scene_op *texts[2] = { NULL, NULL };
    for (int i = 0; i < f.scene.n_ops; i++) {
        const struct ui_scene_op *op = &f.scene.ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == 5 &&
            !memcmp(ui_scene_op_text(&f.scene, op), "Paste", 5))
            texts[texts[0] ? 1 : 0] = op;
    }
    assert(texts[0] && texts[1]);
    assert(ui_debug_text_box(&f.scene, texts[0]).x ==
           ui_debug_text_box(&f.scene, texts[1]).x);
    teardown(&f);
}

int main(void)
{
    filter_and_activate_test();
    backspace_widens_the_filter_test();
    paste_goes_into_the_query_test();
    escape_and_ctrl_c_close_test();
    keys_move_the_selection_test();
    palette_keys_request_frame_test();
    pointer_test();
    paints_inside_its_panel_test();
    follows_parent_resize_test();
    themed_scaled_and_empty_query_test();
    query_lines_up_with_labels_test();
    puts("PASS: palette tests");
    return 0;
}
