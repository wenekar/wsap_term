#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ui/debug.h"
#include "ui/ui.h"

struct fixture {
    struct ui_surface surface;
    struct ui_label label, second;
    struct ui_box box;
    struct ui_scene scene;
    struct css_sheet sheet;
    char *copied;
    char opened[128];
    int opens, actions;
    bool take_action, destroy_on_action;
};

static void copy_text(void *userdata, const char *text)
{
    struct fixture *f = userdata;
    free(f->copied);
    f->copied = strdup(text);
    assert(f->copied);
}

static void open_url(void *userdata, const char *target)
{
    struct fixture *f = userdata;
    assert(strlen(target) < sizeof(f->opened));
    strcpy(f->opened, target);
    f->opens++;
}

static bool action(struct ui_widget *source, const struct ui_action *event, void *userdata)
{
    struct fixture *f = userdata;
    assert(source == ui_label_widget(&f->label));
    assert(event->kind == UI_ACTION_ACTIVATE && event->id == 42);
    f->actions++;
    if (f->destroy_on_action)
        ui_widget_destroy(source);
    return f->take_action;
}

static void init(struct fixture *f, const char *text, struct vector2i size)
{
    *f = (struct fixture){0};
    const struct ui_surface_host host = { .copy_text = copy_text, .open_url = open_url };
    ui_surface_init(&f->surface, &host, f);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    assert(ui_label_set_text(&f->label, text));
    ui_surface_set_root(&f->surface, ui_label_widget(&f->label));
    ui_surface_resize(&f->surface, size);
}

static void init_tree(struct fixture *f)
{
    *f = (struct fixture){0};
    const struct ui_surface_host host = { .copy_text = copy_text, .open_url = open_url };
    ui_surface_init(&f->surface, &host, f);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    assert(ui_label_set_text(&f->label, "Hello world"));
    assert(ui_label_set_text(&f->second, "Second line"));
    ui_label_set_selectable(&f->label, true);
    ui_label_set_selectable(&f->second, true);
    ui_box_set_direction(&f->box, UI_BOX_COLUMN);
    assert(ui_widget_add_child(ui_box_widget(&f->box), ui_label_widget(&f->label)));
    assert(ui_widget_add_child(ui_box_widget(&f->box), ui_label_widget(&f->second)));
    ui_surface_set_root(&f->surface, ui_box_widget(&f->box));
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 240, .y = 96 });
    ui_surface_prepare(&f->surface);
}

static void destroy(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    if (f->box.widget.ops)
        ui_widget_destroy(&f->box.widget);
    ui_widget_destroy(ui_label_widget(&f->label));
    if (f->second.widget.ops)
        ui_widget_destroy(&f->second.widget);
    ui_scene_destroy(&f->scene);
    css_sheet_clear(&f->sheet);
    free(f->copied);
}

