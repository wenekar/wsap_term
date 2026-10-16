#include <assert.h>
#include <string.h>

#include "ui/ui.h"
#include "ui/widget_impl.h"

static void click(struct ui_surface *surface, struct vector2i pos)
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

/* Inits surface with root at 8 by 16 cells, sized to size. */
static void setup_surface(struct ui_surface *surface, struct ui_widget *root,
                          struct vector2i size)
{
    ui_surface_init(surface, NULL, NULL);
    ui_surface_set_root(surface, root);
    ui_surface_set_scale(surface, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(surface, size);
}

static bool count_action(struct ui_widget *source,
                         const struct ui_action *action, void *userdata)
{
    (void)source;
    assert(action->kind == UI_ACTION_ACTIVATE);
    (*(int *)userdata)++;
    return false;
}

static void topmost_child_receives_pointer_test(void)
{
    struct ui_box root = {0};
    struct ui_button lower = {0}, upper = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    int lower_clicks = 0, upper_clicks = 0;
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_STACK);
    ui_button_init(&lower);
    ui_button_init(&upper);
    ui_widget_set_fixed_size(&lower.widget, (struct vector2i){ .x = 80, .y = 30 });
    ui_widget_set_fixed_size(&upper.widget, (struct vector2i){ .x = 80, .y = 30 });
    ui_widget_set_action_handler(&lower.widget, count_action, &lower_clicks);
    ui_widget_set_action_handler(&upper.widget, count_action, &upper_clicks);
    assert(ui_widget_add_child(&root.widget, &lower.widget));
    assert(ui_widget_add_child(&root.widget, &upper.widget));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 80, .y = 30 });
    assert(ui_surface_build_scene(&surface, &scene));

    struct ui_event motion = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = { .x = 4, .y = 4 },
    }, 0);
    ui_surface_dispatch(&surface, &motion);
    assert(upper.widget.impl.style_pseudo & CSS_PSEUDO_HOVER);
    motion.data.pointer.pos = (struct vector2i){ .x = 90, .y = 40 };
    ui_surface_dispatch(&surface, &motion);
    assert(!(upper.widget.impl.style_pseudo & CSS_PSEUDO_HOVER));

    click(&surface, (struct vector2i){ .x = 4, .y = 4 });
    assert(lower_clicks == 0);
    assert(upper_clicks == 1);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_scene_destroy(&scene);
}

static void handler_survives_attachment_and_reparent_test(void)
{
    struct ui_box first_root = {0}, second_root = {0};
    struct ui_button button = {0};
    struct ui_surface first = {0}, second = {0};
    struct ui_scene scene = {0};
    int activations = 0;
    ui_box_init(&first_root);
    ui_box_init(&second_root);
    ui_box_set_direction(&first_root, UI_BOX_STACK);
    ui_box_set_direction(&second_root, UI_BOX_STACK);
    ui_button_init(&button);
    ui_widget_set_fixed_size(&button.widget, (struct vector2i){ .x = 80, .y = 30 });
    ui_widget_set_action_handler(&button.widget, count_action, &activations);
    assert(ui_widget_add_child(&first_root.widget, &button.widget));

    ui_surface_init(&first, NULL, NULL);
    ui_surface_init(&second, NULL, NULL);
    ui_surface_set_root(&first, &first_root.widget);
    ui_surface_set_root(&second, &second_root.widget);
    ui_surface_set_scale(&first, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(&first, (struct vector2i){ .x = 80, .y = 30 });
    ui_surface_set_scale(&second, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(&second, (struct vector2i){ .x = 80, .y = 30 });
    assert(ui_surface_build_scene(&first, &scene));
    click(&first, (struct vector2i){ .x = 4, .y = 4 });
    assert(activations == 1);

    assert(ui_widget_remove_child(&first_root.widget, &button.widget));
    assert(ui_widget_add_child(&second_root.widget, &button.widget));
    assert(ui_surface_build_scene(&second, &scene));
    click(&second, (struct vector2i){ .x = 4, .y = 4 });
    assert(activations == 2);

    ui_surface_destroy(&first);
    ui_surface_destroy(&second);
    ui_widget_destroy(&first_root.widget);
    ui_widget_destroy(&second_root.widget);
    ui_scene_destroy(&scene);
}

struct destroy_action_state {
    int calls;
};

static bool destroy_on_action(struct ui_widget *source,
                              const struct ui_action *action, void *userdata)
{
    struct destroy_action_state *state = userdata;
    assert(action->kind == UI_ACTION_ACTIVATE);
    state->calls++;
    ui_widget_destroy(source);
    return true;
}

static void action_bubbles_and_can_destroy_source_test(void)
{
    struct ui_box root = {0};
    struct ui_button button = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    struct destroy_action_state state = {0};
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_STACK);
    ui_button_init(&button);
    ui_widget_set_fixed_size(&button.widget, (struct vector2i){ .x = 80, .y = 30 });
    ui_widget_set_action_handler(&root.widget, destroy_on_action, &state);
    assert(ui_widget_add_child(&root.widget, &button.widget));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 80, .y = 30 });
    assert(ui_surface_build_scene(&surface, &scene));

    click(&surface, (struct vector2i){ .x = 4, .y = 4 });
    assert(state.calls == 1);
    assert(button.widget.impl.destroyed);
    assert(ui_widget_child_count(&root.widget) == 0);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_scene_destroy(&scene);
}

