
#include <assert.h>
#include <string.h>

#include "ui/ui.h"
#include "ui/debug.h"
#include "ui/widget_impl.h"

static const struct css_scale_context SCALE = {
    .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
};

static void show(struct ui_surface *surface, struct ui_widget *root,
                 struct vector2i size)
{
    ui_surface_init(surface, NULL, NULL);
    ui_surface_set_root(surface, root);
    ui_surface_set_scale(surface, SCALE);
    ui_surface_resize(surface, size);
}

static void click(struct ui_surface *surface, struct vector2i pos)
{
    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = pos,
    }, 0);
    struct ui_event release = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_RELEASE, .pos = pos,
    }, 1);
    ui_surface_dispatch(surface, &press);
    ui_surface_dispatch(surface, &release);
}

static bool record_id(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    if (action->kind == UI_ACTION_ACTIVATE)
        *(int *)userdata = action->id;
    return true;
}

static void hidden_child_takes_no_space_or_input_test(void)
{
    struct ui_box column = {0};
    struct ui_button first = {0}, second = {0};
    struct ui_surface surface;
    int chosen = 0;
    ui_box_init(&column);
    ui_box_set_direction(&column, UI_BOX_COLUMN);
    ui_button_init(&first);
    ui_button_init(&second);
    ui_button_set_action_id(&first, 1);
    ui_button_set_action_id(&second, 2);
    ui_widget_set_fixed_size(&first.widget, (struct vector2i){ .x = 40, .y = 20 });
    ui_widget_set_fixed_size(&second.widget, (struct vector2i){ .x = 40, .y = 20 });
    ui_widget_set_action_handler(&column.widget, record_id, &chosen);
    assert(ui_widget_add_child(&column.widget, &first.widget));
    assert(ui_widget_add_child(&column.widget, &second.widget));
    show(&surface, &column.widget, (struct vector2i){ .x = 100, .y = 100 });
    assert(second.widget.bounds.y == 20);

    ui_surface_focus(&surface, &first.widget);
    ui_widget_set_hidden(&first.widget, true);
    assert(ui_widget_is_hidden(&first.widget));
    assert(!ui_widget_has_focus(&first.widget));
    ui_surface_prepare(&surface);
    assert(second.widget.bounds.y == 0);

    click(&surface, (struct vector2i){ .x = 4, .y = 4 });
    assert(chosen == 2);
    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == &second.widget);
    assert(ui_surface_focus_next(&surface));
    assert(ui_surface_focused(&surface) == &second.widget);

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    ui_widget_set_hidden(&first.widget, false);
    ui_surface_prepare(&surface);
    assert(second.widget.bounds.y == 20);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&column.widget);
    ui_scene_destroy(&scene);
}

static void hidden_modal_restores_focus_test(void)
{
    struct ui_widget root = {0};
    struct ui_box dialog = {0};
    struct ui_button behind = {0}, inside = {0};
    struct ui_surface surface;
    ui_widget_init(&root, NULL, NULL);
    ui_box_init(&dialog);
    ui_button_init(&behind);
    ui_button_init(&inside);
    assert(ui_widget_add_child(&root, &behind.widget));
    assert(ui_widget_add_child(&dialog.widget, &inside.widget));
    assert(ui_widget_add_child(&root, &dialog.widget));
    ui_widget_set_modal(&dialog.widget, true);
    ui_widget_set_hidden(&dialog.widget, true);
    show(&surface, &root, (struct vector2i){ .x = 100, .y = 100 });
    assert(!ui_surface_has_modal(&surface));

    ui_surface_focus(&surface, &behind.widget);
    ui_widget_set_hidden(&dialog.widget, false);
    assert(ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == &inside.widget);

    ui_widget_set_hidden(&dialog.widget, true);
    assert(!ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == &behind.widget);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
}

