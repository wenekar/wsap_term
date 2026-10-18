#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui/surface.h"
#include "ui/internal/tree.h"
#include "ui/widget.h"

struct probe {
    int measures;
    int layouts;
    int events;
    int ticks;
    int destroys;
    int focus_out;
    bool animate;
    bool handle_event;
    struct ui_widget *destroy_in_event;
    struct ui_widget *remove_in_event;
    struct ui_widget *remove_on_focus_out;
    struct ui_widget *destroy_in_tick;
    struct rect2i bounds;
};

static unsigned widget_invalidation(const struct ui_widget *widget)
{
    return widget->impl.invalidation;
}

static struct vector2i measure(const struct ui_widget *widget,
                               struct ui_constraints constraints)
{
    struct probe *probe = widget->userdata;
    probe->measures++;
    for (struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling)
        ui_widget_measure(child, constraints);
    return (struct vector2i){ .x = 100, .y = 30 };
}

static void layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct probe *probe = widget->userdata;
    probe->layouts++;
    probe->bounds = bounds;
    for (struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling)
        ui_widget_place(child, bounds);
}

static bool event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe *probe = widget->userdata;
    probe->events++;
    if (event && event->kind == UI_EVENT_FOCUS_OUT)
        probe->focus_out++;
    if (probe->destroy_in_event) {
        ui_widget_destroy(probe->destroy_in_event);
        probe->destroy_in_event = NULL;
    }
    if (probe->remove_in_event) {
        struct ui_widget *victim = probe->remove_in_event;
        probe->remove_in_event = NULL;
        if (victim->parent)
            ui_widget_remove_child(victim->parent, victim);
    }
    if (event && event->kind == UI_EVENT_FOCUS_OUT &&
        probe->remove_on_focus_out) {
        struct ui_widget *victim = probe->remove_on_focus_out;
        probe->remove_on_focus_out = NULL;
        if (victim->parent)
            ui_widget_remove_child(victim->parent, victim);
    }
    return probe->handle_event && event && event->kind == UI_EVENT_KEY;
}

static uint64_t tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct probe *probe = widget->userdata;
    probe->ticks++;
    if (probe->destroy_in_tick) {
        ui_widget_destroy(probe->destroy_in_tick);
        probe->destroy_in_tick = NULL;
    }
    return probe->animate ? now_ms : 0;
}

static void destroy(struct ui_widget *widget)
{
    struct probe *probe = widget->userdata;
    probe->destroys++;
}

static const struct ui_widget_ops OPS = {
    .measure = measure,
    .layout = layout,
    .event = event,
    .tick = tick,
    .destroy = destroy,
};

static void widget_lifecycle_test(void)
{
    struct probe probe = { .handle_event = true };
    struct ui_widget widget = {0};
    ui_widget_init(&widget, &OPS, &probe);
    assert(widget_invalidation(&widget) ==
           (UI_WIDGET_INVALIDATE_MEASURE |
            UI_WIDGET_INVALIDATE_LAYOUT |
            UI_WIDGET_INVALIDATE_PAINT));

    struct vector2i size = ui_widget_measure(&widget, (struct ui_constraints){
        .min = { .x = 10, .y = 5 },
        .max = { .x = 80, .y = 20 },
    });
    assert(size.x == 80 && size.y == 20);
    assert(!(widget_invalidation(&widget) & UI_WIDGET_INVALIDATE_MEASURE));

    struct rect2i bounds = {
        .pos = { .x = 3, .y = 4 },
        .size = { .x = 50, .y = 60 },
    };
    ui_widget_layout(&widget, bounds);
    assert(probe.layouts == 1);
    assert(widget.bounds.x == 3 && widget.bounds.height == 60);
    assert(!(widget_invalidation(&widget) &
             (UI_WIDGET_INVALIDATE_MEASURE | UI_WIDGET_INVALIDATE_LAYOUT)));
    assert(widget_invalidation(&widget) & UI_WIDGET_INVALIDATE_PAINT);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 42);
    assert(ui_widget_event(&widget, &key));
    assert(probe.events == 1);

    probe.animate = true;
    assert(ui_widget_tick(&widget, 42));
    assert(probe.ticks == 1);

    ui_widget_destroy(&widget);
    assert(probe.destroys == 1);
}