static void container_child_limit_test(void)
{
    struct ui_scrollview scroll = {0};
    struct ui_label first = {0}, second = {0};
    ui_scrollview_init(&scroll);
    ui_label_init(&first);
    ui_label_init(&second);
    assert(ui_widget_add_child(&scroll.widget, &first.widget));
    assert(!ui_widget_add_child(&scroll.widget, &second.widget));
    assert(ui_widget_child_count(&scroll.widget) == 1);
    ui_widget_destroy(&scroll.widget);
    ui_widget_destroy(&second.widget);
}

static void content_change_appears_on_next_frame_test(void)
{
    struct ui_box root = {0};
    struct ui_label label = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_box_init(&root);
    ui_label_init(&label);
    assert(ui_widget_add_child(&root.widget, &label.widget));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 80, .y = 30 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(ui_surface_take_frame_request(&surface));
    assert(!ui_surface_frame_pending(&surface));

    ui_label_set_text(&label, "next frame");
    assert(ui_surface_frame_pending(&surface));
    assert(ui_surface_build_scene(&surface, &scene));
    bool found = false;
    for (int i = 0; i < scene.n_ops; i++) {
        const struct ui_scene_op *op = &scene.ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == 10 &&
            !memcmp(ui_scene_op_text(&scene, op), "next frame", 10))
            found = true;
    }
    assert(found);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_scene_destroy(&scene);
}

static void unchanged_input_and_list_stay_idle_test(void)
{
    static const struct ui_list_item items[] = {
        { .label = "Open", .action = 1 },
        { .label = "Close", .action = 2 },
    };
    struct ui_box root = {0};
    struct ui_input input = {0};
    struct ui_list list = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_COLUMN);
    ui_input_init(&input);
    ui_list_init(&list);
    assert(ui_list_set_items(&list, items, 2));
    assert(ui_widget_add_child(&root.widget, &input.widget));
    assert(ui_widget_add_child(&root.widget, ui_list_widget(&list)));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 160, .y = 80 });

    assert(ui_surface_take_frame_request(&surface));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(!ui_surface_frame_pending(&surface));

    for (int i = 0; i < 3; i++) {
        ui_surface_prepare(&surface);
        assert(ui_surface_build_scene(&surface, &scene));
        assert(!ui_surface_frame_pending(&surface));
    }

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_scene_destroy(&scene);
}

