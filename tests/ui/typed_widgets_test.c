#include <assert.h>
#include <string.h>

#include "ui/ui.h"
#include "ui/internal/tree.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "ui/widgets/button.h"
#include "ui/widgets/label.h"
#include "ui/box.h"

/* A box with the class the panel tests style. */
static void panel_init(struct ui_box *panel)
{
    ui_box_init(panel);
    ui_widget_set_style_identity(&panel->widget, "panel", NULL, "panel");
}

/* A labelled button under a box, styled by css, in a surface. */
static void button_in_surface(struct css_sheet *sheet, const char *css,
                              struct ui_surface *surface, struct ui_box *box,
                              struct ui_button *button)
{
    assert(css_parse(sheet, css));
    ui_box_init(box);
    ui_button_init(button);
    ui_button_set_text(button, "OK");
    assert(ui_widget_add_child(&box->widget, &button->widget));
    ui_surface_init(surface, NULL, NULL);
    ui_surface_set_sheet(surface, sheet);
    ui_surface_set_root(surface, &box->widget);
}

/* Sets the test cell metrics and sizes the surface to 200 by 60. */
static void size_200x60(struct ui_surface *surface)
{
    ui_surface_set_scale(surface, (struct css_scale_context){ .dpi_scale = 1.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    ui_surface_resize(surface, (struct vector2i){ .x = 200, .y = 60 });
}

static bool activated(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    (void)action;
    int *count = userdata;
    (*count)++;
    return false;
}

static void panel_measure_and_label_test(void)
{
    struct ui_box panel = {0};
    struct ui_label label = {0};
    panel_init(&panel);
    ui_label_init(&label);
    ui_label_set_text(&label, "Settings");
    ui_widget_set_fixed_size(&label.widget, (struct vector2i){ .x = 64, .y = 20 });
    assert(ui_widget_add_child(ui_box_widget(&panel), &label.widget));

    struct ui_constraints constraints = {
        .min = { .x = 0, .y = 0 },
        .max = { .x = 200, .y = 100 },
    };
    assert(vector2i_eq(ui_widget_measure(ui_box_widget(&panel), constraints),
                       (struct vector2i){ .x = 64, .y = 20 }));
    ui_widget_layout(ui_box_widget(&panel), (struct rect2i){
        .pos = { .x = 8, .y = 4 }, .size = { .x = 100, .y = 40 },
    });
    assert(label.widget.bounds.x == 8 && label.widget.bounds.y == 4);
    assert(strcmp(ui_label_text(&label), "Settings") == 0);
    ui_widget_destroy(ui_box_widget(&panel));
}

static void label_paint_produces_text_op_test(void)
{
    struct ui_label label = {0};
    ui_label_init(&label);
    ui_label_set_text(&label, "Hi");
    ui_widget_set_fixed_size(&label.widget, (struct vector2i){ .x = 32, .y = 16 });
    ui_widget_layout(&label.widget, (struct rect2i){
        .pos = { .x = 4, .y = 2 }, .size = { .x = 32, .y = 16 },
    });

    struct ui_scene scene = {0};
    assert(ui_widget_build_scene(&label.widget, &scene));

    bool found = false;
    for (int i = 0; i < scene.n_ops; i++) {
        const struct ui_scene_op *op = &scene.ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == 2 &&
            !memcmp(ui_scene_op_text(&scene, op), "Hi", 2))
            found = true;
    }
    assert(found);
    ui_widget_destroy(&label.widget);
    ui_scene_destroy(&scene);
}

static void label_empty_produces_no_text_op_test(void)
{
    struct ui_label label = {0};
    ui_label_init(&label);
    ui_widget_layout(&label.widget, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 0, .y = 0 },
    });

    struct ui_scene scene = {0};
    assert(ui_widget_build_scene(&label.widget, &scene));
    for (int i = 0; i < scene.n_ops; i++)
        assert(scene.ops[i].kind != UI_SCENE_OP_TEXT);
    ui_widget_destroy(&label.widget);

    struct ui_label raw = {0};
    struct ui_scene raw_scene = {0};
    assert(ui_widget_build_scene(&raw.widget, &raw_scene));
    assert(raw_scene.n_ops == 0);
    ui_scene_destroy(&raw_scene);
    ui_scene_destroy(&scene);
}

