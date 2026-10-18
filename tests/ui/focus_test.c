#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/surface.h"
#include "ui/internal/tree.h"
#include "ui/widget.h"

struct probe {
    int focus_in;
    int focus_out;
    int pointer_events;
    int key_events;
    bool handle_key;
    bool decline_pointer;
    struct ui_surface *surface;
    bool refocus_self;
    struct ui_widget *focused_on_out;
    struct ui_widget *focus_on_out;
    struct ui_widget *root_on_out;
};

static bool probe_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe *probe = widget->userdata;
    if (!event)
        return false;
    if (event->kind == UI_EVENT_FOCUS_IN) {
        probe->focus_in++;
        if (probe->refocus_self) {
            probe->refocus_self = false;
            ui_surface_focus(probe->surface, widget);
        }
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        probe->focus_out++;
        if (probe->surface)
            probe->focused_on_out = ui_surface_focused(probe->surface);
        if (probe->focus_on_out) {
            struct ui_widget *target = probe->focus_on_out;
            probe->focus_on_out = NULL;
            ui_surface_focus(probe->surface, target);
        }
        if (probe->root_on_out) {
            struct ui_widget *root = probe->root_on_out;
            probe->root_on_out = NULL;
            ui_surface_set_root(probe->surface, root);
        }
        return true;
    }
    if (event->kind == UI_EVENT_POINTER) {
        probe->pointer_events++;
        return !probe->decline_pointer;
    }
    if (event->kind == UI_EVENT_KEY) {
        probe->key_events++;
        return probe->handle_key;
    }
    return false;
}

static const struct ui_widget_ops OPS = { .event = probe_event };

static void targeted_focus_test(void)
{
    struct probe a = {0}, b = {0};
    struct ui_widget parent = {0}, wa = {0}, wb = {0};
    ui_widget_init(&parent, NULL, NULL);
    ui_widget_init(&wa, &OPS, &a);
    ui_widget_init(&wb, &OPS, &b);
    assert(ui_widget_add_child(&parent, &wa));
    assert(ui_widget_add_child(&parent, &wb));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &parent);

    ui_surface_focus(&context, &wa);
    assert(a.focus_in == 1 && a.focus_out == 0);
    assert(b.focus_in == 0 && b.focus_out == 0);
    assert(ui_surface_focused(&context) == &wa);

    ui_surface_focus(&context, &wb);
    assert(a.focus_out == 1 && a.focus_in == 1);
    assert(b.focus_in == 1 && b.focus_out == 0);
    assert(ui_surface_focused(&context) == &wb);

    ui_surface_focus(&context, NULL);
    assert(b.focus_out == 1);
    assert(ui_surface_focused(&context) == NULL);

    ui_surface_focus(&context, NULL);
    assert(b.focus_out == 1);

    ui_widget_destroy(&parent);
    ui_surface_destroy(&context);
}

static void focus_cycle_test(void)
{
    struct probe a = {0}, b = {0}, c = {0};
    struct ui_widget parent = {0}, wa = {0}, wb = {0}, wc = {0};
    ui_widget_init(&parent, NULL, NULL);
    ui_widget_init(&wa, &OPS, &a);
    ui_widget_init(&wb, &OPS, &b);
    ui_widget_init(&wc, &OPS, &c);
    ui_widget_set_focusable(&wa, true);
    ui_widget_set_focusable(&wc, true);
    assert(ui_widget_add_child(&parent, &wa));
    assert(ui_widget_add_child(&parent, &wb));
    assert(ui_widget_add_child(&parent, &wc));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &parent);

    assert(ui_surface_focus_next(&context));
    assert(ui_surface_focused(&context) == &wa);
    assert(ui_surface_focus_next(&context));
    assert(ui_surface_focused(&context) == &wc);
    assert(ui_surface_focus_next(&context));
    assert(ui_surface_focused(&context) == &wa);

    assert(ui_surface_focus_prev(&context));
    assert(ui_surface_focused(&context) == &wc);

    ui_widget_destroy(&parent);
    ui_surface_destroy(&context);
}