struct responsive_container {
    struct ui_widget widget;
    struct ui_surface *surface;
    struct ui_label compact;
    struct ui_label wide;
};

static void responsive_prepare(struct ui_widget *widget)
{
    struct responsive_container *container = widget->userdata;
    bool use_compact = ui_surface_viewport(container->surface).x < 100;
    struct ui_widget *desired = use_compact
        ? &container->compact.widget : &container->wide.widget;
    struct ui_widget *current = ui_widget_child_at(widget, 0);
    if (current == desired)
        return;
    if (current)
        assert(ui_widget_remove_child(widget, current));
    assert(ui_widget_add_child(widget, desired));
}

static const struct ui_widget_ops RESPONSIVE_OPS = {
    .name = "responsive-test-container",
    .prepare = responsive_prepare,
};

static bool scene_has_text(const struct ui_scene *scene, const char *text)
{
    size_t len = strlen(text);
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_TEXT && op->text_len == len &&
            !memcmp(ui_scene_op_text(scene, op), text, len))
            return true;
    }
    return false;
}

static void resize_prepares_current_content_before_layout_test(void)
{
    struct responsive_container container = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(&surface, (struct vector2i){ .x = 120, .y = 40 });
    container.surface = &surface;
    ui_widget_init(&container.widget, &RESPONSIVE_OPS, &container);
    ui_label_init(&container.compact);
    ui_label_init(&container.wide);
    ui_label_set_text(&container.compact, "compact");
    ui_label_set_text(&container.wide, "wide");
    ui_surface_set_root(&surface, &container.widget);
    assert(ui_widget_child_count(&container.widget) == 1);
    assert(ui_widget_child_at(&container.widget, 0) == &container.wide.widget);
    assert(container.wide.widget.bounds.width == 120);

    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_text(&scene, "wide"));
    assert(!scene_has_text(&scene, "compact"));

    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(&surface, (struct vector2i){ .x = 60, .y = 40 });
    assert(ui_widget_child_count(&container.widget) == 1);
    assert(ui_widget_child_at(&container.widget, 0) == &container.compact.widget);
    assert(container.compact.widget.bounds.width == 60);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_text(&scene, "compact"));
    assert(!scene_has_text(&scene, "wide"));

    ui_surface_destroy(&surface);
    ui_widget_destroy(&container.widget);
    ui_widget_destroy(&container.compact.widget);
    ui_widget_destroy(&container.wide.widget);
    ui_scene_destroy(&scene);
}

static void tab_and_text_dispatch_test(void)
{
    struct ui_box root = {0};
    struct ui_input input = {0};
    struct ui_button first = {0}, second = {0};
    struct ui_surface surface = {0};
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_COLUMN);
    ui_input_init(&input);
    ui_button_init(&first);
    ui_button_init(&second);
    assert(ui_widget_add_child(&root.widget, &input.widget));
    assert(ui_widget_add_child(&root.widget, &first.widget));
    assert(ui_widget_add_child(&root.widget, &second.widget));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 120, .y = 60 });

    struct ui_event tab = ui_event_key((struct ui_key_event){
        .key = UI_KEY_TAB,
    }, 0);
    assert(ui_surface_dispatch(&surface, &tab));
    assert(ui_surface_focused(&surface) == &input.widget);
    const char commit[] = "é";
    struct ui_event text = ui_event_text(commit, sizeof(commit) - 1, 1);
    assert(ui_surface_dispatch(&surface, &text));
    assert(!strcmp(ui_input_text(&input), commit));
    assert(ui_surface_dispatch(&surface, &tab));
    assert(ui_surface_focused(&surface) == &first.widget);
    tab.data.key.mods = UI_MOD_SHIFT;
    assert(ui_surface_dispatch(&surface, &tab));
    assert(ui_surface_focused(&surface) == &input.widget);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
}