static void context_clock_test(void)
{
    struct probe probe = { .animate = true, .handle_event = true };
    struct ui_widget widget = {0};
    ui_widget_init(&widget, &OPS, &probe);

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &widget);
    assert(ui_surface_frame_pending(&context));
    assert(ui_surface_take_frame_request(&context));
    ui_surface_resize(&context, (struct vector2i){ .x = 640, .y = 480 });
    assert(ui_surface_frame_pending(&context));
    assert(ui_surface_take_frame_request(&context));
    assert(probe.layouts == 1);
    assert(probe.bounds.width == 640 && probe.bounds.height == 480);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 10);
    assert(ui_surface_dispatch(&context, &key));
    assert(!ui_surface_frame_pending(&context));

    assert(ui_surface_tick(&context, 10));
    assert(ui_surface_frame_pending(&context));
    assert(ui_surface_take_frame_request(&context));
    assert(!ui_surface_frame_pending(&context));

    ui_surface_request_frame(&context);
    assert(ui_surface_take_frame_request(&context));
    assert(!ui_surface_take_frame_request(&context));
    ui_surface_destroy(&context);
}

static void hierarchy_test(void)
{
    struct probe parent_probe = { .handle_event = true };
    struct probe first_probe = {0};
    struct probe second_probe = { .animate = true, .handle_event = true };
    struct ui_widget parent = {0};
    struct ui_widget first = {0};
    struct ui_widget second = {0};
    ui_widget_init(&parent, &OPS, &parent_probe);
    ui_widget_init(&first, &OPS, &first_probe);
    ui_widget_init(&second, &OPS, &second_probe);

    assert(ui_widget_add_child(&parent, &first));
    assert(ui_widget_add_child(&parent, &second));
    assert(!ui_widget_add_child(&parent, &second));
    assert(!ui_widget_add_child(&first, &parent));
    assert(ui_widget_child_count(&parent) == 2);
    assert(ui_widget_child_at(&parent, 0) == &first);
    assert(ui_widget_child_at(&parent, 1) == &second);
    assert(ui_widget_child_at(&parent, 2) == NULL);
    assert(first.parent == &parent && second.parent == &parent);

    struct ui_constraints constraints = {
        .min = { .x = 1, .y = 1 },
        .max = { .x = 200, .y = 100 },
    };
    ui_widget_measure(&parent, constraints);
    assert(parent_probe.measures == 1);
    assert(first_probe.measures == 1 && second_probe.measures == 1);

    struct rect2i bounds = {
        .pos = { .x = 5, .y = 6 },
        .size = { .x = 70, .y = 30 },
    };
    first.bounds = bounds;
    second.bounds = bounds;
    ui_widget_layout(&parent, bounds);
    assert(parent_probe.layouts == 1);
    assert(first_probe.layouts == 1 && second_probe.layouts == 1);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 10);
    assert(ui_widget_bubble(&first, &key));
    assert(parent_probe.events == 1);
    assert(first_probe.events == 1 && second_probe.events == 0);

    assert(ui_widget_tick(&parent, 10));
    assert(parent_probe.ticks == 1);
    assert(first_probe.ticks == 1 && second_probe.ticks == 1);

    ui_widget_invalidate(&second, UI_WIDGET_INVALIDATE_PAINT);
    assert(widget_invalidation(&second) & UI_WIDGET_INVALIDATE_PAINT);
    assert(widget_invalidation(&parent) & UI_WIDGET_INVALIDATE_PAINT);

    assert(ui_widget_remove_child(&parent, &first));
    assert(first.parent == NULL && ui_widget_child_count(&parent) == 1);
    assert(!ui_widget_remove_child(&parent, &first));
    ui_widget_destroy(&second);
    assert(second.parent == NULL);
    assert(ui_widget_child_count(&parent) == 0);
    assert(second_probe.destroys == 1);
    ui_widget_destroy(&parent);
    assert(parent_probe.destroys == 1);
    assert(first_probe.destroys == 0);
    ui_widget_destroy(&parent);
    assert(parent_probe.destroys == 1);

    ui_widget_destroy(&first);
    assert(first_probe.destroys == 1);
}