static void build(struct fixture *f)
{
    assert(ui_surface_build_scene(&f->surface, &f->scene));
    const struct ui_debug_view view = {
        .root = ui_label_widget(&f->label), .surface = &f->surface, .scene = &f->scene,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
}

static int text_count(const struct fixture *f)
{
    int n = 0;
    for (int i = 0; i < f->scene.n_ops; i++)
        n += f->scene.ops[i].kind == UI_SCENE_OP_TEXT;
    return n;
}

static const struct ui_scene_op *text_op(const struct fixture *f, int index)
{
    for (int i = 0; i < f->scene.n_ops; i++)
        if (f->scene.ops[i].kind == UI_SCENE_OP_TEXT && index-- == 0)
            return &f->scene.ops[i];
    assert(false);
    return NULL;
}

static void expect_text(const struct fixture *f, int index, const char *text)
{
    const struct ui_scene_op *op = text_op(f, index);
    assert(op->text_len == strlen(text));
    assert(!memcmp(ui_scene_op_text(&f->scene, op), text, op->text_len));
}

static bool pointer(struct fixture *f, enum pointer_kind kind, struct vector2i pos,
                      unsigned mods, uint64_t time)
{
    struct ui_event event = ui_event_pointer((struct pointer_event){
        .kind = kind, .pos = pos, .mods = mods,
    }, time);
    return ui_surface_dispatch(&f->surface, &event);
}

static void click(struct fixture *f, struct vector2i pos, uint64_t time)
{
    assert(pointer(f, PTR_PRESS, pos, 0, time));
    assert(pointer(f, PTR_RELEASE, pos, 0, time + 1));
}

static bool key(struct fixture *f, enum ui_key k, uint32_t text, unsigned mods)
{
    struct ui_event event = ui_event_key((struct ui_key_event){
        .key = k, .text = text, .mods = mods,
    }, 1000);
    return ui_surface_dispatch(&f->surface, &event);
}

static void expect_selection(struct fixture *f, const char *text)
{
    size_t lo, hi;
    assert(ui_label_selection(&f->label, &lo, &hi));
    assert(hi - lo == strlen(text));
    assert(!memcmp(ui_label_text(&f->label) + lo, text, hi - lo));
    assert(ui_label_copy_selection(&f->label));
    assert(!strcmp(f->copied, text));
}

static void dynamic_storage_test(void)
{
    struct ui_label label = {0};
    assert(!strcmp(ui_label_text(&label), ""));
    size_t len = 65536;
    char *text = malloc(len + 4);
    assert(text);
    memset(text, 'a', len);
    memcpy(text + len, "中", 4);
    assert(ui_label_set_text(&label, text));
    text[0] = 'b';
    assert(strlen(ui_label_text(&label)) == len + 3);
    assert(ui_label_text(&label)[0] == 'a');
    assert(ui_label_set_text(&label, ui_label_text(&label) + len));
    assert(!strcmp(ui_label_text(&label), "中"));
    assert(ui_label_set_text(&label, ui_label_text(&label)));
    assert(ui_label_set_text(&label, NULL));
    assert(!strcmp(ui_label_text(&label), ""));
    assert(ui_label_set_link(&label, "https://example.org"));
    assert(ui_label_set_link(&label, ui_label_link(&label) + 8));
    assert(!strcmp(ui_label_link(&label), "example.org"));
    ui_widget_destroy(ui_label_widget(&label));
    struct fixture f;
    init(&f, text, (struct vector2i){ .x = 240, .y = 48 });
    ui_label_set_selectable(&f.label, true);
    ui_label_select_all(&f.label);
    assert(ui_label_copy_selection(&f.label));
    assert(!strcmp(f.copied, text));
    build(&f);
    destroy(&f);
    free(text);
}

static void wrapping_and_blank_lines_test(void)
{
    struct fixture f;
    init(&f, "alpha beta\n\n中é\r\n", (struct vector2i){ .x = 48, .y = 96 });
    build(&f);
    assert(text_count(&f) == 3);
    expect_text(&f, 0, "alpha ");
    expect_text(&f, 1, "beta");
    expect_text(&f, 2, "中é");
    assert(ui_label_widget(&f.label)->measured_size.y == 80);
    assert(text_op(&f, 2)->rect.y > text_op(&f, 1)->rect.y + 16);
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 128, .y = 96 });
    build(&f);
    assert(text_count(&f) == 2);
    expect_text(&f, 0, "alpha beta");
    destroy(&f);

    init(&f, "é中a\xCC\x81" "b", (struct vector2i){ .x = 16, .y = 64 });
    ui_label_set_wrap(&f.label, UI_LABEL_WRAP_CHAR);
    build(&f);
    expect_text(&f, 0, "é");
    expect_text(&f, 1, "中");
    expect_text(&f, 2, "a\xCC\x81" "b");
    destroy(&f);
}

static void alignment_ellipsis_and_theme_test(void)
{
    struct fixture f;
    init(&f, "A very long label", (struct vector2i){ .x = 72, .y = 32 });
    assert(css_parse(&f.sheet,
        ".label { padding: 4px 8px; cursor: none; }"
        ".label-selection { background: #112233; color: #abcdef; }"));
    ui_surface_set_sheet(&f.surface, &f.sheet);
    ui_label_set_wrap(&f.label, UI_LABEL_ELLIPSIS);
    build(&f);
    expect_text(&f, 0, "A very");
    expect_text(&f, 1, "…");
    assert(text_op(&f, 0)->rect.x >= 8);
    assert(ui_label_set_text(&f.label, "Hi"));
    ui_label_set_align(&f.label, UI_LABEL_ALIGN_CENTER);
    build(&f);
    int centered = text_op(&f, 0)->rect.x;
    ui_label_set_align(&f.label, UI_LABEL_ALIGN_RIGHT);
    build(&f);
    assert(text_op(&f, 0)->rect.x > centered);
    ui_label_set_selectable(&f.label, true);
    ui_label_select_all(&f.label);
    build(&f);
    bool highlight = false;
    for (int i = 0; i < f.scene.n_ops; i++)
        highlight |= f.scene.ops[i].kind == UI_SCENE_OP_RECT && f.scene.ops[i].color == 0xFF112233;
    assert(highlight);
    assert(text_op(&f, 0)->color == 0xFFABCDEF);
    pointer(&f, PTR_MOTION, (struct vector2i){ .x = 48, .y = 16 }, 0, 1);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_NONE);
    destroy(&f);
}