static void modal_focus_traps_and_restores_test(void)
{
    struct ui_box root = {0}, modal = {0};
    struct ui_input input = {0};
    struct ui_button popup_button = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_COLUMN);
    ui_input_init(&input);
    assert(ui_widget_add_child(&root.widget, &input.widget));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 120, .y = 60 });

    struct ui_event tab = ui_event_key((struct ui_key_event){
        .key = UI_KEY_TAB,
    }, 0);
    assert(ui_surface_dispatch(&surface, &tab));
    assert(ui_surface_focused(&surface) == &input.widget);

    ui_box_init(&modal);
    ui_widget_set_modal(&modal.widget, true);
    ui_button_init(&popup_button);
    assert(ui_widget_add_child(&modal.widget, &popup_button.widget));
    assert(ui_widget_add_child(&root.widget, &modal.widget));
    assert(ui_surface_build_scene(&surface, &scene));

    assert(ui_surface_dispatch(&surface, &tab));
    assert(ui_surface_focused(&surface) == &popup_button.widget);
    tab.data.key.mods = UI_MOD_SHIFT;
    assert(ui_surface_dispatch(&surface, &tab));
    assert(ui_surface_focused(&surface) == &popup_button.widget);

    assert(ui_widget_remove_child(&root.widget, &modal.widget));
    struct ui_event escape = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ESCAPE,
    }, 1);
    ui_surface_dispatch(&surface, &escape);
    assert(ui_surface_focused(&surface) == &input.widget);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_widget_destroy(&modal.widget);
    ui_scene_destroy(&scene);
}

static void modal_cancels_pressed_capture_test(void)
{
    struct ui_box root = {0}, modal = {0};
    struct ui_button covered = {0}, popup_button = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    int activations = 0;
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_STACK);
    ui_button_init(&covered);
    ui_widget_set_fixed_size(&covered.widget,
                                 (struct vector2i){ .x = 80, .y = 30 });
    ui_widget_set_action_handler(&covered.widget, count_action, &activations);
    assert(ui_widget_add_child(&root.widget, &covered.widget));
    setup_surface(&surface, &root.widget, (struct vector2i){ .x = 80, .y = 30 });
    assert(ui_surface_build_scene(&surface, &scene));

    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 4, .y = 4 }, .button = 0,
    }, 0);
    assert(ui_surface_dispatch(&surface, &press));
    assert(ui_button_is_pressed(&covered));

    ui_box_init(&modal);
    ui_box_set_direction(&modal, UI_BOX_STACK);
    ui_widget_set_modal(&modal.widget, true);
    ui_button_init(&popup_button);
    ui_widget_set_fixed_size(&popup_button.widget,
                                 (struct vector2i){ .x = 80, .y = 30 });
    assert(ui_widget_add_child(&modal.widget, &popup_button.widget));
    assert(ui_widget_add_child(&root.widget, &modal.widget));

    struct ui_event release = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_RELEASE, .pos = { .x = 4, .y = 4 }, .button = 0,
    }, 1);
    ui_surface_dispatch(&surface, &release);
    assert(activations == 0);
    assert(!ui_button_is_pressed(&covered));

    assert(ui_widget_remove_child(&root.widget, &modal.widget));
    click(&surface, (struct vector2i){ .x = 4, .y = 4 });
    assert(activations == 1);
    assert(!ui_button_is_pressed(&covered));

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_widget_destroy(&modal.widget);
    ui_scene_destroy(&scene);
}

struct capture_probe {
    int events;
};

static bool capture_and_decline(struct ui_widget *widget,
                                const struct ui_event *event)
{
    struct capture_probe *probe = widget->userdata;
    if (event->kind == UI_EVENT_POINTER) {
        probe->events++;
        if (event->data.pointer.kind == UI_POINTER_PRESS)
            ui_widget_capture_pointer(widget);
    }
    return false;
}

static const struct ui_widget_ops CAPTURE_OPS = {
    .name = "capture-probe",
    .event = capture_and_decline,
};

struct parent_probe {
    int events;
};

