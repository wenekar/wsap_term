/* Integrated screen test. It covers typing, focus, scrolling, resize,
 * theme changes, actions, and focus removal through public APIs. */

#include "ui/ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define N_LABELS 5

struct screen {
    struct ui_box root;
    struct ui_input input;
    struct ui_box buttons;
    struct ui_button ok;
    struct ui_button cancel;
    struct ui_scrollview scroll;
    struct ui_box content;
    struct ui_label labels[N_LABELS];
    struct ui_button more;
    int ok_clicks;
    int cancel_clicks;
    int more_clicks;
};

static bool on_action(struct ui_widget *source, const struct ui_action *action,
                      void *ud)
{
    struct screen *s = ud;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    if (source == UI_WIDGET(&s->ok))
        s->ok_clicks++;
    else if (source == UI_WIDGET(&s->cancel))
        s->cancel_clicks++;
    else if (source == UI_WIDGET(&s->more))
        s->more_clicks++;
    return false;
}

static void screen_init(struct screen *s, struct ui_surface *surface)
{
    memset(s, 0, sizeof(*s));

    ui_box_set_direction(&s->root, UI_BOX_COLUMN);
    ui_box_set_gap(&s->root, 2);

    ui_box_set_direction(&s->buttons, UI_BOX_ROW);
    ui_box_set_gap(&s->buttons, 4);
    ui_button_set_text(&s->ok, "OK");
    ui_button_set_text(&s->cancel, "Cancel");
    struct ui_widget *button_children[] = {
        UI_WIDGET(&s->ok),
        UI_WIDGET(&s->cancel),
    };
    assert(ui_widget_add_children(UI_WIDGET(&s->buttons), button_children, 2));

    ui_box_set_direction(&s->content, UI_BOX_COLUMN);
    struct ui_widget *content_children[N_LABELS + 1];
    for (int i = 0; i < N_LABELS; i++) {
        char text[32];
        snprintf(text, sizeof(text), "Row %d", i + 1);
        ui_label_set_text(&s->labels[i], text);
        content_children[i] = UI_WIDGET(&s->labels[i]);
    }
    ui_button_set_text(&s->more, "Load more");
    content_children[N_LABELS] = UI_WIDGET(&s->more);
    assert(ui_widget_add_children(UI_WIDGET(&s->content), content_children,
                                  N_LABELS + 1));

    ui_widget_set_style_identity(ui_scrollview_widget(&s->scroll), "acceptance-scroll", NULL, NULL);
    assert(ui_widget_add_child(ui_scrollview_widget(&s->scroll), UI_WIDGET(&s->content)));

    struct ui_widget *root_children[] = {
        ui_input_widget(&s->input),
        UI_WIDGET(&s->buttons),
        UI_WIDGET(&s->scroll),
    };
    assert(ui_widget_add_children(UI_WIDGET(&s->root), root_children, 3));

    ui_surface_init(surface, NULL, NULL);
    ui_widget_set_action_handler(UI_WIDGET(&s->root), on_action, s);
    ui_surface_set_root(surface, UI_WIDGET(&s->root));
    static struct css_sheet sheet;
    if (!sheet.rules) {
        css_init_default(&sheet);
        assert(css_parse(&sheet, ".acceptance-scroll { height: grow; min-height: 1px; }"));
    }
    ui_surface_set_sheet(surface, &sheet);
    ui_surface_set_scale(surface, (struct css_scale_context){
                          .dpi_scale = 1.0f, .cell_w = 8, .cell_h = 16,
                      });
    ui_surface_resize(surface, (struct vector2i){ .x = 120, .y = 80 });

}

static void screen_destroy(struct screen *s, struct ui_surface *surface)
{
    ui_surface_destroy(surface);
    ui_widget_destroy(UI_WIDGET(&s->root));
}

static void dispatch_key(struct ui_surface *surface, enum ui_key key,
                         uint32_t text)
{
    struct ui_event ev = ui_event_key((struct ui_key_event){
        .key = key, .text = text,
    }, 0);
    ui_surface_dispatch(surface, &ev);
}

