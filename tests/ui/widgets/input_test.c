#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint/scene.h"
#include "ui/ui.h"

struct fixture {
    struct ui_surface surface;
    struct ui_input in;
    struct ui_scene scene;
    int changes;
    int menus;
    struct vector2i menu_at;
};

static bool on_change(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    if (action->kind == UI_ACTION_CHANGE)
        f->changes++;
    if (action->kind == UI_ACTION_MENU) {
        f->menus++;
        f->menu_at = action->pos;
    }
    return false;
}

static void setup_scaled(struct fixture *f, const char *text,
                         struct css_scale_context scale)
{
    *f = (struct fixture){0};
    ui_input_init(&f->in);
    ui_input_set_text(&f->in, text);
    ui_widget_set_action_handler(ui_input_widget(&f->in), on_change, f);
    ui_surface_set_scale(&f->surface, scale);
    ui_surface_set_root(&f->surface, ui_input_widget(&f->in));
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 200, .y = 24 });
    ui_surface_focus(&f->surface, ui_input_widget(&f->in));
    f->changes = 0;
}

static void setup(struct fixture *f, const char *text)
{
    setup_scaled(f, text, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(ui_input_widget(&f->in));
    ui_scene_destroy(&f->scene);
}

static bool key(struct fixture *f, enum ui_key k, unsigned mods)
{
    struct ui_event e = ui_event_key((struct ui_key_event){
        .key = k, .mods = mods,
        .edit_unit = ui_edit_unit_for_mods(mods, UI_EDIT_BINDINGS_DEFAULT),
    }, 0);
    return ui_surface_dispatch(&f->surface, &e);
}

static bool ch(struct fixture *f, uint32_t cp, unsigned mods)
{
    struct ui_event e = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = cp, .mods = mods,
    }, 0);
    return ui_surface_dispatch(&f->surface, &e);
}

static bool pointer(struct fixture *f, enum ui_pointer_kind kind, int x,
                    int button)
{
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = { .x = x, .y = 12 }, .button = button,
    }, 0);
    return ui_surface_dispatch(&f->surface, &e);
}

static const char *text(const struct fixture *f)
{
    return ui_input_text(&f->in);
}

static bool scene_has_text(const struct ui_scene *s, const char *t)
{
    for (int i = 0; i < s->n_ops; i++) {
        const struct ui_scene_op *op = &s->ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == strlen(t) &&
            !memcmp(ui_scene_op_text(s, op), t, op->text_len))
            return true;
    }
    return false;
}

static void insert_at_caret_test(void)
{
    struct fixture f;
    setup(&f, "");
    assert(ch(&f, 'h', 0) && ch(&f, 'i', 0));
    assert(!strcmp(text(&f), "hi") && ui_input_caret(&f.in) == 2);
    assert(f.changes == 2);
    assert(key(&f, UI_KEY_LEFT, 0));
    assert(ch(&f, 'e', 0));
    assert(!strcmp(text(&f), "hei") && ui_input_caret(&f.in) == 2);
    teardown(&f);
}

static void backspace_delete_test(void)
{
    struct fixture f;
    setup(&f, "abc");
    assert(key(&f, UI_KEY_BACKSPACE, 0));
    assert(!strcmp(text(&f), "ab"));
    assert(key(&f, UI_KEY_HOME, 0));
    assert(key(&f, UI_KEY_DELETE, 0));
    assert(!strcmp(text(&f), "b") && ui_input_caret(&f.in) == 0);
    teardown(&f);
}

static void selection_replace_test(void)
{
    struct fixture f;
    setup(&f, "hello");
    key(&f, UI_KEY_LEFT, UI_MOD_SHIFT);
    key(&f, UI_KEY_LEFT, UI_MOD_SHIFT);
    int s, e;
    assert(ui_input_selection(&f.in, &s, &e) && s == 3 && e == 5);
    assert(ch(&f, 'y', 0));
    assert(!strcmp(text(&f), "hely"));
    assert(!ui_input_selection(&f.in, &s, &e));
    assert(ui_input_caret(&f.in) == 4);
    teardown(&f);
}

static void select_all_type_test(void)
{
    struct fixture f;
    setup(&f, "old");
    assert(ch(&f, 'a', UI_MOD_CTRL));
    int s, e;
    assert(ui_input_selection(&f.in, &s, &e));
    assert(s == 0 && e == 3);
    assert(ch(&f, 'x', 0));
    assert(!strcmp(text(&f), "x"));
    teardown(&f);
}

