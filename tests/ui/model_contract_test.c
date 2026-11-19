
#include <assert.h>
#include <string.h>

#include "ui/ui.h"
#include "ui/debug.h"
#include "ui/widget_ops.h"

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

static void view_dropdown_overlay_test(void)
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
    assert(ui_view_root(view) == ui_view_widget(view, "content"));
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 300, .y = 200 });
    ui_surface_prepare(&surface);
    assert(ui_dropdown_open(dropdown));
    assert(ui_dropdown_is_open(dropdown));
    /* The list floats above the view root, as a surface overlay. */
    assert(ui_surface_overlay_count(&surface) == 1);
    ui_dropdown_close(dropdown);
    assert(ui_surface_overlay_count(&surface) == 0);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
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
    assert(ui_view_root(view) == &control.widget);
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

static void view_root_keyboard_test(void)
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
    struct ui_widget *content = ui_view_root(view);
    assert(content == ui_view_widget(view, "content"));
    struct ui_surface surface;
    show(&surface, content, (struct vector2i){ .x = 300, .y = 200 });
    struct ui_event escape = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ESCAPE,
    }, 0);
    assert(ui_surface_dispatch(&surface, &escape));
    assert(calls == 1);
    ui_surface_set_default_focus(&surface, content);
    assert(ui_surface_dispatch(&surface, &escape));
    assert(calls == 2);
    assert(ui_widget_event(content, &escape));
    assert(calls == 3);
    ui_surface_focus(&surface, content);
    assert(ui_surface_focused(&surface) == content);
    assert(ui_surface_dispatch(&surface, &escape));
    assert(calls == 4);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}


static void view_update_keeps_keyed_widgets_test(void)
{
    struct ui_button *apply = NULL;
    struct ui_label *first = NULL;
    struct ui_node_desc items[3] = {
        { .kind = UI_NODE_LABEL, .key = "first",
          .bind.label = &first,
          .props.label = { .text = "First" } },
        { .kind = UI_NODE_BUTTON, .key = "apply",
          .bind.button = &apply,
          .props.button = { .text = "Apply" } },
        { .kind = UI_NODE_LABEL, .key = "last",
          .props.label = { .text = "Last" } },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "content",
        .children = items, .child_count = 3,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 200, .y = 120 });
    ui_surface_focus(&surface, &apply->widget);
    struct ui_widget *last = ui_view_widget(view, "last");

    /* The same keys in a new order keep the widgets. */
    struct ui_node_desc reordered[3] = { items[2], items[1], items[0] };
    root.children = reordered;
    assert(ui_view_update(view, &root));
    assert(ui_view_widget(view, "first") == &first->widget);
    assert(ui_view_widget(view, "apply") == &apply->widget);
    assert(ui_view_widget(view, "last") == last);
    assert(ui_widget_child_count(ui_view_root(view)) == 3);
    assert(ui_widget_child_at(ui_view_root(view), 0) == last);
    assert(ui_widget_child_at(ui_view_root(view), 2) == &first->widget);
    assert(ui_surface_focused(&surface) == &apply->widget);

    /* A new node gets a new widget. */
    struct ui_node_desc added = {
        .kind = UI_NODE_LABEL, .key = "added",
        .props.label = { .text = "Added" },
    };
    struct ui_node_desc grown[4] = { items[2], items[1], items[0], added };
    root.children = grown;
    root.child_count = 4;
    assert(ui_view_update(view, &root));
    assert(ui_view_widget(view, "added") != NULL);
    assert(ui_widget_child_count(ui_view_root(view)) == 4);

    /* A removed node loses its widget and its handle. */
    struct ui_node_desc shrunk[2] = { items[1], items[0] };
    root.children = shrunk;
    root.child_count = 2;
    assert(ui_view_update(view, &root));
    assert(ui_view_widget(view, "last") == NULL);
    assert(ui_view_widget(view, "apply") == &apply->widget);
    assert(ui_widget_child_count(ui_view_root(view)) == 2);

    /* The new properties show on the widgets that stayed. */
    items[0].props.label.text = "Renamed";
    items[1].props.button.text = "Send";
    items[1].props.button.disabled = true;
    struct ui_node_desc updated[2] = { items[1], items[0] };
    root.children = updated;
    assert(ui_view_update(view, &root));
    assert(!strcmp(ui_label_text(first), "Renamed"));
    assert(!strcmp(ui_button_text(apply), "Send"));
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}

