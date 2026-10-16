#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/widgets/scrollview.h"
#include "ui/surface.h"
#include "ui/internal/tree.h"
#include "ui/paint.h"
#include "ui/debug.h"
#include "paint/scene.h"
#include "ui/ui.h"
#include "ui/widget_impl.h"

struct probe_child {
    struct ui_widget widget;
    struct vector2i preferred;
    int presses;
    struct vector2i last_press;
    int keys;
    int motions;
    int leaves;
    bool capture_on_press;
    bool handle_press;
    bool consume_motion;
};

/* The color the probe paints. Bar fills are every other fill. */
#define PROBE_COLOR 0xff123456u

/* Puts child in the scrollview in place of any earlier child. */
static bool replace_child(struct ui_scrollview *sv, struct ui_widget *child)
{
    struct ui_widget *old = ui_scrollview_child(sv);
    if (old)
        ui_widget_remove_child(&sv->widget, old);
    return !child || ui_widget_add_child(&sv->widget, child);
}

static struct vector2i probe_measure(const struct ui_widget *widget,
                                     struct ui_constraints constraints)
{
    const struct probe_child *p =
        ui_container_of_const(widget, struct probe_child, widget);
    return ui_constraints_clamp(constraints, p->preferred);
}

static bool probe_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe_child *p =
        ui_container_of(widget, struct probe_child, widget);
    if (event->kind == UI_EVENT_KEY) {
        p->keys++;
        return false;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    if (event->data.pointer.kind == UI_POINTER_MOTION) {
        p->motions++;
        return p->consume_motion;
    }
    if (event->data.pointer.kind == UI_POINTER_LEAVE) {
        p->leaves++;
        return false;
    }
    if (event->data.pointer.kind != UI_POINTER_PRESS ||
        !rect2i_contains(widget->bounds, event->data.pointer.pos))
        return false;
    p->presses++;
    p->last_press = event->data.pointer.pos;
    if (p->capture_on_press && ui_widget_surface(widget)) {
        p->capture_on_press = false;
        ui_widget_capture_pointer(widget);
        return true;
    }
    return p->handle_press;
}

static void probe_paint(const struct ui_widget *widget,
                        struct ui_paint_context *paint)
{
    (void)widget;
    assert(ui_paint_rect(paint, ui_paint_bounds(paint), PROBE_COLOR));
}

static const struct ui_widget_ops PROBE_OPS = {
    .measure = probe_measure,
    .event = probe_event,
    .paint = probe_paint,
};

static void probe_init(struct probe_child *p, struct vector2i preferred)
{
    p->preferred = preferred;
    p->handle_press = true;
    ui_widget_init(&p->widget, &PROBE_OPS, NULL);
}

static struct ui_event scroll_event(struct vector2i pos, int lines)
{
    return ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_SCROLL, .pos = pos, .scroll_lines = lines,
    }, 0);
}

static struct ui_event press_event(struct vector2i pos, uint64_t time_ms)
{
    return ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = pos,
    }, time_ms);
}

/* Measures sv at the size of rect, then lays it out at rect. */
static void fit(struct ui_scrollview *sv, struct rect2i rect)
{
    ui_widget_measure(&sv->widget, (struct ui_constraints){
        .max = rect.size,
    });
    ui_widget_layout(&sv->widget, rect);
}

/* Makes root the surface root, sized 100 by 50. */
static void set_root_100x50(struct ui_surface *ctx, struct ui_widget *root)
{
    ui_surface_set_root(ctx, root);
    ui_surface_resize(ctx, (struct vector2i){ .x = 100, .y = 50 });
}

/* The offset that shows the last line of the content. */
static int max_offset(const struct ui_scrollview *sv)
{
    return ui_scrollview_content_height(sv) - ui_scrollview_viewport_height(sv);
}

/* A scrollview with the bar off, for tests that are not about the bar. */
static void init_plain(struct ui_scrollview *sv)
{
    ui_scrollview_init(sv);
    ui_scrollview_set_scrollbar(sv, false);
}

static void test_zero(void)
{
    struct ui_scrollview sv = {0};
    assert(ui_scrollview_offset(&sv) == 0);
    assert(ui_scrollview_content_height(&sv) == 0);
    assert(ui_scrollview_viewport_height(&sv) == 0);
    assert(ui_scrollview_child(&sv) == NULL);
    ui_scrollview_scroll_by(&sv, 10);
    assert(ui_scrollview_offset(&sv) == 0);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_widget_build_scene(&sv.widget, &scene));
    assert(scene.n_ops == 0);
    ui_scene_destroy(&scene);
}

static void test_measure_layout(void)
{
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 40, .y = 200 });
    assert(replace_child(&sv, &child.widget));
    assert(ui_scrollview_child(&sv) == &child.widget);
    assert(child.widget.parent == &sv.widget);

    struct vector2i measured = ui_widget_measure(&sv.widget,
        (struct ui_constraints){
            .min = { .x = 0, .y = 0 },
            .max = { .x = 100, .y = 50 },
        });
    assert(measured.x == 40 && measured.y == 50);
    assert(ui_scrollview_content_height(&sv) == 200);

    ui_widget_layout(&sv.widget, (struct rect2i){
        .pos = { .x = 10, .y = 20 }, .size = { .x = 100, .y = 50 },
    });
    assert(ui_scrollview_viewport_height(&sv) == 50);
    assert(child.widget.bounds.pos.x == 10);
    assert(child.widget.bounds.pos.y == 20);
    assert(child.widget.bounds.width == 100);
    assert(child.widget.bounds.height == 200);
    ui_widget_destroy(&sv.widget);
}

static void test_scroll_and_forward(void)
{
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    replace_child(&sv, &child.widget);
    fit(&sv, (struct rect2i){ .size = { .x = 100, .y = 50 } });

    struct ui_event ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_widget_event(&sv.widget, &ev));
    assert(ui_scrollview_offset(&sv) == 48);
    ui_widget_layout(&sv.widget, sv.widget.bounds);
    assert(child.widget.bounds.pos.y == -48);

    ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -100);
    assert(ui_widget_event(&sv.widget, &ev));
    assert(ui_scrollview_offset(&sv) == 150);
    ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, 100);
    assert(ui_widget_event(&sv.widget, &ev));
    assert(ui_scrollview_offset(&sv) == 0);
    ui_widget_layout(&sv.widget, sv.widget.bounds);
    assert(child.widget.bounds.pos.y == 0);

    ev = scroll_event((struct vector2i){ .x = 200, .y = 5 }, -1);
    assert(!ui_widget_event(&sv.widget, &ev));
    assert(ui_scrollview_offset(&sv) == 0);

    /* A sideways wheel step does not scroll the view up or down. */
    ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    ev.data.pointer.button = UI_POINTER_WHEEL_BUTTON + 3;
    assert(!ui_widget_event(&sv.widget, &ev));
    assert(ui_scrollview_offset(&sv) == 0);

    struct ui_event press = press_event((struct vector2i){ .x = 10, .y = 10 }, 0);
    assert(ui_widget_event(&sv.widget, &press));
    assert(child.presses == 1 && child.last_press.y == 10);

    press.data.pointer.pos = (struct vector2i){ .x = 10, .y = 60 };
    assert(!ui_widget_event(&sv.widget, &press));
    assert(child.presses == 1);
    ui_widget_destroy(&sv.widget);
}