static void click_at(struct ui_surface *surface, struct vector2i pos)
{
    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = pos, .button = 0,
    }, 0);
    struct ui_event release = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_RELEASE, .pos = pos, .button = 0,
    }, 1);
    ui_surface_dispatch(surface, &press);
    ui_surface_dispatch(surface, &release);
}

static struct vector2i center(const struct ui_widget *w)
{
    return (struct vector2i){
        .x = w->bounds.x + w->bounds.width / 2,
        .y = w->bounds.y + w->bounds.height / 2,
    };
}

/* Typing + tab traversal + keyboard activation. */
static void test_focus_and_typing(void)
{
    struct screen s;
    struct ui_surface surface;
    struct ui_scene scene = {0};
    screen_init(&s, &surface);
    assert(ui_surface_build_scene(&surface, &scene));

    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.input));

    dispatch_key(&surface, UI_KEY_CHAR, 'h');
    dispatch_key(&surface, UI_KEY_CHAR, 'i');
    assert(!strcmp(ui_input_text(&s.input), "hi"));
    assert(ui_input_caret(&s.input) == 2);

    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.ok));
    dispatch_key(&surface, UI_KEY_ENTER, 0);
    assert(s.ok_clicks == 1);

    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.cancel));
    dispatch_key(&surface, UI_KEY_CHAR, ' ');
    assert(s.cancel_clicks == 1);

    /* Traversal descends into the scrolled content. */
    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.more));
    dispatch_key(&surface, UI_KEY_ENTER, 0);
    assert(s.more_clicks == 1);

    /* Wraps back to the input; typing goes to it again. */
    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.input));
    dispatch_key(&surface, UI_KEY_CHAR, '!');
    assert(!strcmp(ui_input_text(&s.input), "hi!"));

    screen_destroy(&s, &surface);
    ui_scene_destroy(&scene);
}

/* Click the in-content button after wheel-scrolling; the clip keeps it
 * unreachable before scrolling. */
static void test_click_after_scroll(void)
{
    struct screen s;
    struct ui_surface surface;
    struct ui_scene scene = {0};
    screen_init(&s, &surface);
    assert(ui_surface_build_scene(&surface, &scene));

    assert(ui_scrollview_content_height(&s.scroll) >
           ui_scrollview_viewport_height(&s.scroll));

    /* Unscrolled: the button sits below the viewport — a press at its
     * (out-of-view) position must not reach it. */
    struct vector2i hidden = center(UI_WIDGET(&s.more));
    assert(!rect2i_contains(UI_WIDGET(&s.scroll)->bounds, hidden));
    click_at(&surface, hidden);
    assert(s.more_clicks == 0);

    /* Wheel-scroll through the event path: SCROLL inside the viewport
     * moves the offset, SCROLL outside it does not. */
    struct vector2i inside = {
        .x = UI_WIDGET(&s.scroll)->bounds.x + 2,
        .y = UI_WIDGET(&s.scroll)->bounds.y + 2,
    };
    struct ui_event wheel = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_SCROLL, .pos = inside, .scroll_lines = -1,
    }, 0);
    assert(ui_surface_dispatch(&surface, &wheel));
    assert(ui_scrollview_offset(&s.scroll) > 0);
    wheel.data.pointer.pos = (struct vector2i){ .x = 0, .y = 0 };
    int offset = ui_scrollview_offset(&s.scroll);
    ui_surface_dispatch(&surface, &wheel);
    assert(ui_scrollview_offset(&s.scroll) == offset);

    /* Scroll until the button is inside the viewport. The widget bounds
     * include the current offset, so restore it to get the content position. */
    int content_top = UI_WIDGET(&s.more)->bounds.y - UI_WIDGET(&s.scroll)->bounds.y +
                      ui_scrollview_offset(&s.scroll);
    ui_scrollview_scroll_to(&s.scroll, content_top);
    assert(ui_surface_build_scene(&surface, &scene));
    struct vector2i shown = center(UI_WIDGET(&s.more));
    assert(rect2i_contains(UI_WIDGET(&s.scroll)->bounds, shown));
    click_at(&surface, shown);
    assert(s.more_clicks == 1);

    screen_destroy(&s, &surface);
    ui_scene_destroy(&scene);
}