static void view_update_releases_removed_node_test(void)
{
    struct ui_label *note = NULL;
    struct ui_node_desc child = {
        .kind = UI_NODE_LABEL, .key = "note", .bind.label = &note,
    };
    struct ui_node_desc row = {
        .kind = UI_NODE_COLUMN, .key = "row",
        .children = &child, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&row, NULL);
    assert(view && note);
    assert(ui_view_update(view, &row));
    assert(ui_widget_child_count(ui_view_root(view)) == 1);

    /* A tree without the node releases the node and its handle. */
    row.children = NULL;
    row.child_count = 0;
    assert(ui_view_update(view, &row));
    assert(note == NULL);
    assert(ui_view_widget(view, "note") == NULL);
    assert(ui_widget_child_count(ui_view_root(view)) == 0);
    ui_view_destroy(view);
}

struct view_update_probe {
    struct ui_view *view;
    const struct ui_node_desc *next;
    int updates;
};

static bool view_update_action(struct ui_widget *source,
                               const struct ui_action *action, void *userdata)
{
    (void)source;
    struct view_update_probe *probe = userdata;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    probe->updates += ui_view_update(probe->view, probe->next) ? 1 : 0;
    return true;
}

static void view_update_from_a_handler_test(void)
{
    struct ui_label *status = NULL;
    struct ui_node_desc children[2] = {
        { .kind = UI_NODE_LABEL, .key = "status", .bind.label = &status,
          .props.label = { .text = "Idle" } },
        { .kind = UI_NODE_BUTTON, .key = "go", .action_id = 1,
          .props.button = { .text = "Go" } },
    };
    struct ui_node_desc shorter = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = &children[1], .child_count = 1,
    };
    struct view_update_probe probe = { .next = &shorter };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = children, .child_count = 2,
        .on_action = view_update_action, .action_userdata = &probe,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && status);
    probe.view = view;
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 200, .y = 120 });
    struct ui_widget *button = ui_view_widget(view, "go");
    ui_surface_focus(&surface, button);

    /* A handler updates the view while the surface is busy. The surface runs
     * the cleanup of the removed node after the dispatch ends. */
    struct ui_event enter = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 0);
    assert(ui_surface_dispatch(&surface, &enter));
    assert(probe.updates == 1);
    assert(!ui_surface_busy(&surface));
    assert(status == NULL);
    assert(ui_view_widget(view, "status") == NULL);
    assert(ui_view_widget(view, "go") == button);
    assert(ui_widget_child_count(ui_view_root(view)) == 1);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}

static void view_observer_clears_a_destroyed_widget_test(void)
{
    struct ui_button *button = NULL;
    struct ui_node_desc child = {
        .kind = UI_NODE_BUTTON, .key = "go", .bind.button = &button,
        .props.button = { .text = "Go" },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = &child, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && button);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 200, .y = 120 });

    /* Another owner destroys a widget of the view. The view drops its
     * reference, so an update and a destroy stay safe. */
    ui_widget_destroy(&button->widget);
    assert(button == NULL);
    assert(ui_view_widget(view, "go") == NULL);
    assert(ui_widget_child_count(ui_view_root(view)) == 0);

    /* The node comes back as a new widget. */
    assert(ui_view_update(view, &root));
    assert(button != NULL);
    assert(ui_view_widget(view, "go") == &button->widget);
    assert(ui_widget_child_count(ui_view_root(view)) == 1);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}