static struct ui_event smooth_scroll_event(float delta)
{
    return ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_SCROLL, .pos = { .x = 5, .y = 5 },
        .scroll_delta = delta,
    }, 0);
}

/* A wild smooth-scroll delta clamps or drops, and later wheel events
 * still scroll normally. */
static void test_extreme_scroll_deltas(void)
{
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    replace_child(&sv, &child.widget);
    fit(&sv, (struct rect2i){ .size = { .x = 100, .y = 50 } });

    struct ui_event ev = smooth_scroll_event(-1e30f);
    ui_widget_event(&sv.widget, &ev);
    assert(ui_scrollview_offset(&sv) == 150);
    ev = smooth_scroll_event(1e30f);
    ui_widget_event(&sv.widget, &ev);
    assert(ui_scrollview_offset(&sv) == 0);

    ev = smooth_scroll_event(NAN);
    ui_widget_event(&sv.widget, &ev);
    assert(ui_scrollview_offset(&sv) == 0);
    ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_widget_event(&sv.widget, &ev));
    assert(ui_scrollview_offset(&sv) == 48);
    ui_widget_destroy(&sv.widget);
}

static void test_press_hits_scrolled_child(void)
{
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    replace_child(&sv, &child.widget);
    fit(&sv, (struct rect2i){
        .pos = { .x = 10, .y = 20 }, .size = { .x = 100, .y = 50 },
    });
    ui_scrollview_scroll_to(&sv, 48);
    ui_widget_layout(&sv.widget, sv.widget.bounds);
    assert(child.widget.bounds.pos.y == -28);

    struct ui_event press = press_event((struct vector2i){ .x = 15, .y = 50 }, 0);
    assert(ui_widget_event(&sv.widget, &press));
    assert(child.presses == 1);
    assert(child.last_press.y - child.widget.bounds.y == 78);
    ui_widget_destroy(&sv.widget);
}

static void test_paint_clips_viewport(void)
{
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 40, .y = 80 });
    replace_child(&sv, &child.widget);
    ui_widget_measure(&sv.widget, (struct ui_constraints){
        .min = { .x = 0, .y = 0 },
        .max = { .x = 100, .y = 30 },
    });
    ui_widget_layout(&sv.widget, (struct rect2i){
        .pos = { .x = 100, .y = 50 }, .size = { .x = 20, .y = 30 },
    });
    ui_scrollview_scroll_to(&sv, 10);
    ui_widget_layout(&sv.widget, sv.widget.bounds);

    struct ui_scene scene = {0};
    assert(ui_widget_build_scene(&sv.widget, &scene));
    assert(scene.n_ops == 3);
    assert(scene.ops[0].kind == UI_SCENE_OP_PUSH_CLIP);
    assert(scene.ops[0].rect.x == 100 && scene.ops[0].rect.y == 50);
    assert(scene.ops[0].rect.width == 20 && scene.ops[0].rect.height == 30);
    assert(scene.ops[1].kind == UI_SCENE_OP_RECT);
    assert(scene.ops[1].rect.x == 100 && scene.ops[1].rect.y == 40);
    assert(scene.ops[1].rect.width == 20 && scene.ops[1].rect.height == 80);
    assert(scene.ops[2].kind == UI_SCENE_OP_POP_CLIP);
    assert(scene.clip_depth == 0);
    ui_widget_destroy(&sv.widget);
    ui_scene_destroy(&scene);
}

static void test_offset_clamps_when_emptied(void)
{
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    replace_child(&sv, &child.widget);
    fit(&sv, (struct rect2i){ .size = { .x = 100, .y = 50 } });
    ui_scrollview_scroll_to(&sv, 48);
    assert(ui_scrollview_offset(&sv) == 48);

    assert(replace_child(&sv, NULL));
    ui_widget_layout(&sv.widget, sv.widget.bounds);
    assert(ui_scrollview_offset(&sv) == 0);
    ui_widget_destroy(&sv.widget);
}

static void test_captured_child_declines_once(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    child.capture_on_press = true;
    child.handle_press = false;
    replace_child(&sv, &child.widget);
    set_root_100x50(&ctx, &sv.widget);

    struct ui_event press = press_event((struct vector2i){ .x = 10, .y = 10 }, 1);
    assert(ui_surface_dispatch(&ctx, &press));
    assert(child.presses == 1);
    assert(ui_surface_captured(&ctx) == &child.widget);

    press.data.pointer.pos = (struct vector2i){ .x = 20, .y = 20 };
    assert(!ui_surface_dispatch(&ctx, &press));
    assert(child.presses == 2);
    ui_widget_destroy(&sv.widget);
    ui_surface_destroy(&ctx);
}

static void test_captured_scrollview_skips_children(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    child.handle_press = false;
    replace_child(&sv, &child.widget);
    set_root_100x50(&ctx, &sv.widget);
    ui_widget_capture_pointer(&sv.widget);

    struct ui_event press = press_event((struct vector2i){ .x = 10, .y = 10 }, 1);
    assert(!ui_surface_dispatch(&ctx, &press));
    assert(child.presses == 0);
    ui_widget_destroy(&sv.widget);
    ui_surface_destroy(&ctx);
}

static void test_focused_child_key_bubbles_once(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    ui_widget_set_focusable(&child.widget, true);
    replace_child(&sv, &child.widget);
    set_root_100x50(&ctx, &sv.widget);
    ui_surface_focus(&ctx, &child.widget);
    assert(ui_surface_focused(&ctx) == &child.widget);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_LEFT,
    }, 1);
    assert(!ui_surface_dispatch(&ctx, &key));
    assert(child.keys == 1);
    ui_widget_destroy(&sv.widget);
    ui_surface_destroy(&ctx);
}

static void test_key_bubble_stays_on_ancestor_chain(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child container = {0};
    probe_init(&container, (struct vector2i){ .x = 100, .y = 200 });
    struct probe_child leaf = {0};
    struct probe_child sibling = {0};
    probe_init(&leaf, (struct vector2i){ .x = 50, .y = 50 });
    probe_init(&sibling, (struct vector2i){ .x = 50, .y = 50 });
    ui_widget_set_focusable(&leaf.widget, true);
    assert(ui_widget_add_child(&container.widget, &leaf.widget));
    assert(ui_widget_add_child(&container.widget, &sibling.widget));
    replace_child(&sv, &container.widget);
    set_root_100x50(&ctx, &sv.widget);
    ui_surface_focus(&ctx, &leaf.widget);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_LEFT,
    }, 1);
    assert(!ui_surface_dispatch(&ctx, &key));
    assert(leaf.keys == 1);
    assert(container.keys == 1);
    assert(sibling.keys == 0);
    ui_widget_destroy(&sv.widget);
    ui_surface_destroy(&ctx);
}