/* A smaller viewport re-lays out through the normal update path and
 * clamps the scroll offset. */
static void test_resize(void)
{
    struct screen s;
    struct ui_surface surface;
    struct ui_scene scene = {0};
    screen_init(&s, &surface);
    assert(ui_surface_build_scene(&surface, &scene));

    ui_scrollview_scroll_to(&s.scroll, ui_scrollview_content_height(&s.scroll));
    assert(ui_surface_build_scene(&surface, &scene));
    int offset_before = ui_scrollview_offset(&s.scroll);
    assert(offset_before > 0);

    ui_surface_set_scale(&surface, (struct css_scale_context){
                          .dpi_scale = 1.0f, .cell_w = 8, .cell_h = 16,
                      });
    ui_surface_resize(&surface, (struct vector2i){ .x = 60, .y = 40 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(UI_WIDGET(&s.root)->bounds.width == 60);
    assert(UI_WIDGET(&s.root)->bounds.height == 40);
    assert(UI_WIDGET(&s.scroll)->bounds.height < 40);

    /* Still functional after resize: focus and typing work. */
    assert(ui_surface_focus_next(&surface));
    dispatch_key(&surface, UI_KEY_CHAR, 'x');
    assert(!strcmp(ui_input_text(&s.input), "x"));

    /* Growing the viewport past the content height clamps the offset to
     * the new maximum during the next layout pass. */
    ui_surface_set_scale(&surface, (struct css_scale_context){
                          .dpi_scale = 1.0f, .cell_w = 8, .cell_h = 16,
                      });
    ui_surface_resize(&surface, (struct vector2i){ .x = 120, .y = 300 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(ui_scrollview_offset(&s.scroll) == 0);
    assert(ui_scrollview_content_height(&s.scroll) <=
           ui_scrollview_viewport_height(&s.scroll));

    screen_destroy(&s, &surface);
    ui_scene_destroy(&scene);
}

/* Editing the sheet in place and re-setting the same pointer must refresh
 * geometry on the next build — no forced resize. */
static void test_theme_change(void)
{
    struct screen s;
    struct ui_surface surface;
    struct ui_scene scene = {0};
    screen_init(&s, &surface);
    ui_widget_set_style_identity(UI_WIDGET(&s.ok), "ok-button", NULL, NULL);

    struct css_sheet sheet = {0};
    css_parse(&sheet, ".ok-button { padding: 2px; }");
    ui_surface_set_sheet(&surface, &sheet);
    assert(ui_surface_build_scene(&surface, &scene));
    int w_before = UI_WIDGET(&s.ok)->bounds.width;

    css_sheet_clear(&sheet);
    css_parse(&sheet, ".ok-button { padding: 12px; }");
    ui_surface_set_sheet(&surface, &sheet);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(UI_WIDGET(&s.ok)->bounds.width > w_before);

    screen_destroy(&s, &surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

/* Removing the focused widget clears focus, delivers FOCUS_OUT, and stops
 * key delivery — no stale references remain. */
static void test_remove_focused_control(void)
{
    struct screen s;
    struct ui_surface surface;
    struct ui_scene scene = {0};
    screen_init(&s, &surface);
    assert(ui_surface_build_scene(&surface, &scene));

    ui_surface_focus(&surface, UI_WIDGET(&s.ok));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.ok));

    assert(ui_widget_remove_child(UI_WIDGET(&s.buttons), UI_WIDGET(&s.ok)));
    assert(ui_surface_focused(&surface) == NULL);
    assert(ui_widget_surface(UI_WIDGET(&s.ok)) == NULL);

    /* Keys no longer reach the removed control; traversal skips it. */
    dispatch_key(&surface, UI_KEY_ENTER, 0);
    assert(s.ok_clicks == 0);
    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.input));
    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.cancel));

    /* The detached widget is still valid storage and can reattach. */
    assert(ui_widget_add_child(UI_WIDGET(&s.buttons), UI_WIDGET(&s.ok)));
    assert(ui_surface_build_scene(&surface, &scene));

    screen_destroy(&s, &surface);
    ui_scene_destroy(&scene);
}