static void view_list_and_menu_node_test(void)
{
    const struct ui_list_item items[] = {
        { .label = "One", .action = 1 },
        { .label = "Two", .action = 2 },
    };
    const ui_menu_item menu_items[] = {
        { .label = "Open", .action = 10 },
        { .label = "Quit", .action = 11 },
    };
    struct ui_list *list = NULL;
    struct ui_menu *menu = NULL;
    struct ui_node_desc children[] = {
        { .kind = UI_NODE_LIST, .key = "choices", .bind.list = &list,
          .class_name = "choices",
          .props.list = { .items = items, .count = 2, .selected = 1 } },
        { .kind = UI_NODE_MENU, .key = "commands", .bind.menu = &menu,
          .props.menu = { .items = menu_items, .count = 2 } },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = children, .child_count = 2,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && list && menu);
    assert(ui_view_list(view, "choices") == list);
    assert(ui_view_menu(view, "commands") == menu);
    assert(ui_list_count(list) == 2);
    assert(ui_list_selected(list) == 1);
    assert(ui_menu_is_open(menu));
    assert(ui_menu_level_count(menu) == 1);

    /* An update with fewer items updates the list and closes the menu. */
    struct ui_node_desc shrunk = {
        .kind = UI_NODE_LIST, .key = "choices", .bind.list = &list,
        .props.list = { .items = items, .count = 1 },
    };
    children[0] = shrunk;
    children[1].props.menu.count = 0;
    assert(ui_view_update(view, &root));
    assert(ui_list_count(list) == 1);
    assert(!ui_menu_is_open(menu));
    ui_view_destroy(view);
}

/* A view frees the items of an open menu node when an update removes the
 * node, and when the view is destroyed. Leak detection is the check. */
static void view_menu_node_frees_its_items_test(void)
{
    const ui_menu_item items[] = {
        { .label = "Open", .action = 10 },
        { .label = "Quit", .action = 11 },
    };
    struct ui_menu *menu = NULL;
    struct ui_node_desc child = {
        .kind = UI_NODE_MENU, .key = "commands", .bind.menu = &menu,
        .props.menu = { .items = items, .count = 2 },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = &child, .child_count = 1,
    };
    struct ui_node_desc empty = { .kind = UI_NODE_COLUMN, .key = "root" };

    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && menu && ui_menu_is_open(menu));
    assert(ui_view_update(view, &empty));
    assert(menu == NULL);
    assert(ui_view_update(view, &root));
    assert(menu && ui_menu_is_open(menu));
    ui_view_destroy(view);
    assert(menu == NULL);
}

/* An update with no list items empties the list. */
static void view_list_update_to_empty_test(void)
{
    const struct ui_list_item items[] = {
        { .label = "One", .action = 1 },
        { .label = "Two", .action = 2 },
    };
    struct ui_list *list = NULL;
    struct ui_node_desc root = {
        .kind = UI_NODE_LIST, .key = "choices", .bind.list = &list,
        .props.list = { .items = items, .count = 2 },
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && list && ui_list_count(list) == 2);

    root.props.list.items = NULL;
    root.props.list.count = 0;
    assert(ui_view_update(view, &root));
    assert(ui_list_count(list) == 0);
    assert(ui_list_item(list, 0) == NULL);
    ui_view_destroy(view);
}

/* A failed update leaves the kept widgets as they were. A full supplied
 * container makes the update fail after the kept nodes are matched. */