static void test_motion_and_leave_respect_viewport_clip(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    struct probe_child child = {0};
    probe_init(&child, (struct vector2i){ .x = 100, .y = 200 });
    replace_child(&sv, &child.widget);
    set_root_100x50(&ctx, &sv.widget);

    struct ui_event motion = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = { .x = 500, .y = 500 },
    }, 1);
    assert(!ui_surface_dispatch(&ctx, &motion));
    assert(child.motions == 0);

    struct ui_event leave = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_LEAVE, .pos = { .x = 500, .y = 500 },
    }, 2);
    assert(!ui_surface_dispatch(&ctx, &leave));
    assert(child.leaves == 0);
    ui_widget_destroy(&sv.widget);
    ui_surface_destroy(&ctx);
}

static void test_set_child_and_lifecycle(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview sv = {0};
    init_plain(&sv);
    ui_surface_set_root(&ctx, &sv.widget);

    struct probe_child first = {0};
    struct probe_child second = {0};
    probe_init(&first, (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&second, (struct vector2i){ .x = 10, .y = 10 });

    assert(replace_child(&sv, &first.widget));
    assert(ui_widget_surface(&first.widget) == &ctx);
    assert(replace_child(&sv, &second.widget));
    assert(ui_widget_surface(&first.widget) == NULL && first.widget.parent == NULL);
    assert(!first.widget.impl.destroyed);
    assert(ui_widget_surface(&second.widget) == &ctx);
    assert(second.widget.parent == &sv.widget);

    assert(replace_child(&sv, NULL));
    assert(ui_scrollview_child(&sv) == NULL);
    assert(ui_widget_surface(&second.widget) == NULL && !second.widget.impl.destroyed);

    assert(replace_child(&sv, &first.widget));
    ui_widget_destroy(&sv.widget);
    assert(first.widget.impl.destroyed);
    ui_widget_destroy(&first.widget);
    ui_widget_destroy(&second.widget);
    ui_surface_destroy(&ctx);
}

static bool release_capture_event(struct ui_widget *widget,
                                   const struct ui_event *event)
{
    if (event->kind != UI_EVENT_POINTER)
        return false;
    int *deliveries = widget->userdata;
    (*deliveries)++;
    ui_widget_release_pointer(ui_surface_captured(ui_widget_surface(widget)));
    return false;
}

static void test_capture_released_before_fallback(void)
{
    struct ui_surface ctx = {0};
    struct ui_scrollview outer = {0}, inner = {0};
    struct ui_widget child = {0};
    int deliveries = 0;
    const struct ui_widget_ops ops = { .event = release_capture_event };
    init_plain(&outer);
    init_plain(&inner);
    ui_widget_init(&child, &ops, &deliveries);
    assert(replace_child(&outer, &inner.widget));
    assert(replace_child(&inner, &child));
    ui_surface_set_root(&ctx, &outer.widget);
    ui_surface_resize(&ctx, (struct vector2i){ .x = 100, .y = 100 });
    ui_widget_capture_pointer(&child);
    struct ui_event event = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = { .x = 1, .y = 1 },
    }, 0);
    assert(!ui_surface_dispatch(&ctx, &event));
    assert(deliveries == 1);
    assert(!ui_surface_dispatch(&ctx, &event));
    assert(deliveries == 2);
    ui_widget_destroy(&outer.widget);
    ui_surface_destroy(&ctx);
}

/* ---- The bar. A scrollview in a frame, with the probe as its child. ---- */

/* The frame records the presses it gets. It sits at frame_pos in the
 * surface, as the terminal view sits under the tab bar. */
struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_widget frame;
    struct ui_scrollview sv;
    struct probe_child content;
    struct css_sheet sheet;
    struct ui_scene scene;
    int changes;
    int offset;
    int frame_presses;
};

static struct fixture *frame_fixture(const struct ui_widget *frame)
{
    return ui_container_of((struct ui_widget *)frame, struct fixture, frame);
}

static bool frame_event(struct ui_widget *widget, const struct ui_event *event)
{
    if (event->kind != UI_EVENT_POINTER ||
        event->data.pointer.kind != UI_POINTER_PRESS)
        return false;
    frame_fixture(widget)->frame_presses++;
    return true;
}

static const struct ui_widget_ops FRAME_OPS = {
    .name = "frame",
    .event = frame_event,
};

static bool on_action(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    if (action->kind != UI_ACTION_CHANGE)
        return false;
    f->changes++;
    f->offset = action->index;
    return true;
}

/* The bar starts on. In overlay mode, the default, it starts hidden. The
 * child is content_h tall. */
static void setup(struct fixture *f, const char *css, struct vector2i frame_pos,
                  struct vector2i frame_size, int content_h)
{
    *f = (struct fixture){ .offset = -1 };
    if (css)
        assert(css_parse(&f->sheet, css));
    ui_widget_init(&f->root, NULL, NULL);
    ui_widget_init(&f->frame, &FRAME_OPS, NULL);
    ui_widget_set_fixed_size(&f->frame, frame_size);
    ui_widget_set_placement(&f->frame, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor = { .pos = frame_pos },
    });
    ui_scrollview_init(&f->sv);
    ui_widget_set_action_handler(&f->sv.widget, on_action, f);
    probe_init(&f->content, (struct vector2i){ .x = frame_size.x, .y = content_h });
    assert(ui_widget_add_child(&f->root, &f->frame));
    assert(ui_widget_add_child(&f->frame, &f->sv.widget));
    assert(ui_widget_add_child(&f->sv.widget, &f->content.widget));
    ui_surface_set_sheet(&f->surface, &f->sheet);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, vector2i_add(frame_pos, frame_size));
    ui_surface_prepare(&f->surface);
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(&f->root);
    ui_scene_destroy(&f->scene);
    css_sheet_clear(&f->sheet);
}

static bool pointer(struct fixture *f, enum ui_pointer_kind kind,
                    struct vector2i pos, int button, unsigned mods)
{
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos, .button = button, .mods = mods,
    }, 0);
    bool handled = ui_surface_dispatch(&f->surface, &e);
    ui_surface_prepare(&f->surface);
    return handled;
}

static void set_metrics(struct fixture *f, int history, int visible, int offset)
{
    ui_scrollview_set_metrics(&f->sv, (struct ui_scroll_metrics){
        .history_lines = history, .visible_lines = visible,
        .view_offset = offset,
    });
    ui_surface_prepare(&f->surface);
}

/* Overlay on, with 300 lines of history and 30 visible, and the bar shown. */
static void show_overlay_bar(struct fixture *f)
{
    ui_scrollview_set_overlay(&f->sv, true);
    set_metrics(f, 300, 30, 0);
    ui_scrollview_flash(&f->sv);
}

/* The rect of the named debug part of widget, or an empty rect when the
 * widget reports none. */
static struct rect2i named_part(const struct ui_widget *widget, const char *name)
{
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(widget, &parts);
    struct rect2i r = {0};
    for (int i = 0; i < parts.n; i++)
        if (!strcmp(parts.items[i].name, name))
            r = parts.items[i].rect;
    ui_debug_parts_free(&parts);
    return r;
}

/* The part's rect in surface space, or an empty rect when the scrollview
 * reports none. */