static void shift_extend_test(void)
{
    struct fixture f;
    setup(&f, "abcd");
    key(&f, UI_KEY_HOME, 0);
    key(&f, UI_KEY_RIGHT, UI_MOD_SHIFT);
    key(&f, UI_KEY_RIGHT, UI_MOD_SHIFT);
    int s, e;
    assert(ui_input_selection(&f.in, &s, &e) && s == 0 && e == 2);
    key(&f, UI_KEY_RIGHT, 0);
    assert(!ui_input_selection(&f.in, &s, &e));
    assert(ui_input_caret(&f.in) == 2);
    key(&f, UI_KEY_RIGHT, 0);
    assert(ui_input_caret(&f.in) == 3);
    teardown(&f);
}

static void word_jump_test(void)
{
    struct fixture f;
    setup(&f, "foo  bar_baz qux");
    key(&f, UI_KEY_HOME, 0);
    key(&f, UI_KEY_RIGHT, UI_MOD_CTRL);
    assert(ui_input_caret(&f.in) == 5);
    key(&f, UI_KEY_RIGHT, UI_MOD_CTRL);
    assert(ui_input_caret(&f.in) == 13);
    key(&f, UI_KEY_RIGHT, UI_MOD_CTRL);
    assert(ui_input_caret(&f.in) == (int)strlen("foo  bar_baz qux"));
    key(&f, UI_KEY_LEFT, UI_MOD_CTRL);
    assert(ui_input_caret(&f.in) == 13);
    key(&f, UI_KEY_LEFT, UI_MOD_CTRL);
    assert(ui_input_caret(&f.in) == 5);
    teardown(&f);
}

static void word_delete_test(void)
{
    struct fixture f;
    setup(&f, "foo bar");
    key(&f, UI_KEY_BACKSPACE, UI_MOD_CTRL);
    assert(!strcmp(text(&f), "foo "));
    key(&f, UI_KEY_BACKSPACE, UI_MOD_CTRL);
    assert(!strcmp(text(&f), ""));
    ui_input_set_text(&f.in, "  foo bar");
    key(&f, UI_KEY_HOME, 0);
    key(&f, UI_KEY_DELETE, UI_MOD_CTRL);
    assert(!strcmp(text(&f), "foo bar"));
    teardown(&f);
}

static void utf8_caret_test(void)
{
    struct fixture f;
    setup(&f, "a\xc3\xa9\xe4\xb8\xad");
    assert(ui_input_caret(&f.in) == 6);
    key(&f, UI_KEY_LEFT, 0);
    assert(ui_input_caret(&f.in) == 3);
    key(&f, UI_KEY_LEFT, 0);
    assert(ui_input_caret(&f.in) == 1);
    key(&f, UI_KEY_BACKSPACE, 0);
    assert(!strcmp(text(&f), "\xc3\xa9\xe4\xb8\xad"));
    assert(ch(&f, 0x00e9, 0));
    assert(!strcmp(text(&f), "\xc3\xa9\xc3\xa9\xe4\xb8\xad"));
    assert(ui_input_caret(&f.in) == 2);
    teardown(&f);
}

static void text_and_paste_insert_clean_text_test(void)
{
    struct fixture f;
    setup(&f, "ab");
    key(&f, UI_KEY_LEFT, 0);
    struct ui_event e = ui_event_text("X\nY\tZ", 5, 0);
    assert(ui_surface_dispatch(&f.surface, &e));
    assert(!strcmp(text(&f), "aXYZb") && ui_input_caret(&f.in) == 4);
    e = ui_event_paste("\x01\x02", 2, 0);
    assert(!ui_surface_dispatch(&f.surface, &e));
    e = ui_event_paste("12", 2, 0);
    assert(ui_surface_dispatch(&f.surface, &e));
    assert(!strcmp(text(&f), "aXYZ12b"));
    teardown(&f);
}

static void placeholder_test(void)
{
    struct fixture f;
    setup(&f, "");
    ui_input_set_placeholder(&f.in, "Search...");
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(scene_has_text(&f.scene, "Search..."));
    ch(&f, 'q', 0);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(!scene_has_text(&f.scene, "Search..."));
    teardown(&f);
}