static void test_click_moves_keyboard_focus(void)
{
    struct screen s;
    struct ui_surface surface;
    struct ui_scene scene = {0};
    screen_init(&s, &surface);
    struct ui_input second;
    ui_input_init(&second);
    assert(ui_widget_add_child(UI_WIDGET(&s.root), UI_WIDGET(&second)));
    ui_surface_set_scale(&surface, (struct css_scale_context){0});
    ui_surface_resize(&surface, (struct vector2i){ .x = 300, .y = 400 });
    assert(ui_surface_build_scene(&surface, &scene));
    click_at(&surface, center(UI_WIDGET(&s.input)));
    dispatch_key(&surface, UI_KEY_CHAR, 'a');
    click_at(&surface, center(UI_WIDGET(&second)));
    dispatch_key(&surface, UI_KEY_CHAR, 'b');
    assert(!strcmp(ui_input_text(&s.input), "a"));
    assert(!strcmp(ui_input_text(&second), "b"));
    click_at(&surface, center(UI_WIDGET(&s.ok)));
    dispatch_key(&surface, UI_KEY_ENTER, 0);
    assert(s.ok_clicks == 2);
    assert(ui_surface_focused(&surface) == UI_WIDGET(&s.ok));
    screen_destroy(&s, &surface);
    ui_scene_destroy(&scene);
}

static bool scene_has_color(const struct ui_scene *scene, uint32_t color)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].color == color)
            return true;
    return false;
}

static void test_input_surface_theme_and_scale(void)
{
    struct ui_box box = {0};
    struct ui_input input;
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    struct css_sheet sheet = {0};
    ui_input_init(&input);
    ui_box_init(&box);
    ui_box_set_direction(&box, UI_BOX_COLUMN);
    assert(ui_widget_add_child(UI_WIDGET(&box), UI_WIDGET(&input)));
    ui_input_set_text(&input, "hello");
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, UI_WIDGET(&box));
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(&surface, (struct vector2i){ .x = 800, .y = 600 });
    assert(ui_surface_build_scene(&surface, &scene));
    int original_height = UI_WIDGET(&input)->bounds.height;
    assert(css_parse(&sheet,
        ".text-input { background: #ff0000; padding: 12px; }"
        ".text-input-text { color: #00ff00; }"));
    ui_surface_set_sheet(&surface, &sheet);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_color(&scene, 0xffff0000u));
    assert(scene_has_color(&scene, 0xff00ff00u));
    assert(UI_WIDGET(&input)->bounds.height > original_height);
    int themed_height = UI_WIDGET(&input)->bounds.height;
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(UI_WIDGET(&input)->bounds.height == 2 * themed_height);
    bool scaled_text = false;
    for (int i = 0; i < scene.n_ops; i++)
        if (scene.ops[i].kind == UI_SCENE_OP_TEXT)
            scaled_text |= scene.ops[i].style.advance == 16 &&
                           scene.ops[i].style.cell_h == 32;
    assert(scaled_text);
    ui_surface_set_sheet(&surface, NULL);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(!scene_has_color(&scene, 0xffff0000u));
    assert(UI_WIDGET(&input)->bounds.height == 2 * original_height);
    ui_surface_destroy(&surface);
    ui_widget_destroy(UI_WIDGET(&box));
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

int main(void)
{
    test_click_moves_keyboard_focus();
    test_input_surface_theme_and_scale();
    test_focus_and_typing();
    test_click_after_scroll();
    test_resize();
    test_theme_change();
    test_remove_focused_control();
    return 0;
}