static struct rect2i part(struct fixture *f, const char *name)
{
    return named_part(&f->sv.widget, name);
}

static const struct ui_scene *paint(struct fixture *f)
{
    ui_scene_destroy(&f->scene);
    assert(ui_surface_build_scene(&f->surface, &f->scene));
    return &f->scene;
}

/* True for a plain or rounded rect fill. */
static bool is_fill(const struct ui_scene_op *op)
{
    return op->kind == UI_SCENE_OP_RECT || op->kind == UI_SCENE_OP_ROUNDED_RECT;
}

/* The index of the first filled rect of this color, or -1. */
static int fill_index(const struct ui_scene *scene, uint32_t color)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (is_fill(&scene->ops[i]) && scene->ops[i].color == color)
            return i;
    return -1;
}

/* The number of filled rects that the probe did not paint. */
static int bar_fills(const struct ui_scene *scene)
{
    int n = 0;
    for (int i = 0; i < scene->n_ops; i++)
        if (is_fill(&scene->ops[i]) && scene->ops[i].color != PROBE_COLOR)
            n++;
    return n;
}

static bool has_fill_op(const struct ui_scene *scene, struct rect2i rect,
                        uint32_t color)
{
    int i = fill_index(scene, color);
    return i >= 0 && rect2i_eq(scene->ops[i].rect, rect);
}

static struct vector2i center(struct rect2i r)
{
    return (struct vector2i){ .x = r.x + r.width / 2, .y = r.y + r.height / 2 };
}

/* Ticks to now, then past the bar's hide deadline, which must lie after now. */
static void run_to_hide(struct fixture *f, uint64_t now)
{
    ui_surface_tick(&f->surface, now);
    uint64_t deadline = ui_surface_next_deadline(&f->surface);
    assert(deadline > now);
    ui_surface_tick(&f->surface, deadline);
}

/* CSS sets the track and thumb of each state: width, margin, padding,
 * color, and the thumb's minimum height. Line metrics set the thumb. */
static void css_drives_track_and_thumb_test(void)
{
    const int W = 100, H = 400;
    struct fixture f;
    setup(&f,
        ".scrollbar { width: 10px; margin: 4px 2px 6px; padding: 0 3px; }"
        ".scrollbar:hover { width: 16px; }"
        ".scrollbar:active { width: 16px; margin: 0 0 0; }"
        ".scrollbar-thumb { width: 4px; min-height: 30px; background: #112233; "
        "border-radius: 2px; }"
        ".scrollbar-thumb:hover { width: 10px; background: #445566; }"
        ".scrollbar-thumb:active { width: 10px; background: #778899; }",
        (struct vector2i){0}, (struct vector2i){ .x = W, .y = H }, 200);
    ui_scrollview_set_overlay(&f.sv, true);
    set_metrics(&f, 300, 30, 150);

    /* A hidden bar draws no track and no thumb. */
    assert(rect2i_is_empty(part(&f, "scroll-track")));
    assert(rect2i_is_empty(part(&f, "scroll-thumb")));

    /* Shown and idle, the track is the idle width. */
    ui_scrollview_flash(&f.sv);
    struct rect2i track = part(&f, "scroll-track");
    assert(track.width == 10 && track.x == W - 2 - 10);
    assert(track.y == 4 && rect2i_end(track).y == H - 6);
    const struct ui_scene *s = paint(&f);
    struct rect2i thumb = part(&f, "scroll-thumb");
    assert(thumb.width == 4 && thumb.x == W - 2 - 10 + 3);
    assert(thumb.height >= 30);
    assert(thumb.y >= 4 && rect2i_end(thumb).y <= H - 6);
    int idle = fill_index(s, 0xFF112233);
    assert(idle >= 0 && rect2i_eq(s->ops[idle].rect, thumb));
    /* The bar draws after the child, so it lies over the child's paint. */
    int content = fill_index(s, PROBE_COLOR);
    assert(content >= 0 && idle > content);

    /* Hover widens the track, and the widened track takes the pointer. */
    assert(!pointer(&f, UI_POINTER_MOTION, center(track), 0, 0));
    s = paint(&f);
    track = part(&f, "scroll-track");
    assert(track.width == 16 && track.x == W - 2 - 16);
    thumb = part(&f, "scroll-thumb");
    int hover = fill_index(s, 0xFF445566);
    assert(hover >= 0 && rect2i_eq(s->ops[hover].rect, thumb));
    assert(s->ops[hover].rect.width == 10 && s->ops[hover].rect.x == W - 2 - 16 + 3);

    assert(pointer(&f, UI_POINTER_PRESS, center(part(&f, "scroll-thumb")), 0, 0));
    s = paint(&f);
    int active = fill_index(s, 0xFF778899);
    assert(active >= 0 && s->ops[active].rect.x == W - 16 + 3);
    teardown(&f);

    setup(&f, ".scrollbar { width: 10px; background: #010203; }"
              ".scrollbar-thumb { background: #445566; }",
          (struct vector2i){0}, (struct vector2i){ .x = W, .y = H }, 200);
    set_metrics(&f, 300, 30, 0);
    ui_scrollview_flash(&f.sv);
    s = paint(&f);
    int track_fill = fill_index(s, 0xFF010203);
    assert(track_fill >= 0 && s->ops[track_fill].rect.height == H);
    assert(s->ops[track_fill].rect.x == W - 10);
    assert(fill_index(s, 0xFF445566) >= 0);
    teardown(&f);
}

/* The thumb is to the track as the screen is to all the lines, but never
 * shorter than its minimum or longer than the track. The child is as tall
 * as the view, so only the metrics give the bar a range. */
static void thumb_size_follows_metrics_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    ui_scrollview_set_overlay(&f.sv, true);
    ui_scrollview_flash(&f.sv);

    set_metrics(&f, 100, 100, 0);
    assert(part(&f, "scroll-thumb").height == 100);
    set_metrics(&f, 300, 100, 0);
    assert(part(&f, "scroll-thumb").height == 50);
    set_metrics(&f, 10000, 1, 0);
    assert(part(&f, "scroll-thumb").height == 24);
    set_metrics(&f, 0, 30, 0);
    assert(rect2i_is_empty(part(&f, "scroll-track")));
    teardown(&f);

    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 10 }, 10);
    ui_scrollview_set_overlay(&f.sv, true);
    ui_scrollview_flash(&f.sv);
    set_metrics(&f, 10000, 1, 0);
    assert(part(&f, "scroll-thumb").height == 10);
    teardown(&f);
}

/* Dragging the thumb reports offsets while the pointer is anywhere; the
 * ends of the travel are all the history and none of it. */
static void drag_reports_offsets_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    set_metrics(&f, 40, 10, 20);
    ui_scrollview_flash(&f.sv);

    struct rect2i thumb = part(&f, "scroll-thumb");
    assert(pointer(&f, UI_POINTER_PRESS, center(thumb), 0, 0));
    /* A press on the thumb keeps the view where it is, give or take a line. */
    assert(f.changes == 0 || (f.offset >= 19 && f.offset <= 21));

    assert(pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 10, .y = -50 }, 0, 0));
    assert(f.offset == 40);
    assert(pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 500, .y = 250 }, 0, 0));
    assert(f.offset == 0);
    assert(pointer(&f, UI_POINTER_RELEASE, (struct vector2i){ .x = 500, .y = 250 }, 0, 0));

    int changes = f.changes;
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 95, .y = 10 }, 0, 0);
    assert(f.changes == changes);
    teardown(&f);
}