static void view_failed_update_keeps_widgets_test(void)
{
    struct ui_label *label = NULL;
    struct ui_node_desc status = {
        .kind = UI_NODE_LABEL, .key = "status", .class_name = "status-label",
        .bind.label = &label, .props.label = { .text = "x" },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root", .class_name = "root-box",
        .children = &status, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && label);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 200, .y = 100 });
    ui_surface_prepare(&surface);

    struct ui_widget full = {0}, old_child = {0};
    ui_widget_init(&full, &SINGLE_CHILD_OPS, NULL);
    ui_widget_init(&old_child, NULL, NULL);
    assert(ui_widget_add_child(&full, &old_child));
    struct ui_node_desc inner = {
        .kind = UI_NODE_LABEL, .key = "inner", .props.label = { .text = "y" },
    };
    struct ui_node_desc changed = status;
    changed.class_name = "changed-label";
    changed.props.label.text = "changed";
    struct ui_node_desc kids[] = {
        changed,
        { .kind = UI_NODE_WIDGET, .key = "full", .props.widget = &full,
          .children = &inner, .child_count = 1 },
    };
    struct ui_node_desc bad = root;
    bad.class_name = "changed-box";
    bad.children = kids;
    bad.child_count = 2;
    struct ui_view_diagnostic diagnostic;
    assert(!ui_view_update_ex(view, &bad, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_INVALID_CHILDREN);
    assert(diagnostic.node == &inner);

    ui_surface_prepare(&surface);
    struct ui_widget *view_root = ui_view_root(view);
    assert(ui_debug_find(view_root, ".root-box") == view_root);
    assert(ui_debug_find(view_root, ".status-label") == &label->widget);
    assert(ui_debug_find(view_root, "@status") == &label->widget);
    assert(!strcmp(ui_label_text(label), "x"));
    assert(ui_view_widget(view, "full") == NULL);
    assert(!full.parent && ui_widget_child_count(&full) == 1);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
    ui_widget_destroy(&full);
}

/* A menu handler that updates or destroys the view when the menu closes. */
struct menu_close_probe {
    struct ui_view *view;
    const struct ui_node_desc *next;
    bool destroy;
    int closes;
    bool nested_ok;
    struct ui_view_diagnostic nested;
};

static bool menu_close_action(struct ui_widget *source,
                              const struct ui_action *action, void *userdata)
{
    (void)source;
    struct menu_close_probe *probe = userdata;
    if (action->kind != UI_ACTION_CLOSE)
        return true;
    probe->closes++;
    struct ui_view *view = probe->view;
    if (!view)
        return true;
    if (probe->destroy) {
        probe->view = NULL;
        ui_view_destroy(view);
    } else {
        probe->nested_ok = ui_view_update_ex(view, probe->next, &probe->nested);
    }
    return true;
}

static const ui_menu_item CLOSE_PROBE_ITEMS[] = {
    { .label = "Open", .action = 10 },
    { .label = "More", .action = 12, .flags = UI_LIST_ITEM_SUBMENU },
};

static const ui_menu_item CLOSE_PROBE_SUBMENU[] = {
    { .label = "Alpha", .action = 21 },
};

static const ui_menu_item *close_probe_submenu(void *userdata, int action,
                                               int *n)
{
    (void)userdata;
    *n = action == 12 ? 1 : 0;
    return action == 12 ? CLOSE_PROBE_SUBMENU : NULL;
}

/* A handler that runs during an update cannot update the view again. The
 * menu sends CLOSE when an update closes it. */
static void view_update_from_an_update_test(void)
{
    struct ui_menu *menu = NULL;
    struct menu_close_probe probe = {0};
    struct ui_node_desc child = {
        .kind = UI_NODE_MENU, .key = "commands", .bind.menu = &menu,
        .on_action = menu_close_action, .action_userdata = &probe,
        .props.menu = { .items = CLOSE_PROBE_ITEMS, .count = 2 },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = &child, .child_count = 1,
    };
    struct ui_node_desc other = {
        .kind = UI_NODE_LABEL, .key = "status", .props.label = { .text = "x" },
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && menu && ui_menu_is_open(menu));
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 300, .y = 200 });
    ui_surface_prepare(&surface);
    probe.view = view;
    probe.next = &other;

    child.props.menu.count = 0;
    assert(ui_view_update(view, &root));
    assert(probe.closes == 1);
    assert(!probe.nested_ok);
    assert(probe.nested.error == UI_VIEW_ERROR_BUSY);
    assert(menu && !ui_menu_is_open(menu));
    assert(ui_view_menu(view, "commands") == menu);
    assert(ui_view_widget(view, "status") == NULL);
    ui_surface_prepare(&surface);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}