static void ctx_propagation_test(void)
{
    struct probe root_probe = {0}, mid_probe = {0}, leaf_probe = {0};
    struct ui_widget root = {0}, mid = {0}, leaf = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&mid, &OPS, &mid_probe);
    ui_widget_init(&leaf, &OPS, &leaf_probe);

    assert(ui_widget_add_child(&mid, &leaf));
    assert(ui_widget_surface(&mid) == NULL && ui_widget_surface(&leaf) == NULL);

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    assert(ui_widget_surface(&root) == &context);

    assert(ui_widget_add_child(&root, &mid));
    assert(ui_widget_surface(&mid) == &context);
    assert(ui_widget_surface(&leaf) == &context);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void zero_init_test(void)
{
    struct ui_widget widget = {0};
    struct ui_surface context = {0};
    struct pointer_event pointer = {0};
    struct ui_event event = {0};
    struct ui_constraints constraints = {0};

    assert(pointer.kind == PTR_NONE);
    assert(event.kind == UI_EVENT_NONE);
    assert(ui_constraints_clamp(constraints, (struct vector2i){0}).x == 0);
    assert(widget_invalidation(&widget) == UI_WIDGET_INVALIDATE_NONE);
    assert(ui_widget_measure(&widget, constraints).x == 0);
    assert(!ui_widget_event(&widget, NULL));
    assert(!ui_widget_tick(&widget, 0));
    assert(!ui_surface_dispatch(&context, NULL));
    assert(!ui_surface_tick(&context, 0));
    assert(!ui_surface_frame_pending(&context));
    assert(!ui_surface_take_frame_request(&context));
    assert(ui_surface_next_deadline(&context) == 0);
    ui_surface_destroy(&context);
}

static void destroy_parent_during_dispatch_test(void)
{
    struct probe root_probe = {0};
    struct probe child_probe = {0};
    struct probe sibling_probe = {0};
    struct ui_widget root = {0}, child = {0}, sibling = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&child, &OPS, &child_probe);
    ui_widget_init(&sibling, &OPS, &sibling_probe);
    assert(ui_widget_add_child(&root, &child));
    assert(ui_widget_add_child(&root, &sibling));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    child_probe.destroy_in_event = &root;

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 1);
    assert(!ui_widget_bubble(&child, &key));
    assert(root_probe.events == 0);
    assert(child_probe.events == 1);
    assert(sibling_probe.events == 0);
    assert(root.impl.destroyed && child.impl.destroyed && sibling.impl.destroyed);
    assert(root_probe.destroys == 1 && child_probe.destroys == 1 &&
           sibling_probe.destroys == 1);
    ui_surface_destroy(&context);
}