/* A press on the track in line mode reports the offset, and the child does
 * not move. */
static void line_mode_press_reports_offset_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    set_metrics(&f, 300, 30, 150);
    ui_scrollview_flash(&f.sv);

    struct rect2i track = part(&f, "scroll-track");
    struct vector2i bottom = { .x = track.x + track.width / 2,
                               .y = rect2i_end(track).y - 1 };
    assert(pointer(&f, UI_POINTER_PRESS, bottom, 0, 0));
    assert(f.changes == 1 && f.offset == 0);
    assert(ui_scrollview_offset(&f.sv) == 0);
    assert(f.content.widget.bounds.pos.y == 0);
    pointer(&f, UI_POINTER_RELEASE, bottom, 0, 0);
    teardown(&f);
}

/* In line mode the wheel is left to the host. In pixel mode it scrolls. */
static void wheel_follows_mode_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    set_metrics(&f, 300, 30, 150);
    struct ui_event ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(!ui_surface_dispatch(&f.surface, &ev));
    assert(ui_scrollview_offset(&f.sv) == 0);
    assert(f.changes == 0);
    teardown(&f);

    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_surface_dispatch(&f.surface, &ev));
    assert(ui_scrollview_offset(&f.sv) == 48);
    teardown(&f);
}

/* Under a tab bar the bar paints where it takes presses. */
static void paints_where_it_hits_test(void)
{
    const int BAR_H = 37;
    struct fixture f;
    setup(&f, ".scrollbar-thumb { background: #445566; }",
          (struct vector2i){ .y = BAR_H }, (struct vector2i){ .x = 100, .y = 300 }, 300);
    ui_scrollview_set_overlay(&f.sv, true);
    set_metrics(&f, 300, 30, 100);
    ui_scrollview_flash(&f.sv);

    struct rect2i track = part(&f, "scroll-track");
    assert(track.y >= BAR_H && rect2i_end(track).y <= BAR_H + 300);
    const struct ui_scene *s = paint(&f);
    struct rect2i thumb = part(&f, "scroll-thumb");
    assert(has_fill_op(s, thumb, 0xFF445566));
    assert(pointer(&f, UI_POINTER_PRESS, center(thumb), 0, 0));
    assert(f.changes == 0 || (f.offset >= 99 && f.offset <= 101));
    teardown(&f);
}

/* A press the bar declines goes to the child under the track, so Shift
 * selects text through the track. Only a press the child also declines
 * reaches the scrollview's parent. */
static void declined_press_reaches_parent_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    show_overlay_bar(&f);
    struct vector2i on_track = center(part(&f, "scroll-track"));

    /* Shift and a secondary button are declined. The child takes them. */
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, UI_MOD_SHIFT));
    assert(f.content.presses == 1 && f.frame_presses == 0 && f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, UI_MOD_SHIFT);

    assert(pointer(&f, UI_POINTER_PRESS, on_track, 1, 0));
    assert(f.content.presses == 2 && f.frame_presses == 0 && f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, on_track, 1, 0);

    /* Off the track the child takes the press as usual. */
    assert(pointer(&f, UI_POINTER_PRESS, (struct vector2i){ .x = 10, .y = 10 }, 0, 0));
    assert(f.content.presses == 3 && f.frame_presses == 0);
    pointer(&f, UI_POINTER_RELEASE, (struct vector2i){ .x = 10, .y = 10 }, 0, 0);

    /* Shown, a plain press on the track is the bar's. */
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, 0));
    assert(f.content.presses == 3 && f.changes == 1);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, 0);

    /* When the child declines the declined press, the parent gets it. */
    f.content.handle_press = false;
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, UI_MOD_SHIFT));
    assert(f.content.presses == 4 && f.frame_presses == 1);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, UI_MOD_SHIFT);
    teardown(&f);
}

/* A hidden overlay bar takes no press. The child under its track gets it.
 * Hover still shows the bar. */
static void hidden_bar_takes_no_press_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    show_overlay_bar(&f);
    struct vector2i on_track = center(part(&f, "scroll-track"));
    ui_scrollview_hide(&f.sv);

    assert(!ui_scrollview_is_shown(&f.sv));
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, 0));
    assert(f.content.presses == 1 && f.frame_presses == 0 && f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, 0);

    pointer(&f, UI_POINTER_MOTION, on_track, 0, 0);
    assert(ui_scrollview_is_shown(&f.sv));
    teardown(&f);
}

/* Without overlay the bar keeps its idle width in every state. The child
 * keeps the columns that the bar does not draw, even on hover. */
static void idle_bar_leaves_child_columns_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ui_scrollview_set_overlay(&f.sv, false);
    ui_surface_prepare(&f.surface);
    struct rect2i track = part(&f, "scroll-track");
    assert(track.width == 8 && track.x == 92);
    assert(f.content.widget.bounds.width == 92);

    /* The last 4 px of the child lie left of the track. */
    struct vector2i edge = { .x = 90, .y = 10 };
    pointer(&f, UI_POINTER_MOTION, edge, 0, 0);
    assert(part(&f, "scroll-track").width == 8);
    assert(pointer(&f, UI_POINTER_PRESS, edge, 0, 0));
    assert(f.content.presses == 1 && f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, edge, 0, 0);
    teardown(&f);
}

/* A press on the bar keeps focus where it was. A press on a child that is
 * not focusable clears focus, as before. */
static void bar_press_keeps_focus_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ui_scrollview_flash(&f.sv);
    struct probe_child other = {0};
    probe_init(&other, (struct vector2i){ .x = 10, .y = 10 });
    ui_widget_set_focusable(&other.widget, true);
    assert(ui_widget_add_child(&f.root, &other.widget));
    ui_widget_set_placement(&other.widget, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor = { .pos = { .x = 400, .y = 400 } },
    });
    ui_widget_set_fixed_size(&other.widget, (struct vector2i){ .x = 10, .y = 10 });
    ui_surface_prepare(&f.surface);
    ui_surface_focus(&f.surface, &other.widget);
    assert(ui_surface_focused(&f.surface) == &other.widget);

    struct vector2i on_track = center(part(&f, "scroll-track"));
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, 0));
    assert(ui_surface_focused(&f.surface) == &other.widget);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, 0);

    assert(pointer(&f, UI_POINTER_PRESS, (struct vector2i){ .x = 10, .y = 10 }, 0, 0));
    assert(f.content.presses == 1);
    assert(ui_surface_focused(&f.surface) == NULL);
    pointer(&f, UI_POINTER_RELEASE, (struct vector2i){ .x = 10, .y = 10 }, 0, 0);
    teardown(&f);
}

/* A press on blank space, below the child and off the bar, clears focus.
 * Only the track keeps it. */