/* A handler that destroys the view during an update destroys it when the
 * update ends. The update then fails. */
static void view_destroy_from_an_update_test(void)
{
    struct ui_menu *menu = NULL;
    struct menu_close_probe probe = { .destroy = true };
    struct ui_node_desc child = {
        .kind = UI_NODE_MENU, .key = "commands", .bind.menu = &menu,
        .on_action = menu_close_action, .action_userdata = &probe,
        .props.menu = { .items = CLOSE_PROBE_ITEMS, .count = 2 },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = &child, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && menu);
    probe.view = view;

    child.props.menu.count = 0;
    struct ui_view_diagnostic diagnostic;
    assert(!ui_view_update_ex(view, &root, &diagnostic));
    assert(diagnostic.error == UI_VIEW_ERROR_BUSY);
    assert(probe.closes == 1);
    assert(probe.view == NULL);
    assert(menu == NULL);
}

/* An update with the same items keeps an open menu and its submenu. */
static void view_update_keeps_an_open_menu_test(void)
{
    struct ui_menu *menu = NULL;
    struct menu_close_probe probe = {0};
    struct ui_node_desc child = {
        .kind = UI_NODE_MENU, .key = "commands", .bind.menu = &menu,
        .on_action = menu_close_action, .action_userdata = &probe,
        .props.menu = { .items = CLOSE_PROBE_ITEMS, .count = 2,
                        .on_submenu = close_probe_submenu },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = &child, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && menu);
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 300, .y = 200 });
    ui_surface_prepare(&surface);
    struct ui_widget *top = ui_menu_level_widget(menu, 0);
    assert(top && ui_widget_surface(top) == &surface);
    ui_surface_focus(&surface, top);
    const enum ui_key keys[] = { UI_KEY_END, UI_KEY_RIGHT };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        struct ui_event key = ui_event_key((struct ui_key_event){
            .key = keys[i],
        }, 0);
        ui_surface_dispatch(&surface, &key);
    }
    assert(ui_menu_level_count(menu) == 2);

    assert(ui_view_update(view, &root));
    assert(probe.closes == 0);
    assert(ui_menu_is_open(menu));
    assert(ui_menu_level_count(menu) == 2);

    /* Another submenu provider closes the submenu, and the top level stays. */
    int other_userdata = 0;
    child.props.menu.submenu_userdata = &other_userdata;
    assert(ui_view_update(view, &root));
    assert(probe.closes == 0);
    assert(ui_menu_is_open(menu));
    assert(ui_menu_level_count(menu) == 1);

    /* Other items open the menu again. */
    child.props.menu.count = 1;
    assert(ui_view_update(view, &root));
    assert(probe.closes == 1);
    assert(ui_menu_is_open(menu));
    assert(ui_menu_level_count(menu) == 1);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
}

/* A view updates a supplied widget that it shows. The widget can also move
 * to another parent, and the view keeps it alive. */