static void button_activation_and_disabled_test(void)
{
    struct ui_surface context = {0};
    struct ui_box panel = {0};
    struct ui_button button = {0};
    int count = 0;
    panel_init(&panel);
    ui_button_init(&button);
    ui_button_set_text(&button, "Apply");
    ui_widget_set_fixed_size(&button.widget, (struct vector2i){ .x = 70, .y = 24 });
    assert(ui_widget_add_child(ui_box_widget(&panel), &button.widget));
    ui_surface_set_root(&context, ui_box_widget(&panel));
    ui_widget_set_action_handler(&button.widget, activated, &count);
    ui_widget_layout(&button.widget, (struct rect2i){
        .pos = { .x = 10, .y = 5 }, .size = { .x = 70, .y = 24 },
    });

    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 20, .y = 10 }, .button = 0,
    }, 1);
    struct ui_event release = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_RELEASE, .pos = { .x = 20, .y = 10 }, .button = 0,
    }, 2);
    assert(ui_widget_event(&button.widget, &press));
    assert(ui_button_is_pressed(&button));
    assert(ui_widget_event(&button.widget, &release));
    assert(count == 1 && !ui_button_is_pressed(&button));

    ui_button_set_disabled(&button, true);
    assert(!ui_widget_event(&button.widget, &press));
    assert(!ui_button_is_pressed(&button));
    assert(count == 1);
    ui_widget_destroy(ui_box_widget(&panel));
    ui_surface_destroy(&context);
}

static void context_requests_state_redraw_test(void)
{
    struct ui_surface context = {0};
    struct ui_button button = {0};
    ui_button_init(&button);
    ui_widget_layout(&button.widget, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 80, .y = 24 },
    });
    ui_surface_set_root(&context, &button.widget);
    ui_surface_resize(&context, (struct vector2i){ .x = 80, .y = 24 });
    assert(ui_surface_take_frame_request(&context));

    struct ui_event motion = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = { .x = 10, .y = 10 },
    }, 1);
    assert(!ui_surface_dispatch(&context, &motion));
    assert(button.widget.impl.style_pseudo & CSS_PSEUDO_HOVER);
    assert(ui_surface_frame_pending(&context));
    assert(button.widget.impl.invalidation & UI_WIDGET_INVALIDATE_PAINT);
    ui_surface_destroy(&context);
}

static void explicit_focus_targets_one_control_test(void)
{
    struct ui_surface context = {0};
    struct ui_box panel = {0};
    struct ui_button first = {0};
    struct ui_button second = {0};
    panel_init(&panel);
    ui_button_init(&first);
    ui_button_init(&second);
    assert(ui_widget_add_child(ui_box_widget(&panel), &first.widget));
    assert(ui_widget_add_child(ui_box_widget(&panel), &second.widget));
    ui_surface_set_root(&context, ui_box_widget(&panel));
    ui_surface_take_frame_request(&context);

    ui_surface_focus(&context, &first.widget);
    assert(ui_widget_has_focus(&first.widget));
    assert(!ui_widget_has_focus(&second.widget));
    assert(ui_surface_frame_pending(&context));
    ui_widget_destroy(ui_box_widget(&panel));
    ui_surface_destroy(&context);
}

static void mismatched_release_preserves_press_test(void)
{
    struct ui_surface context = {0};
    struct ui_box panel = {0};
    struct ui_button button = {0};
    int count = 0;
    panel_init(&panel);
    ui_button_init(&button);
    assert(ui_widget_add_child(ui_box_widget(&panel), &button.widget));
    ui_surface_set_root(&context, ui_box_widget(&panel));
    ui_widget_set_action_handler(&button.widget, activated, &count);
    ui_widget_layout(&button.widget, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 80, .y = 24 },
    });
    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .button = 0, .pos = { .x = 5, .y = 5 },
    }, 1);
    struct ui_event wrong_release = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_RELEASE, .button = 1, .pos = { .x = 5, .y = 5 },
    }, 2);
    struct ui_event release = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_RELEASE, .button = 0, .pos = { .x = 5, .y = 5 },
    }, 3);
    assert(ui_widget_event(&button.widget, &press));
    assert(!ui_widget_event(&button.widget, &wrong_release));
    assert(ui_button_is_pressed(&button));
    assert(ui_widget_event(&button.widget, &release));
    assert(count == 1 && !ui_button_is_pressed(&button));
    ui_widget_destroy(ui_box_widget(&panel));
    ui_surface_destroy(&context);
}

static void label_natural_size_test(void)
{
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    struct ui_label label = {0};
    ui_box_init(&box);
    ui_label_init(&label);
    ui_label_set_text(&label, "abcd");
    assert(ui_widget_add_child(&box.widget, &label.widget));
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &box.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    ui_surface_resize(&surface, (struct vector2i){ .x = 400, .y = 200 });
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(label.widget.bounds.width == 32);
    assert(label.widget.bounds.height == 16);

    ui_label_set_text(&label, "é中");
    assert(ui_surface_build_scene(&surface, &scene));
    assert(label.widget.bounds.width == 24);
    assert(label.widget.bounds.height == 16);

    ui_widget_destroy(&box.widget);
    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
}