static void placement_test(void)
{
    struct ui_widget root = {0};
    struct ui_button child = {0};
    struct ui_surface surface;
    ui_widget_init(&root, NULL, NULL);
    ui_button_init(&child);
    ui_widget_set_fixed_size(&child.widget, (struct vector2i){ .x = 40, .y = 20 });
    assert(ui_widget_add_child(&root, &child.widget));
    show(&surface, &root, (struct vector2i){ .x = 200, .y = 100 });
    assert(child.widget.bounds.width == 200 && child.widget.bounds.height == 100);

    ui_widget_set_placement(&child.widget, (struct ui_placement){
        .kind = UI_PLACE_CENTER, .y_fraction = 0.5f,
    });
    ui_surface_prepare(&surface);
    assert(child.widget.bounds.x == 80 && child.widget.bounds.y == 40);
    assert(child.widget.bounds.width == 40);

    struct rect2i anchor = { .pos = { .x = 10, .y = 10 },
                             .size = { .x = 30, .y = 10 } };
    ui_widget_set_placement(&child.widget, (struct ui_placement){
        .kind = UI_PLACE_BELOW, .anchor = anchor,
    });
    ui_surface_prepare(&surface);
    assert(child.widget.bounds.x == 10 && child.widget.bounds.y == 20);

    anchor.y = 85;
    ui_widget_set_placement(&child.widget, (struct ui_placement){
        .kind = UI_PLACE_BELOW, .anchor = anchor,
    });
    ui_surface_prepare(&surface);
    assert(child.widget.bounds.y == 65);

    ui_widget_set_placement(&child.widget, (struct ui_placement){
        .kind = UI_PLACE_RIGHT, .anchor = anchor,
    });
    ui_surface_prepare(&surface);
    assert(child.widget.bounds.x == 40 && child.widget.bounds.y == 80);

    ui_widget_set_placement(&child.widget, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor = { .pos = { .x = 190, .y = 5 } },
    });
    ui_surface_prepare(&surface);
    assert(child.widget.bounds.x == 160 && child.widget.bounds.y == 5);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
}

struct timer {
    struct ui_widget widget;
    uint64_t due;
    int ticks;
};

static uint64_t timer_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct timer *t = ui_container_of(widget, struct timer, widget);
    t->ticks++;
    if (t->due && now_ms >= t->due)
        t->due = 0;
    return t->due;
}

static const struct ui_widget_ops TIMER_OPS = {
    .name = "timer",
    .tick = timer_tick,
};

static void deadline_test(void)
{
    struct timer timer = {0};
    struct ui_surface surface;
    ui_widget_init(&timer.widget, &TIMER_OPS, NULL);
    show(&surface, &timer.widget, (struct vector2i){ .x = 10, .y = 10 });

    timer.due = 600;
    assert(!ui_surface_tick(&surface, 100));
    assert(ui_surface_next_deadline(&surface) == 600);
    (void)ui_surface_take_frame_request(&surface);

    assert(!ui_surface_tick(&surface, 600));
    assert(ui_surface_next_deadline(&surface) == 0);
    assert(!ui_surface_frame_pending(&surface));

    timer.due = 700;
    int ticks = timer.ticks;
    ui_widget_set_hidden(&timer.widget, true);
    assert(!ui_surface_tick(&surface, 650));
    assert(timer.ticks == ticks);
    assert(ui_surface_next_deadline(&surface) == 0);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&timer.widget);
}

static uint64_t animate_tick(struct ui_widget *widget, uint64_t now_ms)
{
    (void)widget;
    return now_ms;
}

static const struct ui_widget_ops ANIMATE_OPS = { .tick = animate_tick };

static void animation_requests_next_frame_test(void)
{
    struct ui_widget widget = {0};
    struct ui_surface surface;
    ui_widget_init(&widget, &ANIMATE_OPS, NULL);
    show(&surface, &widget, (struct vector2i){ .x = 10, .y = 10 });
    (void)ui_surface_take_frame_request(&surface);
    assert(ui_surface_tick(&surface, 5));
    assert(ui_surface_frame_pending(&surface));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&widget);
}