static void view_update_keeps_a_supplied_widget_test(void)
{
    int destroyed = 0;
    struct supplied_control control = { .destroyed = &destroyed };
    ui_widget_init(&control.widget, &SUPPLIED_CONTROL_OPS, NULL);
    struct ui_node_desc custom = {
        .kind = UI_NODE_WIDGET, .key = "custom",
        .props.widget = &control.widget,
    };
    struct ui_node_desc boxes[] = {
        { .kind = UI_NODE_ROW, .key = "a", .children = &custom,
          .child_count = 1 },
        { .kind = UI_NODE_ROW, .key = "b" },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root",
        .children = boxes, .child_count = 2,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);
    struct ui_widget *a = ui_view_widget(view, "a");
    struct ui_widget *b = ui_view_widget(view, "b");
    struct ui_surface surface;
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 200, .y = 100 });
    ui_surface_prepare(&surface);

    /* The same descriptor updates the shown widget. */
    assert(ui_view_update(view, &root));
    assert(control.widget.parent == a);
    assert(ui_view_widget(view, "custom") == &control.widget);

    /* A failed move leaves the widget in its parent. */
    struct ui_widget full = {0}, old_child = {0};
    ui_widget_init(&full, &SINGLE_CHILD_OPS, NULL);
    ui_widget_init(&old_child, NULL, NULL);
    assert(ui_widget_add_child(&full, &old_child));
    struct ui_node_desc inner = { .kind = UI_NODE_LABEL, .key = "inner" };
    struct ui_node_desc moved[] = {
        { .kind = UI_NODE_ROW, .key = "a" },
        { .kind = UI_NODE_ROW, .key = "b", .children = &custom,
          .child_count = 1 },
        { .kind = UI_NODE_WIDGET, .key = "full", .props.widget = &full,
          .children = &inner, .child_count = 1 },
    };
    struct ui_node_desc bad = root;
    bad.children = moved;
    bad.child_count = 3;
    assert(!ui_view_update(view, &bad));
    assert(control.widget.parent == a);
    assert(ui_widget_surface(&control.widget) == &surface);
    assert(!destroyed);
    ui_widget_destroy(&full);

    /* The widget moves to another parent and stays alive. */
    root.children = moved;
    root.child_count = 2;
    assert(ui_view_update(view, &root));
    assert(control.widget.parent == b);
    assert(ui_view_widget(view, "custom") == &control.widget);
    assert(!destroyed);
    ui_surface_prepare(&surface);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
    assert(destroyed == 1);

    /* A supplied root on a surface updates too. */
    destroyed = 0;
    ui_widget_init(&control.widget, &SUPPLIED_CONTROL_OPS, NULL);
    view = ui_view_mount(&custom, NULL);
    assert(view && ui_view_root(view) == &control.widget);
    show(&surface, ui_view_root(view), (struct vector2i){ .x = 100, .y = 100 });
    assert(ui_view_update(view, &custom));
    assert(ui_view_root(view) == &control.widget);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
    assert(destroyed == 1);
}

/* Supplied widgets that count their destroy calls. */
struct supplied_set {
    int destroyed[4];
    struct supplied_control w[4];
};

static void supplied_set_init(struct supplied_set *set)
{
    for (int i = 0; i < 4; i++) {
        set->destroyed[i] = 0;
        set->w[i] = (struct supplied_control){ .destroyed = &set->destroyed[i] };
        ui_widget_init(&set->w[i].widget, &SUPPLIED_CONTROL_OPS, NULL);
    }
}

static int supplied_set_destroyed(const struct supplied_set *set)
{
    int total = 0;
    for (int i = 0; i < 4; i++)
        total += set->destroyed[i];
    return total;
}

/* An update can rearrange the supplied widgets in any order. A failed
 * update leaves them where they were, and destroys none of them. */