static bool bubble_probe(struct ui_widget *widget,
                         const struct ui_event *event)
{
    struct parent_probe *probe = widget->userdata;
    if (event->kind == UI_EVENT_POINTER)
        probe->events++;
    return false;
}

static const struct ui_widget_ops PARENT_OPS = {
    .name = "bubble-probe",
    .event = bubble_probe,
};

static void capture_bubbles_without_sibling_fallback_test(void)
{
    struct ui_widget root = {0}, target = {0}, sibling = {0};
    struct capture_probe target_probe = {0}, sibling_probe = {0};
    struct parent_probe root_probe = {0};
    struct ui_surface surface = {0};
    ui_widget_init(&root, &PARENT_OPS, &root_probe);
    ui_widget_init(&target, &CAPTURE_OPS, &target_probe);
    ui_widget_init(&sibling, &CAPTURE_OPS, &sibling_probe);
    ui_widget_set_pointer_transparent(&sibling, true);
    assert(ui_widget_add_child(&root, &target));
    assert(ui_widget_add_child(&root, &sibling));
    setup_surface(&surface, &root, (struct vector2i){ .x = 40, .y = 20 });

    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 4, .y = 4 },
    }, 0);
    assert(!ui_surface_dispatch(&surface, &press));
    assert(target_probe.events == 1);
    assert(root_probe.events == 1);
    assert(sibling_probe.events == 0);

    press.data.pointer.pos = (struct vector2i){ .x = 80, .y = 80 };
    assert(!ui_surface_dispatch(&surface, &press));
    assert(target_probe.events == 2);
    assert(root_probe.events == 2);
    assert(sibling_probe.events == 0);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
}

struct nested_build_probe {
    struct ui_surface *surface;
    bool event_blocked;
    bool paint_blocked;
};

static bool nested_build_event(struct ui_widget *widget,
                               const struct ui_event *event)
{
    struct nested_build_probe *probe = widget->userdata;
    if (event->kind == UI_EVENT_KEY) {
        struct ui_scene nested = {0};
        probe->event_blocked =
            !ui_surface_build_scene(probe->surface, &nested);
        ui_scene_destroy(&nested);
    }
    return false;
}

static void nested_build_paint(const struct ui_widget *widget,
                               struct ui_paint_context *paint)
{
    (void)paint;
    struct nested_build_probe *probe = widget->userdata;
    struct ui_scene nested = {0};
    probe->paint_blocked = !ui_surface_build_scene(probe->surface, &nested);
    ui_scene_destroy(&nested);
}

static const struct ui_widget_ops NESTED_BUILD_OPS = {
    .name = "nested-build-probe",
    .event = nested_build_event,
    .paint = nested_build_paint,
};

static void nested_scene_build_is_rejected_test(void)
{
    struct nested_build_probe probe = {0};
    struct ui_widget root = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_surface_init(&surface, NULL, NULL);
    probe.surface = &surface;
    ui_widget_init(&root, &NESTED_BUILD_OPS, &probe);
    ui_surface_set_root(&surface, &root);
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1, .cell_w = 8, .cell_h = 16 });
    ui_surface_resize(&surface, (struct vector2i){ .x = 40, .y = 20 });

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ESCAPE,
    }, 0);
    ui_surface_dispatch(&surface, &key);
    assert(probe.event_blocked);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(probe.paint_blocked);

    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
}

int main(void)
{
    topmost_child_receives_pointer_test();
    handler_survives_attachment_and_reparent_test();
    action_bubbles_and_can_destroy_source_test();
    container_child_limit_test();
    content_change_appears_on_next_frame_test();
    unchanged_input_and_list_stay_idle_test();
    resize_prepares_current_content_before_layout_test();
    tab_and_text_dispatch_test();
    modal_focus_traps_and_restores_test();
    modal_cancels_pressed_capture_test();
    capture_bubbles_without_sibling_fallback_test();
    nested_scene_build_is_rejected_test();
    return 0;
}