struct key_probe {
    struct ui_widget widget;
    int presses;
    int releases;
};

static bool key_probe_event(struct ui_widget *widget,
                            const struct ui_event *event)
{
    struct key_probe *p = ui_container_of(widget, struct key_probe, widget);
    if (event->kind != UI_EVENT_KEY)
        return false;
    if (event->data.key.action == UI_KEY_RELEASE)
        p->releases++;
    else
        p->presses++;
    return true;
}

static const struct ui_widget_ops PLAIN_KEYS_OPS = { .event = key_probe_event };
static const struct ui_widget_ops RELEASE_KEYS_OPS = {
    .event = key_probe_event,
    .key_release = true,
};

static void key_release_reaches_only_opted_in_widgets_test(void)
{
    struct key_probe parent = {0}, child = {0};
    struct ui_surface surface;
    ui_widget_init(&parent.widget, &RELEASE_KEYS_OPS, NULL);
    ui_widget_init(&child.widget, &PLAIN_KEYS_OPS, NULL);
    ui_widget_set_focusable(&child.widget, true);
    assert(ui_widget_add_child(&parent.widget, &child.widget));
    show(&surface, &parent.widget, (struct vector2i){ .x = 10, .y = 10 });
    ui_surface_focus(&surface, &child.widget);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = 'a',
    }, 0);
    assert(ui_surface_dispatch(&surface, &key));
    assert(child.presses == 1 && parent.presses == 0);

    key.data.key.action = UI_KEY_RELEASE;
    assert(ui_surface_dispatch(&surface, &key));
    assert(child.releases == 0 && parent.releases == 1);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&parent.widget);
}

static void paste_types_into_input_test(void)
{
    struct ui_input input;
    struct ui_surface surface;
    ui_input_init(&input);
    show(&surface, ui_input_widget(&input), (struct vector2i){ .x = 200, .y = 30 });
    ui_surface_focus(&surface, ui_input_widget(&input));
    struct ui_event paste = ui_event_paste("abc", 3, 0);
    assert(ui_surface_dispatch(&surface, &paste));
    assert(strcmp(ui_input_text(&input), "abc") == 0);
    ui_surface_destroy(&surface);
    ui_widget_destroy(ui_input_widget(&input));
}

static void button_sends_its_action_id_test(void)
{
    struct ui_button button;
    struct ui_surface surface;
    int chosen = 0;
    ui_button_init(&button);
    ui_button_set_action_id(&button, 42);
    ui_widget_set_action_handler(&button.widget, record_id, &chosen);
    show(&surface, &button.widget, (struct vector2i){ .x = 80, .y = 30 });
    ui_surface_focus(&surface, &button.widget);
    struct ui_event enter = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 0);
    assert(ui_surface_dispatch(&surface, &enter));
    assert(chosen == 42);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&button.widget);
}

