#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "input/input.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/testing.h"
#include "ui/yasl/window.h"

/*
 * Decision of the codebase owner (2026-11-19):
 * - A paste always goes to the focused widget.
 * - Only a drop release goes to the drop target, and the drop location on the
 *   window surface gives it.
 * - A text drop marks only a widget inside the focused subtree.
 *
 * The file drop case uses the order of a real backend: the drag enters, the
 * LEAVE comes, and then the release lands.
 */

struct probe {
    struct ui_widget widget;
    int enter;
    int leave;
    int drop;
    struct vector2i drop_pos;
};

static struct window_host host;

static bool probe_event(struct ui_widget *widget, const struct ui_event *event);

static const struct ui_widget_ops PROBE_OPS = {
    .name = "drop-route-probe",
    .event = probe_event,
};

static bool probe_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe *p = widget->userdata;
    if (event->kind == UI_EVENT_DRAG_ENTER || event->kind == UI_EVENT_DRAG_OVER)
        p->enter++;
    if (event->kind == UI_EVENT_DRAG_LEAVE)
        p->leave++;
    if (event->kind == UI_EVENT_DROP) {
        p->drop++;
        p->drop_pos = event->data.drag.pos;
    }
    if (event->kind == UI_EVENT_DROP_FILES || event->kind == UI_EVENT_PASTE)
        p->drop++;
    /* The widget takes the drop, so it is the receiver of the drag. */
    return true;
}

struct fixture {
    struct ui_widget root;
    struct probe pane;
    struct probe mark;
    struct window *window;
};

static void show(struct fixture *f)
{
    *f = (struct fixture){0};
    struct window_config config = { .size = { .x = 320, .y = 200 } };
    window_host_init(&host, NULL, &config, NULL);
    f->window = window_create(&host, &(struct window_spec){0});
    assert(f->window);
    ui_widget_init(&f->root, NULL, NULL);
    ui_widget_init(&f->pane.widget, &PROBE_OPS, &f->pane);
    ui_widget_init(&f->mark.widget, &PROBE_OPS, &f->mark);
    /* The pane owns the top half of the window, and the mark the bottom. */
    ui_widget_set_fixed_size(&f->pane.widget, (struct vector2i){ .x = 320, .y = 100 });
    ui_widget_set_fixed_size(&f->mark.widget, (struct vector2i){ .x = 320, .y = 100 });
    ui_widget_set_placement(&f->pane.widget, (struct ui_placement){
        .kind = UI_PLACE_AT,
    });
    ui_widget_set_placement(&f->mark.widget, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor.pos = { .y = 100 },
    });
    ui_widget_set_focusable(&f->pane.widget, true);
    assert(ui_widget_add_child(&f->root, &f->pane.widget));
    assert(ui_widget_add_child(&f->root, &f->mark.widget));
    assert(window_ui_mount(f->window, &(struct window_ui_spec){ .root = &f->root }));
    /* A window with no native surface gets no relayout, so the test sizes the
     * screen itself. */
    ui_surface_resize(window_ui_key_surface(f->window),
                      (struct vector2i){ .x = 320, .y = 200 });
}

static void teardown(struct fixture *f)
{
    window_destroy(f->window);
    window_host_registry_remove(&host);
    ui_widget_destroy(&f->root);
}

static void drag(struct fixture *f, enum input_kind kind, const char *mime,
                 struct vector2i pos)
{
    struct input_event ev = {
        .kind = kind,
        .drop_type = { .mime = (char *)mime, .pos = pos },
    };
    window_ui_route(f->window, &ev);
}

static void leave(struct fixture *f, struct vector2i pos)
{
    struct input_event ev = {
        .kind = INPUT_DRAG,
        .drop_type = { .mime = (char *)"", .pos = pos, .leave = true },
    };
    window_ui_route(f->window, &ev);
}

static void drop(struct fixture *f, const char *mime, struct vector2i pos)
{
    struct input_event ev = {
        .kind = INPUT_DROP,
        .drop_type = { .mime = (char *)mime, .data = (char *)"x", .len = 1,
                       .pos = pos },
    };
    window_ui_route(f->window, &ev);
}