static void detach_mutation_robust_test(void)
{
    struct probe root_probe = {0}, trigger_probe = {0};
    struct probe a = {0}, b = {0}, c = {0};
    struct ui_widget root = {0}, parent = {0}, trigger = {0};
    struct ui_widget wa = {0}, wb = {0}, wc = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&parent, NULL, NULL);
    ui_widget_init(&trigger, &OPS, &trigger_probe);
    ui_widget_init(&wa, &OPS, &a);
    ui_widget_init(&wb, &OPS, &b);
    ui_widget_init(&wc, &OPS, &c);
    assert(ui_widget_add_child(&parent, &wa));
    assert(ui_widget_add_child(&parent, &wb));
    assert(ui_widget_add_child(&parent, &wc));
    assert(ui_widget_add_child(&root, &parent));
    assert(ui_widget_add_child(&root, &trigger));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_surface_focus(&context, &wb);

    trigger_probe.remove_in_event = &parent;
    b.remove_on_focus_out = &wa;

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 1);
    assert(!ui_widget_bubble(&trigger, &key));

    assert(b.focus_out == 1);
    assert(wa.parent == NULL);
    assert(ui_widget_surface(&parent) == NULL && ui_widget_surface(&trigger) == &context);
    assert(ui_widget_surface(&wa) == NULL && ui_widget_surface(&wb) == NULL && ui_widget_surface(&wc) == NULL);
    assert(ui_surface_focused(&context) == NULL);

    ui_widget_destroy(&wa);
    ui_widget_destroy(&wb);
    ui_widget_destroy(&wc);
    ui_widget_destroy(&parent);
    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void tick_destroyed_parent_test(void)
{
    struct probe parent_probe = {0};
    struct probe child_probe = {0};
    struct probe sibling_probe = {0};
    struct ui_widget parent = {0}, child = {0}, sibling = {0};
    ui_widget_init(&parent, &OPS, &parent_probe);
    ui_widget_init(&child, &OPS, &child_probe);
    ui_widget_init(&sibling, &OPS, &sibling_probe);
    assert(ui_widget_add_child(&parent, &child));
    assert(ui_widget_add_child(&parent, &sibling));

    child_probe.destroy_in_tick = &parent;
    assert(!ui_widget_tick(&parent, 10));
    assert(parent_probe.ticks == 1);
    assert(child_probe.ticks == 1 && sibling_probe.ticks == 0);
    assert(parent.impl.destroyed && child.impl.destroyed && sibling.impl.destroyed);
}

struct typed_widget_probe {
    struct ui_widget widget;
    int value;
};