static void view_diagnostic_test(void)
{
    struct ui_input *bound = NULL;
    struct ui_node_desc children[] = {
        { .kind = UI_NODE_INPUT, .key = "name", .bind.input = &bound },
        { .kind = UI_NODE_BUTTON, .key = "name" },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .children = children, .child_count = 2,
    };
    struct ui_view_diagnostic diagnostic = {0};
    assert(!ui_view_mount_ex(&root, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_DUPLICATE_KEY);
    assert(diagnostic.node == &children[1]);
    assert(diagnostic.detail && strstr(diagnostic.detail, "key"));
    assert(strstr(ui_view_error_message(diagnostic.error), "key"));
    assert(bound == NULL);

    children[1].key = "submit";
    children[1].bind.input = &bound;
    assert(!ui_view_mount_ex(&root, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_INVALID_ARGUMENT);
    assert(diagnostic.node == &children[1]);
    assert(bound == NULL);

    children[1].bind.input = NULL;
    struct ui_view *view = ui_view_mount_ex(&root, &diagnostic);
    assert(view && diagnostic.error == UI_VIEW_ERROR_NONE && !diagnostic.node);
    assert(bound == ui_view_input(view, "name"));
    assert(ui_debug_find(ui_view_root(view), "@name") == ui_input_widget(bound));
    ui_view_destroy(view);

    enum ui_view_error error = UI_VIEW_ERROR_NONE;
    assert(!ui_view_mount(NULL, &error));
    assert(error == UI_VIEW_ERROR_INVALID_ARGUMENT);
}

static void view_children_macro_test(void)
{
    int calls = 0;
    const struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN,
        UI_CHILDREN(
            { .kind = UI_NODE_LABEL, .props.label.text = "First" },
            { .kind = UI_NODE_BUTTON, .action_id = ++calls },
            { .kind = UI_NODE_INPUT }
        ),
    };
    assert(calls == 1);
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);
    assert(ui_widget_child_count(ui_view_root(view)) == 3);
    ui_view_destroy(view);
}

static void view_automatic_overlay_test(void)
{
    const struct ui_dropdown_item items[] = { { "One", "one" } };
    struct ui_dropdown *dropdown = NULL;
    struct ui_node_desc child = {
        .kind = UI_NODE_DROPDOWN,
        .key = "choice",
        .bind.dropdown = &dropdown,
        .props.dropdown = { .items = items, .count = 1 },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "content",
        .children = &child, .child_count = 1,
    };
    struct ui_view_diagnostic diagnostic;
    struct ui_view *view = ui_view_mount_ex(&root, &diagnostic);
    assert(view && dropdown);
    assert(ui_view_root(view) != ui_view_widget(view, "content"));
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 300, .y = 200 });
    ui_surface_prepare(&surface);
    assert(ui_dropdown_open(dropdown));
    assert(ui_dropdown_is_open(dropdown));
    ui_dropdown_close(dropdown);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);

    root.kind = UI_NODE_STACK;
    view = ui_view_mount_ex(&root, &diagnostic);
    assert(view);
    assert(ui_view_root(view) != ui_view_widget(view, "content"));
    ui_view_destroy(view);
}

static void view_explicit_overlay_test(void)
{
    const struct ui_dropdown_item items[] = { { "One", "one" } };
    struct ui_dropdown *dropdown = NULL;
    struct ui_node_desc children[] = {
        { .kind = UI_NODE_STACK, .key = "overlay" },
        { .kind = UI_NODE_DROPDOWN, .key = "choice",
          .bind.dropdown = &dropdown,
          .props.dropdown = { .items = items, .count = 1,
                              .overlay_key = "overlay" } },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_ROW, .children = children, .child_count = 2,
    };
    struct ui_view_diagnostic diagnostic;
    struct ui_view *view = ui_view_mount_ex(&root, &diagnostic);
    assert(view && dropdown);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 300, .y = 200 });
    ui_surface_prepare(&surface);
    assert(ui_dropdown_open(dropdown));
    ui_dropdown_close(dropdown);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);

    children[0].kind = UI_NODE_LABEL;
    dropdown = NULL;
    assert(!ui_view_mount_ex(&root, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_INVALID_OVERLAY);
    assert(diagnostic.node == &children[1]);
    assert(diagnostic.detail && strstr(diagnostic.detail, "stack"));
    assert(!dropdown);
}

struct supplied_control {
    struct ui_widget widget;
    int *destroyed;
};

static bool supplied_control_event(struct ui_widget *widget,
                                   const struct ui_event *event)
{
    if (event->kind != UI_EVENT_KEY ||
        event->data.key.action == UI_KEY_RELEASE ||
        event->data.key.key != UI_KEY_ENTER)
        return false;
    return ui_widget_notify(widget, (struct ui_action){
        .kind = UI_ACTION_ACTIVATE, .id = 42,
    });
}