/* A text drop goes to the focused widget. A drag over another widget draws no
 * mark there, and the other widget takes no drop. */
static void text_drag_marks_no_other_widget_test(void)
{
    struct fixture f;
    show(&f);
    ui_surface_focus(window_ui_key_surface(f.window), &f.pane.widget);
    struct vector2i over_mark = { .x = 40, .y = 150 };
    drag(&f, INPUT_DRAG, "text/plain;charset=utf-8", over_mark);
    assert(f.mark.enter == 0);
    assert(f.pane.enter == 0);
    /* The drop still goes to the focused pane. */
    drop(&f, "text/plain;charset=utf-8", over_mark);
    assert(f.mark.drop == 0);
    teardown(&f);
}

/* A drag of a type that lands at its position draws the mark there, and the
 * drop goes to the widget that drew it. */
static void position_drop_goes_to_the_marked_widget_test(void)
{
    struct fixture f;
    show(&f);
    ui_surface_focus(window_ui_key_surface(f.window), &f.pane.widget);
    struct vector2i over_mark = { .x = 40, .y = 150 };
    drag(&f, INPUT_DRAG, "image/png", over_mark);
    assert(f.mark.enter == 1);
    assert(f.pane.enter == 0);
    drop(&f, "image/png", over_mark);
    assert(f.mark.drop == 1);
    assert(f.mark.drop_pos.x == over_mark.x &&
           f.mark.drop_pos.y == over_mark.y);
    teardown(&f);
}

/* A file drop goes to the widget at its drop location, also after the LEAVE
 * that Wayland and X11 send before the release. */
static void file_drop_goes_to_the_marked_widget_test(void)
{
    struct fixture f;
    show(&f);
    ui_surface_focus(window_ui_key_surface(f.window), &f.pane.widget);
    struct vector2i over_mark = { .x = 40, .y = 150 };
    drag(&f, INPUT_DRAG, "text/uri-list", over_mark);
    assert(f.mark.enter == 1);
    leave(&f, over_mark);
    assert(f.mark.leave == 1);
    struct input_event ev = {
        .kind = INPUT_DROP_FILES,
        .drop = { .data = (char *)"file:///tmp/a\r\n", .len = 17,
                  .pos = over_mark, .dropped = true },
    };
    window_ui_route(f.window, &ev);
    assert(f.mark.drop == 1);
    teardown(&f);
}

/* A text drag over the focused widget draws its mark, because that widget
 * takes the drop. */
static void text_drag_marks_the_focused_widget_test(void)
{
    struct fixture f;
    show(&f);
    ui_surface_focus(window_ui_key_surface(f.window), &f.pane.widget);
    struct vector2i over_pane = { .x = 40, .y = 20 };
    drag(&f, INPUT_DRAG, "text/plain", over_pane);
    assert(f.pane.enter == 1);
    assert(f.mark.enter == 0);
    teardown(&f);
}

/* A release ends the drag. The widget that the drag was over gets one LEAVE,
 * and the surface holds no drop target. */
static void release_ends_the_drag_test(void)
{
    struct fixture f;
    show(&f);
    ui_surface_focus(window_ui_key_surface(f.window), &f.pane.widget);
    struct vector2i over_mark = { .x = 40, .y = 150 };
    drag(&f, INPUT_DRAG, "image/png", over_mark);
    assert(f.mark.enter == 1);
    drop(&f, "image/png", over_mark);
    assert(f.mark.drop == 1);
    assert(f.mark.leave == 1);
    assert(!ui_surface_drag_target(window_ui_key_surface(f.window)));
    teardown(&f);
}

int main(void)
{
    text_drag_marks_no_other_widget_test();
    position_drop_goes_to_the_marked_widget_test();
    text_drag_marks_the_focused_widget_test();
    file_drop_goes_to_the_marked_widget_test();
    release_ends_the_drag_test();
    return 0;
}