static void blank_press_clears_focus_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 50);
    struct probe_child other = {0};
    probe_init(&other, (struct vector2i){ .x = 10, .y = 10 });
    ui_widget_set_focusable(&other.widget, true);
    assert(ui_widget_add_child(&f.root, &other.widget));
    ui_widget_set_placement(&other.widget, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor = { .pos = { .x = 400, .y = 400 } },
    });
    ui_widget_set_fixed_size(&other.widget, (struct vector2i){ .x = 10, .y = 10 });
    ui_surface_prepare(&f.surface);
    ui_surface_focus(&f.surface, &other.widget);

    struct vector2i blank = { .x = 10, .y = 150 };
    assert(!rect2i_contains(f.content.widget.bounds, blank));
    assert(rect2i_contains(f.sv.widget.bounds, blank));
    pointer(&f, UI_POINTER_PRESS, blank, 0, 0);
    assert(ui_surface_focused(&f.surface) == NULL);
    pointer(&f, UI_POINTER_RELEASE, blank, 0, 0);
    teardown(&f);
}

/* The child consumes motion, but the bar still shows under the pointer. The
 * child still gets every motion. */
static void hover_shows_bar_when_child_consumes_motion_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ui_scrollview_set_overlay(&f.sv, true);
    set_metrics(&f, 300, 30, 0);
    f.content.consume_motion = true;
    assert(!ui_scrollview_is_shown(&f.sv));

    /* The track is hit at its hover width while the bar is hidden. */
    struct vector2i on_track = { .x = 100 - 4, .y = 100 };
    pointer(&f, UI_POINTER_MOTION, on_track, 0, 0);
    assert(ui_scrollview_is_shown(&f.sv));
    assert(f.content.motions == 1);
    assert(part(&f, "scroll-track").width == 12);

    /* Off the track the child keeps the motion, and the bar goes idle. */
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 10, .y = 10 }, 0, 0);
    assert(f.content.motions == 2);
    assert(part(&f, "scroll-track").width == 8);
    teardown(&f);
}

/* The debug parts report the bar as it draws. A hidden bar reports nothing. */
static void debug_track_reports_drawn_bar_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    ui_scrollview_set_overlay(&f.sv, true);
    set_metrics(&f, 300, 30, 0);
    assert(rect2i_is_empty(part(&f, "scroll-track")));

    ui_scrollview_flash(&f.sv);
    assert(part(&f, "scroll-track").width == 8);
    assert(!rect2i_is_empty(part(&f, "scroll-thumb")));

    pointer(&f, UI_POINTER_MOTION, center(part(&f, "scroll-track")), 0, 0);
    assert(part(&f, "scroll-track").width == 12);

    ui_scrollview_hide(&f.sv);
    assert(rect2i_is_empty(part(&f, "scroll-track")));
    assert(rect2i_is_empty(part(&f, "scroll-thumb")));
    teardown(&f);
}

/* A child that asks for the text cursor everywhere it covers. */
static enum ui_cursor text_cursor(const struct ui_widget *widget,
                                  struct vector2i pos)
{
    (void)widget;
    (void)pos;
    return UI_CURSOR_TEXT;
}

/* Declines every event. It still takes the pointer, so it is hit. */
static bool text_event(struct ui_widget *widget, const struct ui_event *event)
{
    (void)widget;
    (void)event;
    return false;
}

static const struct ui_widget_ops TEXT_OPS = {
    .name = "text",
    .event = text_event,
    .cursor = text_cursor,
};

static enum ui_cursor cursor_after_motion(struct ui_surface *surface,
                                          struct vector2i pos)
{
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = pos,
    }, 0);
    ui_surface_dispatch(surface, &e);
    ui_surface_prepare(surface);
    return ui_surface_cursor(surface);
}

/* A selectable child covers the whole view, track included. The track must
 * still show the arrow, not the child's text cursor. The child keeps the
 * text cursor on its own body. */
static void track_cursor_is_not_child_cursor_test(void)
{
    const struct vector2i view = { .x = 100, .y = 200 };
    struct ui_scrollview sv = {0};
    struct ui_widget text = {0};
    struct ui_surface surface = {0};
    struct css_sheet sheet = {0};

    ui_scrollview_init(&sv);
    ui_widget_init(&text, &TEXT_OPS, NULL);
    ui_widget_set_fixed_size(&text, (struct vector2i){ .x = 100, .y = 400 });
    assert(ui_widget_add_child(&sv.widget, &text));
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_set_root(&surface, &sv.widget);
    ui_surface_resize(&surface, view);
    ui_surface_prepare(&surface);

    ui_scrollview_flash(&sv);
    ui_surface_prepare(&surface);
    struct rect2i track = named_part(&sv.widget, "scroll-track");
    assert(!rect2i_is_empty(track));
    assert(cursor_after_motion(&surface, center(track)) == UI_CURSOR_DEFAULT);

    assert(cursor_after_motion(&surface, (struct vector2i){ .x = 10, .y = 100 })
           == UI_CURSOR_TEXT);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&sv.widget);
    css_sheet_clear(&sheet);
}

/* Clearing the metrics returns to pixel mode. The wheel scrolls the child
 * again, and a drag on the thumb moves it without reporting an offset. */
static void clear_metrics_returns_to_pixel_mode_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    set_metrics(&f, 300, 30, 150);
    struct ui_event ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(!ui_surface_dispatch(&f.surface, &ev));

    ui_scrollview_clear_metrics(&f.sv);
    ui_surface_prepare(&f.surface);
    ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_surface_dispatch(&f.surface, &ev));
    assert(ui_scrollview_offset(&f.sv) == 48);
    ui_surface_prepare(&f.surface);
    assert(f.content.widget.bounds.pos.y == -48);

    struct rect2i thumb = part(&f, "scroll-thumb");
    struct rect2i track = part(&f, "scroll-track");
    assert(pointer(&f, UI_POINTER_PRESS, center(thumb), 0, 0));
    assert(pointer(&f, UI_POINTER_MOTION, rect2i_end(track), 0, 0));
    assert(ui_scrollview_offset(&f.sv) == max_offset(&f.sv));
    assert(f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, rect2i_end(track), 0, 0);
    teardown(&f);
}

/* Shown on hover and on flash, it hides at its own deadline once the
 * pointer leaves, and then needs no more ticks. */
static void hides_after_idle_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    show_overlay_bar(&f);
    struct vector2i on_track = center(part(&f, "scroll-track"));
    ui_scrollview_hide(&f.sv);

    assert(!ui_scrollview_is_shown(&f.sv));
    pointer(&f, UI_POINTER_MOTION, on_track, 0, 0);
    assert(ui_scrollview_is_shown(&f.sv));
    ui_surface_tick(&f.surface, 1000);
    assert(ui_surface_next_deadline(&f.surface) == 0);

    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 10, .y = 10 }, 0, 0);
    ui_surface_tick(&f.surface, 1000);
    uint64_t deadline = ui_surface_next_deadline(&f.surface);
    assert(deadline > 1000);
    ui_surface_tick(&f.surface, deadline - 1);
    assert(ui_scrollview_is_shown(&f.sv));
    ui_surface_tick(&f.surface, deadline);
    assert(!ui_scrollview_is_shown(&f.sv));
    assert(ui_surface_next_deadline(&f.surface) == 0);
    assert(bar_fills(paint(&f)) == 0);

    ui_scrollview_flash(&f.sv);
    run_to_hide(&f, 5000);
    assert(!ui_scrollview_is_shown(&f.sv));
    teardown(&f);
}