static void label_utf8_storage_test(void)
{
    struct ui_label label = {0};
    ui_label_init(&label);

    char text[UI_LABEL_TEXT_CAP + 8];
    memset(text, 'a', UI_LABEL_TEXT_CAP - 2);
    memcpy(text + UI_LABEL_TEXT_CAP - 2, "\xE4\xB8\xAD", 3);
    text[UI_LABEL_TEXT_CAP + 1] = '\0';
    ui_label_set_text(&label, text);
    assert(strcmp(ui_label_text(&label), text) == 0);
    for (size_t i = 0; i < UI_LABEL_TEXT_CAP - 2; i++)
        assert(ui_label_text(&label)[i] == 'a');

    memset(text, 'a', UI_LABEL_TEXT_CAP - 3);
    memcpy(text + UI_LABEL_TEXT_CAP - 3, "\xC3\xA9", 2);
    text[UI_LABEL_TEXT_CAP - 1] = '\0';
    ui_label_set_text(&label, text);
    assert(strlen(ui_label_text(&label)) == UI_LABEL_TEXT_CAP - 1);
    assert((unsigned char)ui_label_text(&label)[UI_LABEL_TEXT_CAP - 3] ==
           0xC3);

    ui_widget_destroy(&label.widget);
}

static int count_color_rects(const struct ui_scene *scene, uint32_t color)
{
    int n = 0;
    for (int i = 0; i < scene->n_ops; i++) {
        if (scene->ops[i].kind == UI_SCENE_OP_RECT &&
            scene->ops[i].color == color)
            n++;
    }
    return n;
}

static void button_paint_and_focus_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    struct ui_button button = {0};
    button_in_surface(&sheet,
        ".button { background: #112233; color: #aabbcc; "
        "padding: 2px 4px; }",
        &surface, &box, &button);
    size_200x60(&surface);

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(button.widget.bounds.width == 24);
    assert(button.widget.bounds.height == 20);

    bool bg = false, text = false;
    for (int i = 0; i < scene.n_ops; i++) {
        const struct ui_scene_op *op = &scene.ops[i];
        if (op->kind == UI_SCENE_OP_RECT && op->color == 0xFF112233 &&
            op->rect.x == button.widget.bounds.x &&
            op->rect.y == button.widget.bounds.y &&
            op->rect.width == button.widget.bounds.width &&
            op->rect.height == button.widget.bounds.height)
            bg = true;
        if (op->kind == UI_SCENE_OP_TEXT && op->color == 0xFFAABBCC &&
            op->text_len == 2 &&
            !memcmp(ui_scene_op_text(&scene, op), "OK", 2))
            text = true;
    }
    assert(bg && text);
    assert(count_color_rects(&scene, 0xFFAABBCC) == 0);

    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == &button.widget);
    assert(ui_widget_has_focus(&button.widget));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(count_color_rects(&scene, 0xFFAABBCC) == 4);

    ui_widget_destroy(&box.widget);
    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static void button_key_activation_test(void)
{
    struct ui_surface context = {0};
    struct ui_box panel = {0};
    struct ui_button button = {0};
    int count = 0;
    panel_init(&panel);
    ui_button_init(&button);
    assert(button.widget.impl.focusable);
    assert(ui_widget_add_child(ui_box_widget(&panel), &button.widget));
    ui_surface_set_root(&context, ui_box_widget(&panel));
    ui_widget_set_action_handler(&button.widget, activated, &count);
    assert(ui_surface_focus_next(&context));
    assert(ui_surface_focused(&context) == &button.widget);

    struct ui_event enter = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_ENTER }, 1);
    assert(ui_surface_dispatch(&context, &enter));
    assert(count == 1);
    struct ui_event space = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_CHAR, .text = ' ' }, 2);
    assert(ui_surface_dispatch(&context, &space));
    assert(count == 2);
    struct ui_event other = ui_event_key(
        (struct ui_key_event){ .key = UI_KEY_CHAR, .text = 'x' }, 3);
    assert(!ui_surface_dispatch(&context, &other));
    assert(count == 2);
    ui_widget_destroy(ui_box_widget(&panel));
    ui_surface_destroy(&context);
}

static void disabling_clears_focus_test(void)
{
    struct ui_button button = {0};
    struct ui_surface surface = {0};
    ui_button_init(&button);
    ui_surface_set_root(&surface, &button.widget);
    ui_surface_focus(&surface, &button.widget);
    assert(ui_widget_has_focus(&button.widget));
    ui_button_set_disabled(&button, true);
    assert(!ui_widget_has_focus(&button.widget));
    assert(!ui_button_is_pressed(&button));
    assert(!(button.widget.impl.style_pseudo & CSS_PSEUDO_HOVER));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&button.widget);
}