static void supplied_control_destroy(struct ui_widget *widget)
{
    struct supplied_control *control =
        ui_container_of(widget, struct supplied_control, widget);
    (*control->destroyed)++;
}

static const struct ui_widget_ops SUPPLIED_CONTROL_OPS = {
    .name = "supplied-control",
    .event = supplied_control_event,
    .destroy = supplied_control_destroy,
};

static void view_supplied_widget_test(void)
{
    int destroyed = 0, action = 0;
    struct supplied_control control = { .destroyed = &destroyed };
    ui_widget_init(&control.widget, &SUPPLIED_CONTROL_OPS, NULL);
    ui_widget_set_focusable(&control.widget, true);
    ui_widget_set_style_identity(&control.widget, "original", NULL, "custom");
    struct ui_node_desc children[] = {
        { .kind = UI_NODE_WIDGET, .key = "custom", .props.widget = &control.widget },
        { .kind = UI_NODE_LABEL, .key = "other" },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .children = children, .child_count = 2,
        .on_action = record_id, .action_userdata = &action,
    };
    struct ui_view_diagnostic diagnostic;

    /* A failed mount leaves the supplied widget ready for a later mount. */
    children[1].key = "custom";
    assert(!ui_view_mount_ex(&root, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_DUPLICATE_KEY);
    assert(!control.widget.parent && !ui_widget_surface(&control.widget));
    assert(!destroyed);
    children[1].key = "other";

    struct ui_view *view = ui_view_mount_ex(&root, &diagnostic);
    assert(view && ui_view_widget(view, "custom") == &control.widget);
    assert(ui_debug_find(ui_view_root(view), "@custom") == &control.widget);
    assert(ui_debug_find(ui_view_root(view), ".original") == &control.widget);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 100, .y = 100 });
    ui_surface_focus(&surface, &control.widget);
    struct ui_event enter = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 0);
    assert(ui_surface_dispatch(&surface, &enter));
    assert(action == 42);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
    assert(destroyed == 1);
}

static void view_supplied_widget_validation_test(void)
{
    struct ui_widget widget = {0};
    struct ui_node_desc node = { .kind = UI_NODE_WIDGET, .props.widget = &widget };
    struct ui_view_diagnostic diagnostic;
    assert(!ui_view_mount_ex(&node, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_INVALID_ARGUMENT);

    ui_widget_init(&widget, NULL, NULL);
    struct ui_node_desc children[] = { node, node };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .children = children, .child_count = 2,
    };
    assert(!ui_view_mount_ex(&root, &diagnostic));
    assert(diagnostic.node == &children[1]);
    assert(!widget.parent);

    struct ui_widget parent = {0};
    ui_widget_init(&parent, NULL, NULL);
    assert(ui_widget_add_child(&parent, &widget));
    assert(!ui_view_mount_ex(&node, &diagnostic));
    assert(widget.parent == &parent);
    assert(ui_widget_remove_child(&parent, &widget));
    struct ui_view *view = ui_view_mount_ex(&node, &diagnostic);
    assert(view && ui_view_root(view) == &widget);
    ui_view_destroy(view);
    ui_widget_destroy(&parent);
}

static const struct ui_widget_ops SINGLE_CHILD_OPS = {
    .name = "single-child",
    .child_limit = 1,
};