static void blink_test(void)
{
    struct fixture f;
    setup(&f, "x");
    ui_surface_tick(&f.surface, 1000);
    assert(ui_surface_next_deadline(&f.surface) == 1500);
    ui_surface_tick(&f.surface, 1200);
    assert(ui_surface_next_deadline(&f.surface) == 1500);
    ui_surface_tick(&f.surface, 1600);
    assert(ui_surface_next_deadline(&f.surface) == 2000);
    ui_surface_focus(&f.surface, NULL);
    ui_surface_tick(&f.surface, 1700);
    assert(ui_surface_next_deadline(&f.surface) == 0);
    teardown(&f);
}

static bool scene_has_caret(const struct ui_scene *s)
{
    for (int i = 0; i < s->n_ops; i++) {
        const struct ui_scene_op *op = &s->ops[i];
        if (op->kind == UI_SCENE_OP_RECT && op->rect.width <= 3 &&
            op->rect.height >= 8)
            return true;
    }
    return false;
}

static void caret_shows_only_with_focus_test(void)
{
    struct fixture f;
    setup(&f, "edit me");
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(scene_has_text(&f.scene, "edit me") && scene_has_caret(&f.scene));
    ui_surface_focus(&f.surface, NULL);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(!scene_has_caret(&f.scene));
    teardown(&f);
}

static const char PART_SHEET_A[] =
    ".text-input-text { color: #112233; }"
    ".text-input-text.empty { color: #334455; }"
    ".text-input-selection { background: #556677; }"
    ".text-input-caret { background: #8899aa; }";

static const char PART_SHEET_B[] =
    ".text-input-text { color: #aabbcc; }"
    ".text-input-text.empty { color: #ddeeff; }"
    ".text-input-selection { background: #102030; }"
    ".text-input-caret { background: #405060; }";

static bool scene_has_rect_color(const struct ui_scene *s, uint32_t color)
{
    for (int i = 0; i < s->n_ops; i++)
        if (s->ops[i].kind == UI_SCENE_OP_RECT && s->ops[i].color == color)
            return true;
    return false;
}

static bool scene_has_text_color(const struct ui_scene *s, const char *t,
                                 uint32_t color)
{
    for (int i = 0; i < s->n_ops; i++) {
        const struct ui_scene_op *op = &s->ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == strlen(t) &&
            !memcmp(ui_scene_op_text(s, op), t, op->text_len) &&
            op->color == color)
            return true;
    }
    return false;
}

static void part_colors_test(void)
{
    struct css_sheet sheet_a = {0};
    struct css_sheet sheet_b = {0};
    assert(css_parse(&sheet_a, PART_SHEET_A));
    assert(css_parse(&sheet_b, PART_SHEET_B));

    struct fixture f;
    setup(&f, "edit");
    ui_input_set_placeholder(&f.in, "Search");
    ui_surface_set_sheet(&f.surface, &sheet_a);
    ui_input_select_all(&f.in);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(scene_has_text_color(&f.scene, "edit", 0xFF112233));
    assert(scene_has_rect_color(&f.scene, 0xFF556677));
    assert(scene_has_rect_color(&f.scene, 0xFF8899AA));

    ui_input_clear(&f.in);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(scene_has_text_color(&f.scene, "Search", 0xFF334455));
    assert(!scene_has_rect_color(&f.scene, 0xFF556677));

    ui_surface_set_sheet(&f.surface, &sheet_b);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(scene_has_text_color(&f.scene, "Search", 0xFFDDEEFF));
    assert(scene_has_rect_color(&f.scene, 0xFF405060));

    ui_input_set_text(&f.in, "edit");
    ui_input_select_all(&f.in);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(scene_has_text_color(&f.scene, "edit", 0xFFAABBCC));
    assert(scene_has_rect_color(&f.scene, 0xFF102030));
    teardown(&f);
    css_sheet_clear(&sheet_a);
    css_sheet_clear(&sheet_b);
}

static void pointer_test(void)
{
    struct fixture f;
    setup_scaled(&f, "abcd", (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 10, .cell_h = 20,
    });
    int inset = ui_input_text_inset(&f.in);
    assert(pointer(&f, UI_POINTER_PRESS, inset + 20, 0));
    assert(ui_input_caret(&f.in) == 2);
    assert(pointer(&f, UI_POINTER_MOTION, inset - 5, 0));
    int s, e;
    assert(ui_input_selection(&f.in, &s, &e) && s == 0 && e == 2);
    assert(pointer(&f, UI_POINTER_RELEASE, inset - 5, 0));
    int caret = ui_input_caret(&f.in);
    assert(!pointer(&f, UI_POINTER_MOTION, inset + 30, 0));
    assert(ui_input_caret(&f.in) == caret);
    pointer(&f, UI_POINTER_PRESS, inset + 10, 2);
    assert(ui_input_caret(&f.in) == caret);
    assert(f.menus == 1 && f.menu_at.x == inset + 10);
    teardown(&f);
}