/* Overlay is on by default, so the bar starts hidden. Overlay off shows the
 * bar at all times. Overlay on again hides it. */
static void overlay_gates_bar_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    assert(!ui_scrollview_is_shown(&f.sv));
    assert(rect2i_is_empty(part(&f, "scroll-thumb")));
    assert(bar_fills(paint(&f)) == 0);

    ui_scrollview_flash(&f.sv);
    assert(ui_scrollview_is_shown(&f.sv));
    assert(bar_fills(paint(&f)) == 1);

    ui_scrollview_set_overlay(&f.sv, false);
    assert(ui_scrollview_is_shown(&f.sv));
    assert(!rect2i_is_empty(part(&f, "scroll-thumb")));

    ui_scrollview_set_overlay(&f.sv, true);
    assert(!ui_scrollview_is_shown(&f.sv));
    assert(rect2i_is_empty(part(&f, "scroll-thumb")));
    teardown(&f);
}

/* Pushing the same metrics every frame lets the window go idle. */
static void same_metrics_request_no_frame_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    ui_scrollview_flash(&f.sv);
    set_metrics(&f, 300, 30, 0);
    (void)ui_surface_take_frame_request(&f.surface);
    set_metrics(&f, 300, 30, 0);
    assert(!ui_surface_take_frame_request(&f.surface));
    set_metrics(&f, 300, 30, 5);
    assert(ui_surface_take_frame_request(&f.surface));
    teardown(&f);
}

/* Hiding mid-drag ends the drag. */
static void hide_ends_drag_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 200);
    show_overlay_bar(&f);
    assert(pointer(&f, UI_POINTER_PRESS, center(part(&f, "scroll-thumb")), 0, 0));
    ui_scrollview_hide(&f.sv);
    int changes = f.changes;
    /* Captured, this would scroll to the top. */
    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 10, .y = 0 }, 0, 0);
    assert(f.changes == changes);
    assert(!ui_scrollview_is_shown(&f.sv));
    teardown(&f);
}

/* Off, the bar takes no room, no press, and no part. On again, it returns. */
static void set_scrollbar_off_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ui_scrollview_set_overlay(&f.sv, false);
    ui_surface_prepare(&f.surface);
    f.content.handle_press = false;
    struct vector2i on_track = center(part(&f, "scroll-track"));

    ui_scrollview_set_scrollbar(&f.sv, false);
    assert(!ui_scrollview_is_shown(&f.sv));
    assert(rect2i_is_empty(part(&f, "scroll-track")));
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, 0));
    assert(f.frame_presses == 1 && f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, 0);

    ui_scrollview_set_scrollbar(&f.sv, true);
    assert(ui_scrollview_is_shown(&f.sv));
    assert(!rect2i_is_empty(part(&f, "scroll-track")));
    teardown(&f);
}

/* Dragging the thumb in pixel mode moves the child to the bottom. */
static void pixel_drag_scrolls_child_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 240, .y = 200 }, 1000);
    ui_scrollview_flash(&f.sv);
    assert(ui_scrollview_is_shown(&f.sv));
    struct rect2i thumb = part(&f, "scroll-thumb");
    struct rect2i track = part(&f, "scroll-track");
    assert(!rect2i_is_empty(thumb) && !rect2i_is_empty(track));

    struct ui_event event = press_event(thumb.pos, 0);
    assert(ui_surface_dispatch(&f.surface, &event));
    event.data.pointer.kind = UI_POINTER_MOTION;
    event.data.pointer.pos = rect2i_end(track);
    assert(ui_surface_dispatch(&f.surface, &event));
    assert(ui_scrollview_offset(&f.sv) == max_offset(&f.sv));
    event.data.pointer.kind = UI_POINTER_RELEASE;
    assert(ui_surface_dispatch(&f.surface, &event));
    assert(!ui_surface_captured(&f.surface));
    teardown(&f);
}

/* A pixel wheel step scrolls the view by three cell heights. */
static void pixel_wheel_scrolls_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    struct ui_event ev = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_surface_dispatch(&f.surface, &ev));
    assert(ui_scrollview_offset(&f.sv) == 48);
    teardown(&f);
}

/* The track and thumb styles come from the sheet. A new sheet restyles the
 * bar on the next prepare. The host sheet sits over the default sheet, so
 * the rules here set every size they need. */
static void part_sheet_drives_bar_test(void)
{
    struct fixture f;
    setup(&f,
        ".scrollbar { width: 12px; padding: 0; background: #112233; }"
        ".scrollbar-thumb { width: 12px; min-height: 40px; background: #445566; }",
        (struct vector2i){0}, (struct vector2i){ .x = 240, .y = 200 }, 1000);
    ui_scrollview_set_overlay(&f.sv, false);
    ui_surface_prepare(&f.surface);
    struct css_sheet narrow = {0};
    assert(css_parse(&narrow,
        ".scrollbar { width: 3px; padding: 0; }"
        ".scrollbar-thumb { width: 3px; min-height: 90px; background: #778899; }"));

    struct rect2i track = part(&f, "scroll-track");
    struct rect2i thumb = part(&f, "scroll-thumb");
    assert(track.width == 12 && track.x == 228);
    assert(thumb.width == 12 && thumb.height == 40);
    assert(f.content.widget.bounds.width == 228);
    const struct ui_scene *s = paint(&f);
    assert(has_fill_op(s, track, 0xff112233));
    assert(has_fill_op(s, thumb, 0xff445566));

    ui_surface_set_sheet(&f.surface, &narrow);
    ui_surface_prepare(&f.surface);
    track = part(&f, "scroll-track");
    thumb = part(&f, "scroll-thumb");
    assert(track.width == 3 && track.x == 237);
    assert(thumb.width == 3 && thumb.height == 90);
    assert(f.content.widget.bounds.width == 237);
    s = paint(&f);
    assert(has_fill_op(s, thumb, 0xff778899));

    teardown(&f);
    css_sheet_clear(&narrow);
}

/* The color of the fill that covers rect exactly, or 0 when none does. */
static uint32_t fill_color_at(const struct ui_scene *scene, struct rect2i rect)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (is_fill(&scene->ops[i]) && scene->ops[i].color != PROBE_COLOR &&
            rect2i_eq(scene->ops[i].rect, rect))
            return scene->ops[i].color;
    return 0;
}

/* Overlay is on by default. The bar starts hidden, so it draws nothing and
 * a press on its track reaches the child. Hover shows it, and it hides
 * after its delay. */