static void view_supplied_container_test(void)
{
    struct ui_widget container = {0};
    ui_widget_init(&container, &SINGLE_CHILD_OPS, NULL);
    struct ui_node_desc child = {
        .kind = UI_NODE_BUTTON, .key = "button",
        .props.button = { .text = "Apply" },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_WIDGET, .key = "container", .props.widget = &container,
        .children = &child, .child_count = 1,
    };
    struct ui_view_diagnostic diagnostic;
    struct ui_view *view = ui_view_mount_ex(&root, &diagnostic);
    assert(view);
    assert(ui_view_widget(view, "container") == &container);
    assert(ui_widget_child_count(&container) == 1);
    assert(ui_view_widget(view, "button") == ui_widget_child_at(&container, 0));
    ui_view_destroy(view);

    struct ui_widget old_child = {0};
    ui_widget_init(&container, &SINGLE_CHILD_OPS, NULL);
    ui_widget_init(&old_child, NULL, NULL);
    assert(ui_widget_add_child(&container, &old_child));
    assert(!ui_view_mount_ex(&root, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_INVALID_CHILDREN);
    assert(diagnostic.node == &child);
    assert(ui_widget_child_count(&container) == 1);
    assert(ui_widget_child_at(&container, 0) == &old_child);
    assert(!container.parent);
    ui_widget_destroy(&container);
}

static void view_supplied_root_with_popup_test(void)
{
    int destroyed = 0;
    struct supplied_control control = { .destroyed = &destroyed };
    ui_widget_init(&control.widget, &SUPPLIED_CONTROL_OPS, NULL);
    const struct ui_dropdown_item items[] = { { "One", "one" } };
    struct ui_dropdown *dropdown = NULL;
    struct ui_node_desc child = {
        .kind = UI_NODE_DROPDOWN,
        .bind.dropdown = &dropdown,
        .props.dropdown = { .items = items, .count = 1 },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_WIDGET, .key = "content", .props.widget = &control.widget,
        .children = &child, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && dropdown);
    assert(ui_view_widget(view, "content") == &control.widget);
    assert(ui_view_root(view) != &control.widget);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 300, .y = 200 });
    assert(ui_dropdown_open(dropdown));
    ui_view_destroy(view);
    assert(!ui_surface_root(&surface));
    assert(!ui_surface_has_modal(&surface));
    assert(destroyed == 1);
    ui_surface_destroy(&surface);
}

static bool view_root_event(struct ui_widget *widget,
                            const struct ui_event *event, void *userdata)
{
    (void)widget;
    if (event->kind != UI_EVENT_KEY || event->data.key.key != UI_KEY_ESCAPE)
        return false;
    (*(int *)userdata)++;
    return true;
}

static void view_overlay_root_keyboard_test(void)
{
    int calls = 0;
    struct ui_node_desc child = { .kind = UI_NODE_DROPDOWN };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "content",
        .children = &child, .child_count = 1,
        .on_event = view_root_event, .event_userdata = &calls,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);
    struct ui_widget *host = ui_view_root(view);
    struct ui_widget *content = ui_view_widget(view, "content");
    assert(host != content);
    struct ui_surface surface;
    show(&surface, host, (struct vector2i){ .x = 300, .y = 200 });
    struct ui_event escape = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ESCAPE,
    }, 0);
    assert(ui_surface_dispatch(&surface, &escape));
    assert(calls == 1);
    ui_surface_set_default_focus(&surface, host);
    assert(ui_surface_dispatch(&surface, &escape));
    assert(calls == 2);
    assert(ui_widget_event(host, &escape));
    assert(calls == 3);
    ui_surface_focus(&surface, host);
    assert(ui_surface_focused(&surface) == content);
    assert(ui_surface_dispatch(&surface, &escape));
    assert(calls == 4);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}

int main(void)
{
    view_diagnostic_test();
    view_children_macro_test();
    view_automatic_overlay_test();
    view_explicit_overlay_test();
    view_supplied_widget_test();
    view_supplied_widget_validation_test();
    view_supplied_container_test();
    view_supplied_root_with_popup_test();
    view_overlay_root_keyboard_test();
    hidden_child_takes_no_space_or_input_test();
    hidden_modal_restores_focus_test();
    placement_test();
    deadline_test();
    animation_requests_next_frame_test();
    key_release_reaches_only_opted_in_widgets_test();
    paste_types_into_input_test();
    button_sends_its_action_id_test();
    return 0;
}