static void tab_focus_requests_frame_test(void)
{
    struct probe a = {0}, b = {0};
    struct ui_widget parent = {0}, wa = {0}, wb = {0};
    ui_widget_init(&parent, NULL, NULL);
    ui_widget_init(&wa, &OPS, &a);
    ui_widget_init(&wb, &OPS, &b);
    ui_widget_set_focusable(&wa, true);
    ui_widget_set_focusable(&wb, true);
    assert(ui_widget_add_child(&parent, &wa));
    assert(ui_widget_add_child(&parent, &wb));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &parent);
    ui_surface_focus(&context, &wa);
    ui_surface_take_frame_request(&context);

    struct ui_event tab = ui_event_key((struct ui_key_event){
        .key = UI_KEY_TAB,
    }, 0);
    assert(ui_surface_dispatch(&context, &tab));
    assert(ui_surface_focused(&context) == &wb);
    assert(ui_surface_frame_pending(&context));

    ui_widget_destroy(&parent);
    ui_surface_destroy(&context);
}

static void focus_cycle_none_focusable_test(void)
{
    struct probe p = {0};
    struct ui_widget root = {0};
    ui_widget_init(&root, &OPS, &p);

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);

    assert(!ui_surface_focus_next(&context));
    assert(!ui_surface_focus_prev(&context));
    assert(ui_surface_focused(&context) == NULL);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void capture_routes_to_target_test(void)
{
    struct probe root_probe = {0}, child_probe = {0};
    struct ui_widget root = {0}, child = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&child, &OPS, &child_probe);
    child.bounds = (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 10, .y = 10 },
    };
    assert(ui_widget_add_child(&root, &child));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_surface_resize(&context, (struct vector2i){ .x = 10, .y = 10 });

    ui_widget_capture_pointer(&child);

    struct ui_event pointer = ui_event_pointer((struct pointer_event){
        .kind = PTR_PRESS,
        .pos = { .x = 500, .y = 500 },
    }, 0);
    assert(ui_surface_dispatch(&context, &pointer));
    assert(child_probe.pointer_events == 1);
    assert(root_probe.pointer_events == 0);

    ui_widget_release_pointer(ui_surface_captured(&context));
    pointer.data.pointer.pos = (struct vector2i){ .x = 5, .y = 5 };
    assert(ui_surface_dispatch(&context, &pointer));
    assert(child_probe.pointer_events == 2);
    assert(root_probe.pointer_events == 0);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void destroy_focused_clears_focus_test(void)
{
    struct probe leaf_probe = {0};
    struct ui_widget root = {0}, leaf = {0};
    ui_widget_init(&root, NULL, NULL);
    ui_widget_init(&leaf, &OPS, &leaf_probe);
    assert(ui_widget_add_child(&root, &leaf));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_surface_focus(&context, &leaf);
    assert(leaf_probe.focus_in == 1);

    ui_widget_destroy(&leaf);
    assert(leaf_probe.focus_out == 0);
    assert(ui_surface_focused(&context) == NULL);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 0);
    assert(!ui_surface_dispatch(&context, &key));
    assert(leaf_probe.key_events == 0);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void destroy_captured_clears_capture_test(void)
{
    struct probe root_probe = {0}, child_probe = {0};
    struct ui_widget root = {0}, child = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&child, &OPS, &child_probe);
    assert(ui_widget_add_child(&root, &child));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_surface_resize(&context, (struct vector2i){ .x = 10, .y = 10 });
    ui_widget_capture_pointer(&child);
    assert(ui_surface_captured(&context) == &child);

    ui_widget_destroy(&child);
    assert(ui_surface_captured(&context) == NULL);

    struct ui_event pointer = ui_event_pointer((struct pointer_event){
        .kind = PTR_PRESS,
        .pos = { .x = 1, .y = 1 },
    }, 0);
    assert(ui_surface_dispatch(&context, &pointer));
    assert(child_probe.pointer_events == 0);
    assert(root_probe.pointer_events == 1);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void captured_decline_bubbles_without_sibling_fallback_test(void)
{
    struct probe root_probe = { .decline_pointer = true };
    struct probe child_probe = { .decline_pointer = true };
    struct probe sib_probe = {0};
    struct ui_widget root = {0}, child = {0}, sib = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&child, &OPS, &child_probe);
    ui_widget_init(&sib, &OPS, &sib_probe);
    assert(ui_widget_add_child(&root, &child));
    assert(ui_widget_add_child(&root, &sib));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_widget_capture_pointer(&child);

    struct ui_event pointer = ui_event_pointer((struct pointer_event){
        .kind = PTR_PRESS,
        .pos = { .x = 1, .y = 1 },
    }, 0);
    assert(!ui_surface_dispatch(&context, &pointer));
    assert(child_probe.pointer_events == 1);
    assert(root_probe.pointer_events == 1);
    assert(sib_probe.pointer_events == 0);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void key_bubbles_to_ancestor_test(void)
{
    struct probe root_probe = { .handle_key = true };
    struct probe leaf_probe = {0};
    struct probe sib_probe = {0};
    struct ui_widget root = {0}, leaf = {0}, sib = {0};
    ui_widget_init(&root, &OPS, &root_probe);
    ui_widget_init(&leaf, &OPS, &leaf_probe);
    ui_widget_init(&sib, &OPS, &sib_probe);
    assert(ui_widget_add_child(&root, &leaf));
    assert(ui_widget_add_child(&root, &sib));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_surface_focus(&context, &leaf);
    assert(leaf_probe.focus_in == 1);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 0);
    assert(ui_surface_dispatch(&context, &key));
    assert(leaf_probe.key_events == 1);
    assert(root_probe.key_events == 1);
    assert(sib_probe.key_events == 0);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void remove_focused_detaches_subtree_test(void)
{
    struct probe leaf_probe = {0};
    struct ui_widget root = {0}, mid = {0}, leaf = {0};
    ui_widget_init(&root, NULL, NULL);
    ui_widget_init(&mid, NULL, NULL);
    ui_widget_init(&leaf, &OPS, &leaf_probe);
    assert(ui_widget_add_child(&mid, &leaf));
    assert(ui_widget_add_child(&root, &mid));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_surface_focus(&context, &leaf);
    assert(leaf_probe.focus_in == 1);

    assert(ui_widget_remove_child(&root, &mid));
    assert(leaf_probe.focus_out == 1);
    assert(ui_surface_focused(&context) == NULL);
    assert(ui_widget_surface(&mid) == NULL && ui_widget_surface(&leaf) == NULL);

    ui_widget_destroy(&mid);
    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void focus_self_refocus_test(void)
{
    struct probe a = {0};
    struct ui_widget root = {0}, wa = {0};
    ui_widget_init(&root, NULL, NULL);
    ui_widget_init(&wa, &OPS, &a);
    assert(ui_widget_add_child(&root, &wa));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);

    a.surface = &context;
    a.refocus_self = true;
    ui_surface_focus(&context, &wa);
    assert(a.focus_in == 1);
    assert(ui_surface_focused(&context) == &wa);

    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void remove_captured_clears_capture_test(void)
{
    struct ui_widget root = {0}, child = {0};
    ui_widget_init(&root, NULL, NULL);
    ui_widget_init(&child, NULL, NULL);
    assert(ui_widget_add_child(&root, &child));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root);
    ui_widget_capture_pointer(&child);
    assert(ui_surface_captured(&context) == &child);

    assert(ui_widget_remove_child(&root, &child));
    assert(ui_surface_captured(&context) == NULL);
    assert(ui_widget_surface(&child) == NULL);

    ui_widget_destroy(&child);
    ui_widget_destroy(&root);
    ui_surface_destroy(&context);
}