static void add_children_test(void)
{
    struct typed_widget_probe parent_probe = {0};
    struct typed_widget_probe child_a = {0};
    struct typed_widget_probe child_b = {0};
    struct typed_widget_probe child_c = {0};

    assert(UI_WIDGET(&parent_probe) == &parent_probe.widget);
    ui_widget_init(UI_WIDGET(&parent_probe), NULL, NULL);
    ui_widget_init(UI_WIDGET(&child_a), NULL, NULL);
    ui_widget_init(UI_WIDGET(&child_b), NULL, NULL);
    ui_widget_init(UI_WIDGET(&child_c), NULL, NULL);

    struct ui_widget *items[] = {
        UI_WIDGET(&child_a),
        UI_WIDGET(&child_b),
        UI_WIDGET(&child_c),
    };
    assert(ui_widget_add_children(UI_WIDGET(&parent_probe), items, 3));
    assert(ui_widget_child_count(UI_WIDGET(&parent_probe)) == 3);
    assert(parent_probe.widget.first_child == UI_WIDGET(&child_a));
    assert(child_a.widget.next_sibling == UI_WIDGET(&child_b));
    assert(child_b.widget.next_sibling == UI_WIDGET(&child_c));
    assert(child_c.widget.next_sibling == NULL);
    assert(parent_probe.widget.last_child == UI_WIDGET(&child_c));
    assert(child_a.widget.parent == UI_WIDGET(&parent_probe));
    assert(child_b.widget.parent == UI_WIDGET(&parent_probe));
    assert(child_c.widget.parent == UI_WIDGET(&parent_probe));

    assert(ui_widget_add_children(UI_WIDGET(&parent_probe), NULL, 0));
    assert(ui_widget_child_count(UI_WIDGET(&parent_probe)) == 3);

    struct ui_widget extra = {0};
    ui_widget_init(&extra, NULL, NULL);
    assert(!ui_widget_add_children(NULL, (struct ui_widget *[]){ &extra }, 1));

    struct ui_widget dead_parent = {0};
    ui_widget_init(&dead_parent, NULL, NULL);
    ui_widget_destroy(&dead_parent);
    assert(!ui_widget_add_children(&dead_parent, (struct ui_widget *[]){ &extra }, 1));

    assert(!ui_widget_add_children(UI_WIDGET(&parent_probe), NULL, 2));

    struct ui_widget p2 = {0}, c1 = {0}, c2 = {0};
    ui_widget_init(&p2, NULL, NULL);
    ui_widget_init(&c1, NULL, NULL);
    ui_widget_init(&c2, NULL, NULL);
    assert(!ui_widget_add_children(&p2, (struct ui_widget *[]){ &c1, NULL, &c2 }, 3));
    assert(ui_widget_child_count(&p2) == 0);
    assert(c1.parent == NULL && c2.parent == NULL);

    assert(!ui_widget_add_children(&p2, (struct ui_widget *[]){ &p2 }, 1));
    assert(ui_widget_child_count(&p2) == 0);

    assert(!ui_widget_add_children(&p2, (struct ui_widget *[]){ &c1, &c2, &c1 }, 3));
    assert(ui_widget_child_count(&p2) == 0);
    assert(c1.parent == NULL && c2.parent == NULL);

    assert(!ui_widget_add_children(&p2, (struct ui_widget *[]){ &c1, UI_WIDGET(&child_a) }, 2));
    assert(ui_widget_child_count(&p2) == 0);
    assert(c1.parent == NULL);

    struct ui_widget dead_child = {0};
    ui_widget_init(&dead_child, NULL, NULL);
    ui_widget_destroy(&dead_child);
    assert(!ui_widget_add_children(&p2, (struct ui_widget *[]){ &c1, &dead_child }, 2));
    assert(ui_widget_child_count(&p2) == 0);
    assert(c1.parent == NULL);

    static const struct ui_widget_ops LIMITED_OPS = { .child_limit = 2 };
    struct ui_widget limited = {0}, lim_a = {0}, lim_b = {0}, lim_c = {0};
    ui_widget_init(&limited, &LIMITED_OPS, NULL);
    ui_widget_init(&lim_a, NULL, NULL);
    ui_widget_init(&lim_b, NULL, NULL);
    ui_widget_init(&lim_c, NULL, NULL);
    assert(!ui_widget_add_children(&limited, (struct ui_widget *[]){ &lim_a, &lim_b, &lim_c }, 3));
    assert(ui_widget_child_count(&limited) == 0);
    assert(lim_a.parent == NULL && lim_b.parent == NULL && lim_c.parent == NULL);

    assert(ui_widget_add_children(&limited, (struct ui_widget *[]){ &lim_a, &lim_b }, 2));
    assert(ui_widget_child_count(&limited) == 2);
    assert(!ui_widget_add_children(&limited, (struct ui_widget *[]){ &lim_c }, 1));
    assert(ui_widget_child_count(&limited) == 2);
    assert(lim_c.parent == NULL);

    assert(!ui_widget_add_children(UI_WIDGET(&child_a), (struct ui_widget *[]){ UI_WIDGET(&parent_probe) }, 1));

    ui_widget_destroy(&limited);
    ui_widget_destroy(&lim_c);
    ui_widget_destroy(&p2);
    ui_widget_destroy(&c1);
    ui_widget_destroy(&c2);
    ui_widget_destroy(&extra);
    ui_widget_destroy(UI_WIDGET(&parent_probe));
}

int main(void)
{
    widget_lifecycle_test();
    context_clock_test();
    hierarchy_test();
    ctx_propagation_test();
    destroy_parent_during_dispatch_test();
    detach_mutation_robust_test();
    tick_destroyed_parent_test();
    zero_init_test();
    add_children_test();
    return 0;
}