static void overlay_default_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    assert(!ui_scrollview_is_shown(&f.sv));
    assert(rect2i_is_empty(part(&f, "scroll-track")));
    assert(bar_fills(paint(&f)) == 0);

    /* The idle track is the rightmost 8 px. The child takes this press. */
    struct vector2i on_track = { .x = 96, .y = 50 };
    assert(pointer(&f, UI_POINTER_PRESS, on_track, 0, 0));
    assert(f.content.presses == 1 && f.changes == 0);
    pointer(&f, UI_POINTER_RELEASE, on_track, 0, 0);

    pointer(&f, UI_POINTER_MOTION, on_track, 0, 0);
    assert(ui_scrollview_is_shown(&f.sv));
    assert(!rect2i_is_empty(part(&f, "scroll-track")));

    pointer(&f, UI_POINTER_MOTION, (struct vector2i){ .x = 10, .y = 10 }, 0, 0);
    run_to_hide(&f, 1000);
    assert(!ui_scrollview_is_shown(&f.sv));
    teardown(&f);
}

/* Overlay takes no room. The bar widens over the child on hover, and the
 * child keeps its full width and its place. */
static void hover_widens_without_moving_child_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ui_scrollview_flash(&f.sv);
    struct rect2i child = f.content.widget.bounds;
    assert(child.width == 100);
    struct rect2i track = part(&f, "scroll-track");
    assert(track.width == 8 && rect2i_end(track).x == 100);

    pointer(&f, UI_POINTER_MOTION, center(track), 0, 0);
    track = part(&f, "scroll-track");
    assert(track.width == 12 && rect2i_end(track).x == 100);
    assert(rect2i_eq(f.content.widget.bounds, child));
    teardown(&f);
}

/* Hover colors the track with the sheet's hover color. The thumb keeps its
 * own hover color. */
static void track_hover_color_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    ui_scrollview_flash(&f.sv);
    const struct ui_scene *s = paint(&f);
    assert(fill_color_at(s, part(&f, "scroll-track")) == 0);
    assert(has_fill_op(s, part(&f, "scroll-thumb"), 0xFFA6ADC8));

    pointer(&f, UI_POINTER_MOTION, center(part(&f, "scroll-track")), 0, 0);
    s = paint(&f);
    /* The default sheet's track hover color: #bac2de at 15% alpha. */
    assert(fill_color_at(s, part(&f, "scroll-track")) == 0x26BAC2DE);
    assert(has_fill_op(s, part(&f, "scroll-thumb"), 0xFFBAC2DE));
    teardown(&f);
}

/* A wheel step that moves the view flashes the bar. A step that does not
 * move it leaves the bar hidden. A page key that moves the view flashes it
 * too. */
static void wheel_flashes_bar_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    struct ui_event up = scroll_event((struct vector2i){ .x = 5, .y = 5 }, 1);
    assert(!ui_surface_dispatch(&f.surface, &up));
    assert(!ui_scrollview_is_shown(&f.sv));

    struct ui_event down = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_surface_dispatch(&f.surface, &down));
    assert(ui_scrollview_offset(&f.sv) == 48);
    assert(ui_scrollview_is_shown(&f.sv));
    assert(!rect2i_is_empty(part(&f, "scroll-thumb")));

    ui_scrollview_hide(&f.sv);
    ui_widget_set_focusable(&f.content.widget, true);
    ui_surface_focus(&f.surface, &f.content.widget);
    struct ui_event page = ui_event_key((struct ui_key_event){
        .key = UI_KEY_PAGE_DOWN,
    }, 1);
    assert(ui_surface_dispatch(&f.surface, &page));
    assert(ui_scrollview_offset(&f.sv) > 48);
    assert(ui_scrollview_is_shown(&f.sv));
    teardown(&f);
}

/* A wheel step during a drag moves the view and keeps the drag. The bar
 * hides only after the drag ends and its delay passes. */
static void wheel_flash_keeps_drag_test(void)
{
    struct fixture f;
    setup(&f, NULL, (struct vector2i){0}, (struct vector2i){ .x = 100, .y = 200 }, 1000);
    struct ui_event wheel = scroll_event((struct vector2i){ .x = 5, .y = 5 }, -1);
    assert(ui_surface_dispatch(&f.surface, &wheel));
    assert(ui_scrollview_is_shown(&f.sv));

    struct rect2i track = part(&f, "scroll-track");
    assert(pointer(&f, UI_POINTER_PRESS, center(part(&f, "scroll-thumb")), 0, 0));
    assert(f.content.presses == 0);
    assert(pointer(&f, UI_POINTER_MOTION, rect2i_end(track), 0, 0));
    int bottom = max_offset(&f.sv);
    assert(ui_scrollview_offset(&f.sv) == bottom);

    wheel = scroll_event((struct vector2i){ .x = 5, .y = 5 }, 1);
    assert(ui_surface_dispatch(&f.surface, &wheel));
    assert(ui_scrollview_offset(&f.sv) == bottom - 48);
    assert(ui_surface_captured(&f.surface) == &f.sv.widget);
    assert(ui_scrollview_is_shown(&f.sv));

    assert(pointer(&f, UI_POINTER_MOTION, rect2i_end(track), 0, 0));
    assert(ui_scrollview_offset(&f.sv) == bottom);
    pointer(&f, UI_POINTER_RELEASE, rect2i_end(track), 0, 0);
    assert(!ui_surface_captured(&f.surface));

    run_to_hide(&f, 1000);
    assert(!ui_scrollview_is_shown(&f.sv));
    teardown(&f);
}

int main(void)
{
    test_zero();
    test_measure_layout();
    test_scroll_and_forward();
    test_extreme_scroll_deltas();
    test_press_hits_scrolled_child();
    test_paint_clips_viewport();
    test_offset_clamps_when_emptied();
    test_captured_child_declines_once();
    test_captured_scrollview_skips_children();
    test_focused_child_key_bubbles_once();
    test_key_bubble_stays_on_ancestor_chain();
    test_motion_and_leave_respect_viewport_clip();
    test_set_child_and_lifecycle();
    test_capture_released_before_fallback();
    css_drives_track_and_thumb_test();
    thumb_size_follows_metrics_test();
    drag_reports_offsets_test();
    line_mode_press_reports_offset_test();
    wheel_follows_mode_test();
    paints_where_it_hits_test();
    declined_press_reaches_parent_test();
    hidden_bar_takes_no_press_test();
    idle_bar_leaves_child_columns_test();
    bar_press_keeps_focus_test();
    blank_press_clears_focus_test();
    hover_shows_bar_when_child_consumes_motion_test();
    debug_track_reports_drawn_bar_test();
    clear_metrics_returns_to_pixel_mode_test();
    hides_after_idle_test();
    overlay_gates_bar_test();
    same_metrics_request_no_frame_test();
    hide_ends_drag_test();
    set_scrollbar_off_test();
    pixel_drag_scrolls_child_test();
    pixel_wheel_scrolls_test();
    part_sheet_drives_bar_test();
    overlay_default_test();
    hover_widens_without_moving_child_test();
    track_hover_color_test();
    wheel_flashes_bar_test();
    wheel_flash_keeps_drag_test();
    track_cursor_is_not_child_cursor_test();
    return 0;
}