static void set_root_replaces_live_root_test(void)
{
    struct probe leaf_probe = {0};
    struct ui_widget root_a = {0}, root_b = {0}, leaf = {0};
    ui_widget_init(&root_a, NULL, NULL);
    ui_widget_init(&root_b, NULL, NULL);
    ui_widget_init(&leaf, &OPS, &leaf_probe);
    assert(ui_widget_add_child(&root_a, &leaf));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root_a);
    leaf_probe.surface = &context;
    ui_surface_focus(&context, &leaf);
    assert(leaf_probe.focus_in == 1);

    ui_surface_set_root(&context, &root_b);
    assert(leaf_probe.focus_out == 1);
    assert(leaf_probe.focused_on_out == NULL);
    assert(ui_surface_focused(&context) == NULL);
    assert(ui_widget_surface(&root_a) == NULL && ui_widget_surface(&leaf) == NULL);
    assert(ui_widget_surface(&root_b) == &context);
    assert(ui_surface_root(&context) == &root_b);

    ui_widget_destroy(&root_a);
    ui_widget_destroy(&root_b);
    ui_surface_destroy(&context);
}

static void focus_change_in_focus_out_test(void)
{
    struct probe a = {0}, b = {0}, c = {0};
    struct ui_widget parent = {0}, wa = {0}, wb = {0}, wc = {0};
    ui_widget_init(&parent, NULL, NULL);
    ui_widget_init(&wa, &OPS, &a);
    ui_widget_init(&wb, &OPS, &b);
    ui_widget_init(&wc, &OPS, &c);
    assert(ui_widget_add_child(&parent, &wa));
    assert(ui_widget_add_child(&parent, &wb));
    assert(ui_widget_add_child(&parent, &wc));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &parent);
    ui_surface_focus(&context, &wa);
    assert(a.focus_in == 1);

    a.surface = &context;
    a.focus_on_out = &wc;
    ui_surface_focus(&context, &wb);

    assert(a.focus_out == 1);
    assert(ui_surface_focused(&context) == &wc);
    assert(c.focus_in == 1);
    assert(b.focus_in == 0 && b.focus_out == 0);

    ui_widget_destroy(&parent);
    ui_surface_destroy(&context);
}

