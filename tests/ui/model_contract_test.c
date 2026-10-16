
#include <assert.h>
#include <string.h>

#include "ui/ui.h"
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

int main(void)
{
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