static void disabled_button_leaves_tab_order_test(void)
{
    struct ui_surface context = {0};
    struct ui_box panel = {0};
    struct ui_button first = {0};
    struct ui_button second = {0};
    panel_init(&panel);
    ui_button_init(&first);
    ui_button_init(&second);
    assert(ui_widget_add_child(ui_box_widget(&panel), &first.widget));
    assert(ui_widget_add_child(ui_box_widget(&panel), &second.widget));
    ui_surface_set_root(&context, ui_box_widget(&panel));

    ui_button_set_disabled(&first, true);
    assert(!first.widget.impl.focusable);
    assert(ui_surface_focus_next(&context));
    assert(ui_surface_focused(&context) == &second.widget);

    ui_button_set_disabled(&first, false);
    assert(first.widget.impl.focusable);
    assert(ui_surface_focus_next(&context));
    assert(ui_surface_focused(&context) == &first.widget);
    ui_widget_destroy(ui_box_widget(&panel));
    ui_surface_destroy(&context);
}

static void button_scale_resolves_padding_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    struct ui_button button = {0};
    button_in_surface(&sheet, ".button { padding: 10px; }",
                      &surface, &box, &button);
    size_200x60(&surface);
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(button.widget.bounds.width == 36);
    assert(button.widget.bounds.height == 36);

    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 2.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(button.widget.bounds.width == 56);
    assert(button.widget.bounds.height == 56);

    ui_widget_destroy(&box.widget);
    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static void button_text_padded_inset_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    struct ui_button button = {0};
    button_in_surface(&sheet, ".button { padding: 2px 0px 2px 20px; }",
                      &surface, &box, &button);
    size_200x60(&surface);
    assert(button.widget.bounds.width == 36 &&
           button.widget.bounds.height == 20);

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    bool found = false;
    for (int i = 0; i < scene.n_ops; i++) {
        const struct ui_scene_op *op = &scene.ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == 2 &&
            !memcmp(ui_scene_op_text(&scene, op), "OK", 2)) {
            found = true;
            assert(op->rect.x == 20 && op->rect.y == 2);
            assert(op->rect.width == 16 && op->rect.height == 16);
        }
    }
    assert(found);
    ui_widget_destroy(&box.widget);
    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static void zero_state_test(void)
{
    struct ui_box panel = {0};
    struct ui_label label = {0};
    struct ui_button button = {0};
    struct ui_input input = {0};
    struct ui_scrollview scroll = {0};

    assert(ui_box_widget(&panel) != NULL);
    assert(panel.widget.ops != NULL);

    assert(ui_label_text(&label)[0] == '\0');
    ui_label_set_text(&label, "Zero-Init Label");
    assert(label.widget.ops != NULL);
    assert(!strcmp(ui_label_text(&label), "Zero-Init Label"));

    assert(ui_button_text(&button)[0] == '\0');
    assert(!ui_button_is_disabled(&button));
    assert(!ui_button_is_pressed(&button));
    ui_button_set_text(&button, "Zero-Init Button");
    assert(button.widget.ops != NULL);
    assert(!strcmp(ui_button_text(&button), "Zero-Init Button"));

    ui_input_set_text(&input, "Zero-Init Input");
    assert(input.widget.ops != NULL);
    assert(!strcmp(ui_input_text(&input), "Zero-Init Input"));

    ui_scrollview_set_scroll_step(&scroll, 16);
    assert(scroll.widget.ops != NULL);

    ui_scrollview_flash(&scroll);
    assert(scroll.widget.ops != NULL);

    /* Test that these zero-init widgets can be mounted into a surface tree. */
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_surface_init(&surface, NULL, NULL);

    assert(ui_widget_add_child(UI_WIDGET(&panel), UI_WIDGET(&label)));
    assert(ui_widget_add_child(UI_WIDGET(&panel), UI_WIDGET(&button)));
    assert(ui_widget_add_child(UI_WIDGET(&panel), UI_WIDGET(&input)));

    ui_surface_set_root(&surface, UI_WIDGET(&panel));
    ui_surface_resize(&surface, (struct vector2i){ .x = 200, .y = 200 });
    assert(ui_surface_build_scene(&surface, &scene));

    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    ui_widget_destroy(UI_WIDGET(&panel));
    ui_widget_destroy(UI_WIDGET(&scroll));
}

int main(void)
{
    panel_measure_and_label_test();
    label_paint_produces_text_op_test();
    label_empty_produces_no_text_op_test();
    label_natural_size_test();
    label_utf8_storage_test();
    button_activation_and_disabled_test();
    button_paint_and_focus_test();
    button_key_activation_test();
    context_requests_state_redraw_test();
    explicit_focus_targets_one_control_test();
    mismatched_release_preserves_press_test();
    disabling_clears_focus_test();
    disabled_button_leaves_tab_order_test();
    button_scale_resolves_padding_test();
    button_text_padded_inset_test();
    zero_state_test();
    return 0;
}