static void keys_for_the_parent_pass_through_test(void)
{
    struct fixture f;
    setup(&f, "x");
    assert(!key(&f, UI_KEY_ENTER, 0));
    assert(!key(&f, UI_KEY_ESCAPE, 0));
    assert(!key(&f, UI_KEY_UP, 0));
    assert(!key(&f, UI_KEY_PAGE_DOWN, 0));
    assert(!ch(&f, 'c', UI_MOD_CTRL));
    assert(!strcmp(text(&f), "x"));
    teardown(&f);
}

static void attached_layout_is_stable_test(void)
{
    struct ui_box box;
    struct ui_input in;
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_box_init(&box);
    ui_box_set_padding(&box, (struct insets2i){ .top = 30, .left = 40 });
    ui_input_init(&in);
    ui_input_set_text(&in, "abcd");
    assert(ui_widget_add_child(ui_box_widget(&box), ui_input_widget(&in)));
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 10, .cell_h = 20 });
    ui_surface_set_root(&surface, ui_box_widget(&box));
    ui_surface_resize(&surface, (struct vector2i){ .x = 400, .y = 100 });
    assert(ui_surface_build_scene(&surface, &scene));
    (void)ui_surface_take_frame_request(&surface);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(!ui_surface_frame_pending(&surface));

    struct ui_widget *widget = ui_input_widget(&in);
    assert(widget->bounds.x == 40 && widget->bounds.y == 30);
    bool inside = false;
    for (int i = 0; i < scene.n_ops; i++)
        inside |= scene.ops[i].kind == UI_SCENE_OP_TEXT &&
                  rect2i_contains(widget->bounds, scene.ops[i].rect.pos);
    assert(inside);

    int x = 40 + ui_input_text_inset(&in) + 30;
    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = x, .y = 40 } }, 1);
    assert(ui_surface_dispatch(&surface, &press));
    assert(ui_input_caret(&in) == 3);
    press.data.pointer.pos = (struct vector2i){ .x = 36, .y = 10 };
    assert(!ui_surface_dispatch(&surface, &press));

    ui_surface_destroy(&surface);
    ui_widget_destroy(ui_box_widget(&box));
    ui_scene_destroy(&scene);
}

static void assert_text_at_start(struct fixture *f)
{
    assert(ui_surface_build_scene(&f->surface, &f->scene));
    for (int i = 0; i < f->scene.n_ops; i++) {
        const struct ui_scene_op *op = &f->scene.ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == strlen(text(f)) &&
            !memcmp(ui_scene_op_text(&f->scene, op), text(f), op->text_len)) {
            assert(op->rect.x == f->in.widget.bounds.x + ui_input_text_inset(&f->in));
            return;
        }
    }
    assert(false);
}

static void text_fits_after_layout_test(void)
{
    struct fixture f;
    setup(&f, "FiraCode Nerd Font Mono");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 24 });
    assert_text_at_start(&f);
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32,
    });
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 600, .y = 48 });
    assert_text_at_start(&f);
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 120, .y = 48 });
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 600, .y = 48 });
    assert_text_at_start(&f);
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 120, .y = 48 });
    ui_input_set_text(&f.in, "Hi");
    assert_text_at_start(&f);
    teardown(&f);
}

static void text_cursor_test(void)
{
    struct fixture f;
    setup(&f, "Text");
    pointer(&f, UI_POINTER_MOTION, 40, 0);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_TEXT);
    pointer(&f, UI_POINTER_MOTION, 220, 0);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_DEFAULT);
    teardown(&f);
}

static bool mac_key(struct fixture *f, enum ui_key key, unsigned mods)
{
    struct ui_event event = ui_event_key((struct ui_key_event){
        .key = key, .mods = mods,
        .edit_unit = ui_edit_unit_for_mods(mods, UI_EDIT_BINDINGS_MACOS),
    }, 0);
    return ui_surface_dispatch(&f->surface, &event);
}