static void set_root_in_detach_focus_out_test(void)
{
    struct probe leaf_probe = {0};
    struct ui_widget root_a = {0}, root_b = {0}, root_x = {0}, leaf = {0};
    ui_widget_init(&root_a, NULL, NULL);
    ui_widget_init(&root_b, NULL, NULL);
    ui_widget_init(&root_x, NULL, NULL);
    ui_widget_init(&leaf, &OPS, &leaf_probe);
    assert(ui_widget_add_child(&root_a, &leaf));

    struct ui_surface context = {0};
    ui_surface_set_root(&context, &root_a);
    leaf_probe.surface = &context;
    ui_surface_focus(&context, &leaf);
    assert(leaf_probe.focus_in == 1);

    leaf_probe.root_on_out = &root_x;
    ui_surface_set_root(&context, &root_b);

    assert(leaf_probe.focus_out == 1);
    assert(ui_widget_surface(&root_x) == NULL);
    assert(ui_widget_surface(&root_a) == NULL && ui_widget_surface(&leaf) == NULL);
    assert(ui_surface_root(&context) == &root_b && ui_widget_surface(&root_b) == &context);
    assert(ui_surface_focused(&context) == NULL);

    ui_widget_destroy(&root_a);
    ui_widget_destroy(&root_b);
    ui_widget_destroy(&root_x);
    ui_surface_destroy(&context);
}

static void invalid_targets_rejected_test(void)
{
    struct ui_surface a = {0}, b = {0};
    struct ui_widget root_a = {0}, root_b = {0}, foreign = {0}, dead = {0};
    ui_widget_init(&root_a, NULL, NULL);
    ui_widget_init(&root_b, NULL, NULL);
    ui_widget_init(&foreign, NULL, NULL);
    ui_widget_init(&dead, NULL, NULL);
    assert(ui_widget_add_child(&root_b, &foreign));
    assert(ui_widget_add_child(&root_a, &dead));
    ui_surface_set_root(&a, &root_a);
    ui_surface_set_root(&b, &root_b);

    ui_widget_destroy(&dead);
    ui_surface_focus(&a, &dead);
    assert(ui_surface_focused(&a) == NULL);
    ui_widget_capture_pointer(&dead);
    assert(ui_surface_captured(&a) == NULL);

    ui_surface_focus(&a, &foreign);
    assert(ui_surface_focused(&a) == NULL);
    ui_widget_capture_pointer(&foreign);
    assert(ui_surface_captured(&a) == NULL);

    ui_surface_focus(&a, &root_a);
    assert(ui_surface_focused(&a) == &root_a);
    ui_widget_capture_pointer(&root_a);
    assert(ui_surface_captured(&a) == &root_a);

    ui_widget_destroy(&root_a);
    ui_widget_destroy(&root_b);
    ui_surface_destroy(&b);
    ui_surface_destroy(&a);
}

int main(void)
{
    targeted_focus_test();
    focus_cycle_test();
    tab_focus_requests_frame_test();
    focus_cycle_none_focusable_test();
    capture_routes_to_target_test();
    destroy_focused_clears_focus_test();
    destroy_captured_clears_capture_test();
    captured_decline_bubbles_without_sibling_fallback_test();
    key_bubbles_to_ancestor_test();
    remove_focused_detaches_subtree_test();
    focus_self_refocus_test();
    remove_captured_clears_capture_test();
    set_root_replaces_live_root_test();
    focus_change_in_focus_out_test();
    set_root_in_detach_focus_out_test();
    invalid_targets_rejected_test();
    return 0;
}