static void word_line_and_keyboard_selection_test(void)
{
    struct fixture f;
    init(&f, "Hello café world\nSecond line", (struct vector2i){ .x = 240, .y = 48 });
    ui_label_set_selectable(&f.label, true);
    build(&f);
    struct vector2i pos = { .x = 60, .y = 8 };
    click(&f, pos, 100);
    click(&f, pos, 200);
    expect_selection(&f, "café");
    build(&f);
    click(&f, pos, 300);
    expect_selection(&f, "Hello café world\n");
    assert(key(&f, UI_KEY_CHAR, 'a', UI_MOD_SUPER));
    expect_selection(&f, ui_label_text(&f.label));
    assert(key(&f, UI_KEY_CHAR, 'c', UI_MOD_CTRL));
    assert(key(&f, UI_KEY_LEFT, 0, 0));
    assert(!ui_label_selection(&f.label, NULL, NULL));
    assert(key(&f, UI_KEY_RIGHT, 0, UI_MOD_SHIFT));
    expect_selection(&f, "H");
    assert(key(&f, UI_KEY_ESCAPE, 0, 0));
    assert(!ui_label_selection(&f.label, NULL, NULL));
    assert(!ui_label_copy_selection(&f.label));
    destroy(&f);

    init(&f, "a\xCC\x81é中", (struct vector2i){ .x = 80, .y = 16 });
    ui_label_set_selectable(&f.label, true);
    click(&f, (struct vector2i){ .x = 0, .y = 8 }, 1);
    assert(key(&f, UI_KEY_RIGHT, 0, UI_MOD_SHIFT));
    expect_selection(&f, "a\xCC\x81");
    assert(key(&f, UI_KEY_RIGHT, 0, UI_MOD_SHIFT));
    expect_selection(&f, "a\xCC\x81é");
    assert(key(&f, UI_KEY_LEFT, 0, UI_MOD_SHIFT));
    expect_selection(&f, "a\xCC\x81");
    destroy(&f);
}

static void drag_across_wrapped_unicode_test(void)
{
    struct fixture f;
    init(&f, "é中\nsecond", (struct vector2i){ .x = 48, .y = 48 });
    ui_label_set_selectable(&f.label, true);
    assert(pointer(&f, PTR_PRESS, (struct vector2i){ .x = 0, .y = 8 }, 0, 1));
    assert(pointer(&f, PTR_MOTION, (struct vector2i){ .x = 200, .y = 200 }, 0, 2));
    expect_selection(&f, "é中\nsecond");
    assert(pointer(&f, PTR_RELEASE, (struct vector2i){ .x = 200, .y = 200 }, 0, 3));
    assert(!ui_surface_captured(&f.surface));
    assert(pointer(&f, PTR_PRESS, (struct vector2i){ .x = 0, .y = 8 }, 0, 1000));
    ui_surface_cancel_capture(&f.surface);
    assert(!ui_surface_captured(&f.surface));
    assert(!pointer(&f, PTR_MOTION, (struct vector2i){ .x = 40, .y = 8 }, 0, 1001));
    assert(ui_label_set_text(&f.label, "New"));
    assert(!ui_label_selection(&f.label, NULL, NULL));
    ui_label_set_selectable(&f.label, false);
    assert(!pointer(&f, PTR_PRESS, (struct vector2i){ .x = 0, .y = 8 }, 0, 2000));
    destroy(&f);
}