static void mac_editing_test(void)
{
    struct fixture f;
    setup(&f, "foo  bar baz");
    key(&f, UI_KEY_HOME, 0);
    assert(mac_key(&f, UI_KEY_RIGHT, UI_MOD_ALT));
    assert(ui_input_caret(&f.in) == 3);
    assert(mac_key(&f, UI_KEY_RIGHT, UI_MOD_ALT | UI_MOD_SHIFT));
    int start, end;
    assert(ui_input_selection(&f.in, &start, &end) && start == 3 && end == 8);
    assert(mac_key(&f, UI_KEY_RIGHT, UI_MOD_SUPER));
    assert(ui_input_caret(&f.in) == 12);
    assert(!ui_input_selection(&f.in, NULL, NULL));
    assert(mac_key(&f, UI_KEY_LEFT, UI_MOD_SUPER | UI_MOD_SHIFT));
    assert(ui_input_selection(&f.in, &start, &end) && start == 0 && end == 12);
    assert(mac_key(&f, UI_KEY_RIGHT, UI_MOD_SUPER));
    assert(mac_key(&f, UI_KEY_BACKSPACE, UI_MOD_ALT));
    assert(!strcmp(text(&f), "foo  bar "));
    assert(mac_key(&f, UI_KEY_BACKSPACE, UI_MOD_SUPER));
    assert(!strcmp(text(&f), ""));
    ui_input_set_text(&f.in, "foo  bar baz");
    assert(mac_key(&f, UI_KEY_LEFT, UI_MOD_ALT));
    assert(ui_input_caret(&f.in) == 9);
    assert(mac_key(&f, UI_KEY_DELETE, UI_MOD_SUPER));
    assert(!strcmp(text(&f), "foo  bar "));
    key(&f, UI_KEY_HOME, 0);
    assert(mac_key(&f, UI_KEY_DELETE, UI_MOD_ALT));
    assert(!strcmp(text(&f), "  bar "));
    teardown(&f);
}

static void keep_copied(void *userdata, const char *text)
{
    char **copied = userdata;
    free(*copied);
    *copied = strdup(text);
}

static void copy_shortcut_test(void)
{
    char *copied = NULL;
    const struct ui_surface_host host = { .copy_text = keep_copied };
    struct ui_surface surface;
    ui_surface_init(&surface, &host, &copied);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    struct ui_input in;
    ui_input_init(&in);
    ui_input_set_text(&in, "hello world");
    ui_surface_set_root(&surface, ui_input_widget(&in));
    ui_surface_resize(&surface, (struct vector2i){ .x = 200, .y = 24 });
    ui_surface_focus(&surface, ui_input_widget(&in));

    for (int i = 0; i < 5; i++) {
        struct ui_event left = ui_event_key((struct ui_key_event){
            .key = UI_KEY_LEFT, .mods = UI_MOD_SHIFT,
        }, 0);
        assert(ui_surface_dispatch(&surface, &left));
    }
    struct ui_event copy = ui_event_shortcut(0, UI_EDIT_COPY, 0);
    assert(ui_surface_dispatch(&surface, &copy));
    assert(copied && !strcmp(copied, "world"));
    assert(!strcmp(ui_input_text(&in), "hello world"));

    ui_surface_destroy(&surface);
    ui_widget_destroy(ui_input_widget(&in));
    free(copied);
}

static void edit_requests_frame_test(void)
{
    struct fixture f;
    setup(&f, "");
    ui_surface_take_frame_request(&f.surface);
    assert(ch(&f, 'h', 0));
    assert(ui_surface_frame_pending(&f.surface));

    ui_surface_take_frame_request(&f.surface);
    assert(key(&f, UI_KEY_LEFT, 0));
    assert(ui_surface_frame_pending(&f.surface));

    ui_surface_take_frame_request(&f.surface);
    assert(ch(&f, 'a', UI_MOD_CTRL));
    assert(ui_surface_frame_pending(&f.surface));
    teardown(&f);
}

int main(void)
{
    edit_requests_frame_test();
    copy_shortcut_test();
    mac_editing_test();
    text_cursor_test();
    text_fits_after_layout_test();
    insert_at_caret_test();
    backspace_delete_test();
    selection_replace_test();
    select_all_type_test();
    shift_extend_test();
    word_jump_test();
    word_delete_test();
    utf8_caret_test();
    text_and_paste_insert_clean_text_test();
    placeholder_test();
    blink_test();
    caret_shows_only_with_focus_test();
    part_colors_test();
    pointer_test();
    keys_for_the_parent_pass_through_test();
    attached_layout_is_stable_test();
    puts("PASS: input tests");
    return 0;
}