static void view_update_rearranges_supplied_widgets_test(void)
{
    struct supplied_set set;
    supplied_set_init(&set);
    struct ui_widget *p = &set.w[0].widget;
    struct ui_widget *c = &set.w[1].widget;
    struct ui_widget *q = &set.w[2].widget;

    /* Now root -> q -> p and root -> c. */
    struct ui_node_desc p0 = { .kind = UI_NODE_WIDGET, .key = "p", .props.widget = p };
    struct ui_node_desc k0[] = {
        { .kind = UI_NODE_WIDGET, .key = "q", .props.widget = q,
          .children = &p0, .child_count = 1 },
        { .kind = UI_NODE_WIDGET, .key = "c", .props.widget = c },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root", .children = k0, .child_count = 2,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);
    struct ui_widget *top = ui_view_root(view);
    struct ui_surface surface;
    show(&surface, top, (struct vector2i){ .x = 200, .y = 100 });

    /* Then root -> p -> c -> n -> q, with a full container that fails. */
    struct ui_node_desc q1 = { .kind = UI_NODE_WIDGET, .key = "q", .props.widget = q };
    struct ui_node_desc n1 = {
        .kind = UI_NODE_ROW, .key = "n", .children = &q1, .child_count = 1,
    };
    struct ui_node_desc c1 = {
        .kind = UI_NODE_WIDGET, .key = "c", .props.widget = c,
        .children = &n1, .child_count = 1,
    };
    struct ui_widget full = {0}, old_child = {0};
    ui_widget_init(&full, &SINGLE_CHILD_OPS, NULL);
    ui_widget_init(&old_child, NULL, NULL);
    assert(ui_widget_add_child(&full, &old_child));
    struct ui_node_desc inner = { .kind = UI_NODE_LABEL, .key = "inner" };
    struct ui_node_desc k1[] = {
        { .kind = UI_NODE_WIDGET, .key = "p", .props.widget = p,
          .children = &c1, .child_count = 1 },
        { .kind = UI_NODE_WIDGET, .key = "full", .props.widget = &full,
          .children = &inner, .child_count = 1 },
    };
    struct ui_node_desc next = root;
    next.children = k1;
    next.child_count = 2;
    assert(!ui_view_update(view, &next));
    assert(q->parent == top && p->parent == q && c->parent == top);
    assert(ui_widget_child_at(top, 0) == q && ui_widget_child_at(top, 1) == c);
    assert(!full.parent);
    assert(supplied_set_destroyed(&set) == 0);
    ui_widget_destroy(&full);

    /* Without the full container the same move succeeds. */
    next.child_count = 1;
    assert(ui_view_update(view, &next));
    struct ui_widget *n = ui_view_widget(view, "n");
    assert(p->parent == top && c->parent == p && n->parent == c &&
           q->parent == n);
    assert(supplied_set_destroyed(&set) == 0);

    /* root -> w1 -> w2 -> w3 becomes root -> w3 -> w1 and root -> w2. */
    struct ui_node_desc d3 = { .kind = UI_NODE_WIDGET, .key = "w3", .props.widget = q };
    struct ui_node_desc d2 = {
        .kind = UI_NODE_WIDGET, .key = "w2", .props.widget = c,
        .children = &d3, .child_count = 1,
    };
    struct ui_node_desc d1 = {
        .kind = UI_NODE_WIDGET, .key = "w1", .props.widget = p,
        .children = &d2, .child_count = 1,
    };
    root = (struct ui_node_desc){
        .kind = UI_NODE_COLUMN, .key = "root", .children = &d1, .child_count = 1,
    };
    assert(ui_view_update(view, &root));
    assert(p->parent == top && c->parent == p && q->parent == c);
    struct ui_node_desc e1 = { .kind = UI_NODE_WIDGET, .key = "w1", .props.widget = p };
    struct ui_node_desc e[] = {
        { .kind = UI_NODE_WIDGET, .key = "w3", .props.widget = q,
          .children = &e1, .child_count = 1 },
        { .kind = UI_NODE_WIDGET, .key = "w2", .props.widget = c },
    };
    root.children = e;
    root.child_count = 2;
    assert(ui_view_update(view, &root));
    assert(q->parent == top && p->parent == q && c->parent == top);
    assert(ui_widget_child_at(top, 0) == q && ui_widget_child_at(top, 1) == c);
    assert(supplied_set_destroyed(&set) == 0);
    ui_surface_prepare(&surface);
    ui_surface_destroy(&surface);
    ui_view_destroy(view);
    assert(set.destroyed[0] == 1 && set.destroyed[1] == 1 &&
           set.destroyed[2] == 1);
    ui_widget_destroy(&set.w[3].widget);
}

/* A supplied widget that becomes the view root leaves its old parent, so
 * the retired parent does not take it along. */
static void view_update_makes_a_supplied_widget_the_root_test(void)
{
    int destroyed = 0;
    struct supplied_control control = { .destroyed = &destroyed };
    ui_widget_init(&control.widget, &SUPPLIED_CONTROL_OPS, NULL);
    struct ui_node_desc w = {
        .kind = UI_NODE_WIDGET, .key = "w", .props.widget = &control.widget,
    };
    struct ui_node_desc row = {
        .kind = UI_NODE_ROW, .key = "c", .children = &w, .child_count = 1,
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root", .children = &row, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);

    assert(ui_view_update(view, &w));
    assert(ui_view_root(view) == &control.widget);
    assert(!control.widget.parent);
    assert(!destroyed);

    assert(ui_view_update(view, &root));
    assert(control.widget.parent == ui_view_widget(view, "c"));
    assert(!destroyed);
    ui_view_destroy(view);
    assert(destroyed == 1);
}

/* The child limit counts the children that the update plans. A child that
 * leaves or that the update retires does not count. */
static void view_update_counts_only_planned_children_test(void)
{
    /* A supplied single-child container gives its child to the root and
     * takes a new one. */
    struct supplied_set set;
    supplied_set_init(&set);
    struct ui_widget box = {0};
    ui_widget_init(&box, &SINGLE_CHILD_OPS, NULL);
    struct ui_node_desc w = {
        .kind = UI_NODE_WIDGET, .key = "w", .props.widget = &set.w[0].widget,
    };
    struct ui_node_desc k0[] = {
        { .kind = UI_NODE_WIDGET, .key = "box", .props.widget = &box,
          .children = &w, .child_count = 1 },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "root", .children = k0, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view);
    struct ui_node_desc label = { .kind = UI_NODE_LABEL, .key = "label" };
    struct ui_node_desc k1[] = {
        { .kind = UI_NODE_WIDGET, .key = "box", .props.widget = &box,
          .children = &label, .child_count = 1 },
        w,
    };
    root.children = k1;
    root.child_count = 2;
    assert(ui_view_update(view, &root));
    assert(set.w[0].widget.parent == ui_view_root(view));
    assert(ui_widget_child_at(&box, 0) == ui_view_widget(view, "label"));
    ui_view_destroy(view);
    assert(set.destroyed[0] == 1);
    for (int i = 1; i < 4; i++)
        ui_widget_destroy(&set.w[i].widget);

    /* A scrollview takes content of another kind. */
    struct ui_node_desc a = { .kind = UI_NODE_LABEL, .props.label = { .text = "a" } };
    struct ui_node_desc scroll = {
        .kind = UI_NODE_SCROLLVIEW, .key = "sv", .children = &a, .child_count = 1,
    };
    view = ui_view_mount(&scroll, NULL);
    assert(view);
    struct ui_node_desc b = { .kind = UI_NODE_BUTTON, .key = "b",
                              .props.button = { .text = "b" } };
    scroll.children = &b;
    assert(ui_view_update(view, &scroll));
    assert(ui_view_widget(view, "b")->parent == ui_view_root(view));
    ui_view_destroy(view);
}

int main(void)
{
    view_diagnostic_test();
    view_children_macro_test();
    view_dropdown_overlay_test();
    view_supplied_widget_test();
    view_supplied_widget_validation_test();
    view_supplied_container_test();
    view_supplied_root_with_popup_test();
    view_update_keeps_keyed_widgets_test();
    view_update_releases_removed_node_test();
    view_update_from_a_handler_test();
    view_observer_clears_a_destroyed_widget_test();
    view_list_and_menu_node_test();
    view_menu_node_frees_its_items_test();
    view_list_update_to_empty_test();
    view_failed_update_keeps_widgets_test();
    view_update_from_an_update_test();
    view_destroy_from_an_update_test();
    view_update_keeps_an_open_menu_test();
    view_update_keeps_a_supplied_widget_test();
    view_update_rearranges_supplied_widgets_test();
    view_update_makes_a_supplied_widget_the_root_test();
    view_update_counts_only_planned_children_test();
    view_root_keyboard_test();
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