static void links_and_lifetime_test(void)
{
    struct fixture f;
    init(&f, "Read docs", (struct vector2i){ .x = 160, .y = 16 });
    assert(ui_label_set_link(&f.label, "https://example.org/docs"));
    ui_label_set_action_id(&f.label, 42);
    ui_widget_set_action_handler(ui_label_widget(&f.label), action, &f);
    build(&f);
    struct vector2i pos = { .x = 12, .y = 8 };
    pointer(&f, PTR_MOTION, pos, 0, 1);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_POINTER);
    assert(!pointer(&f, PTR_PRESS, (struct vector2i){ .x = 150, .y = 8 }, 0, 10));
    click(&f, pos, 100);
    assert(f.actions == 1 && f.opens == 1);
    assert(!strcmp(f.opened, "https://example.org/docs"));
    assert(key(&f, UI_KEY_ENTER, 0, 0));
    assert(f.actions == 2 && f.opens == 2);
    f.take_action = true;
    assert(key(&f, UI_KEY_CHAR, ' ', 0));
    assert(f.actions == 3 && f.opens == 2);
    ui_label_set_disabled(&f.label, true);
    assert(!pointer(&f, PTR_PRESS, pos, 0, 1000));
    pointer(&f, PTR_MOTION, pos, 0, 1001);
    assert(ui_surface_cursor(&f.surface) == UI_CURSOR_DEFAULT);
    ui_label_set_disabled(&f.label, false);
    ui_label_set_selectable(&f.label, true);
    assert(pointer(&f, PTR_PRESS, pos, 0, 2000));
    assert(pointer(&f, PTR_MOTION, (struct vector2i){ .x = 64, .y = 8 }, 0, 2001));
    assert(pointer(&f, PTR_RELEASE, (struct vector2i){ .x = 64, .y = 8 }, 0, 2002));
    assert(f.actions == 3);
    f.take_action = false;
    f.destroy_on_action = true;
    assert(key(&f, UI_KEY_ENTER, 0, 0));
    assert(f.actions == 4 && f.opens == 2);
    destroy(&f);
}

static void clip_mode_preserves_explicit_lines_test(void)
{
    struct fixture f;
    init(&f, "abcdef\nxyzuvw", (struct vector2i){ .x = 24, .y = 32 });
    ui_label_set_wrap(&f.label, UI_LABEL_CLIP);
    build(&f);
    assert(text_count(&f) == 2);
    expect_text(&f, 0, "abc");
    expect_text(&f, 1, "xyz");
    destroy(&f);
}

static void narrow_box_keeps_utf8_whole_test(void)
{
    struct fixture f;
    init(&f, "中é", (struct vector2i){ .x = 8, .y = 48 });
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    expect_text(&f, 0, "中");
    expect_text(&f, 1, "é");
    ui_label_set_wrap(&f.label, UI_LABEL_ELLIPSIS);
    build(&f);
    assert(text_count(&f) == 1);
    expect_text(&f, 0, "…");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 0, .y = 48 });
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    destroy(&f);
}

static int rect_count(const struct fixture *f, uint32_t color, int height)
{
    int n = 0;
    for (int i = 0; i < f->scene.n_ops; i++)
        n += f->scene.ops[i].kind == UI_SCENE_OP_RECT &&
             f->scene.ops[i].color == color &&
             (height == 0 || f->scene.ops[i].rect.height == height);
    return n;
}

static void link_hover_and_focus_styles_test(void)
{
    struct fixture f;
    init(&f, "Read docs", (struct vector2i){ .x = 160, .y = 32 });
    assert(css_parse(&f.sheet,
        ".label-link { color: #00ff00; border-width: 2px; }"
        ".label-link:hover { color: #0000ff; }"
        ".label-focus { border-color: #ff0000; border-width: 3px; }"));
    ui_surface_set_sheet(&f.surface, &f.sheet);
    assert(ui_label_set_link(&f.label, "https://example.org/docs"));
    build(&f);
    assert(text_op(&f, 0)->color == 0xFF00FF00);
    assert(rect_count(&f, 0xFF00FF00, 2) == 1);
    assert(rect_count(&f, 0xFFFF0000, 0) == 0);

    pointer(&f, PTR_MOTION, (struct vector2i){ .x = 12, .y = 8 }, 0, 1);
    build(&f);
    assert(text_op(&f, 0)->color == 0xFF0000FF);
    assert(rect_count(&f, 0xFF0000FF, 2) == 1);

    pointer(&f, PTR_LEAVE, (struct vector2i){ .x = 12, .y = 8 }, 0, 2);
    build(&f);
    assert(text_op(&f, 0)->color == 0xFF00FF00);

    ui_label_set_selectable(&f.label, true);
    ui_surface_focus(&f.surface, ui_label_widget(&f.label));
    build(&f);
    assert(rect_count(&f, 0xFFFF0000, 0) == 4);

    ui_label_set_disabled(&f.label, true);
    build(&f);
    assert(rect_count(&f, 0xFFFF0000, 0) == 0);
    destroy(&f);
}

static void select_word(struct fixture *f, uint64_t time)
{
    struct vector2i word = { .x = 60, .y = 8 };
    click(f, word, time);
    click(f, word, time + 100);
}

static void focus_move_clears_selection_test(void)
{
    struct fixture f;
    init_tree(&f);
    struct ui_widget *first = ui_label_widget(&f.label);
    struct ui_widget *second = ui_label_widget(&f.second);
    select_word(&f, 100);
    assert(ui_surface_focused(&f.surface) == first);
    expect_selection(&f, "world");
    free(f.copied);
    f.copied = NULL;
    assert(key(&f, UI_KEY_CHAR, 'c', UI_MOD_CTRL));
    assert(f.copied && !strcmp(f.copied, "world"));
    ui_surface_focus(&f.surface, second);
    assert(ui_surface_focused(&f.surface) == second);
    assert(!ui_label_selection(&f.label, NULL, NULL));
    assert(!ui_label_copy_selection(&f.label));
    destroy(&f);
}

static void blank_press_clears_selection_test(void)
{
    struct fixture f;
    init_tree(&f);
    struct vector2i blank = { .x = 200, .y = 80 };
    assert(!rect2i_contains(ui_label_widget(&f.label)->bounds, blank));
    assert(!rect2i_contains(ui_label_widget(&f.second)->bounds, blank));
    select_word(&f, 100);
    expect_selection(&f, "world");
    assert(ui_surface_focused(&f.surface) == ui_label_widget(&f.label));
    pointer(&f, PTR_PRESS, blank, 0, 300);
    pointer(&f, PTR_RELEASE, blank, 0, 301);
    assert(!ui_surface_focused(&f.surface));
    assert(!ui_label_selection(&f.label, NULL, NULL));
    destroy(&f);
}

static void focus_in_keeps_selection_test(void)
{
    struct fixture f;
    init_tree(&f);
    struct ui_widget *first = ui_label_widget(&f.label);
    ui_label_select_all(&f.label);
    assert(!ui_surface_focused(&f.surface));
    ui_surface_focus(&f.surface, first);
    expect_selection(&f, "Hello world");
    assert(key(&f, UI_KEY_ESCAPE, 0, 0));
    assert(!ui_label_selection(&f.label, NULL, NULL));
    destroy(&f);
}

static void press_on_own_text_keeps_selection_test(void)
{
    struct fixture f;
    init_tree(&f);
    select_word(&f, 100);
    struct vector2i start = { .x = 0, .y = 8 };
    assert(pointer(&f, PTR_PRESS, start, UI_MOD_SHIFT, 5000));
    assert(pointer(&f, PTR_RELEASE, start, UI_MOD_SHIFT, 5001));
    assert(ui_surface_focused(&f.surface) == ui_label_widget(&f.label));
    expect_selection(&f, "Hello ");
    destroy(&f);
}

static void unselectable_link_focus_test(void)
{
    struct fixture f;
    init_tree(&f);
    struct ui_widget *first = ui_label_widget(&f.label);
    ui_label_set_selectable(&f.label, false);
    assert(ui_label_set_link(&f.label, "https://example.org/docs"));
    struct vector2i text = { .x = 12, .y = 8 };
    click(&f, text, 100);
    assert(f.opens == 1 && !strcmp(f.opened, "https://example.org/docs"));
    assert(ui_surface_focused(&f.surface) == first);
    ui_surface_focus(&f.surface, ui_label_widget(&f.second));
    assert(!ui_label_selection(&f.label, NULL, NULL));
    click(&f, text, 5000);
    assert(f.opens == 2);
    assert(ui_surface_focused(&f.surface) == first);
    assert(key(&f, UI_KEY_ENTER, 0, 0));
    assert(f.opens == 3);
    destroy(&f);
}

int main(void)
{
    dynamic_storage_test();
    wrapping_and_blank_lines_test();
    alignment_ellipsis_and_theme_test();
    word_line_and_keyboard_selection_test();
    drag_across_wrapped_unicode_test();
    links_and_lifetime_test();
    clip_mode_preserves_explicit_lines_test();
    narrow_box_keeps_utf8_whole_test();
    link_hover_and_focus_styles_test();
    focus_move_clears_selection_test();
    blank_press_clears_selection_test();
    focus_in_keeps_selection_test();
    press_on_own_text_keeps_selection_test();
    unselectable_link_focus_test();
    return 0;
}
