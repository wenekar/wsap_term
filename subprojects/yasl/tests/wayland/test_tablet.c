#include "../../src/platform/wayland/pointer.c"
#include <assert.h>

static struct yasl_private_event next_raw(struct wayland_surface *surface)
{
    struct yasl_private_event event;
    assert(input_queue_pop(&surface->input, &event));
    assert(event.surface == &surface->base);
    return event;
}

static struct yasl_private_event next_pointer(struct wayland_surface *surface)
{
    struct yasl_private_event event;
    do { event = next_raw(surface); } while (event.type == YASL_PRIVATE_EVENT_PEN);
    return event;
}

int main(void)
{
    struct wayland_display display = {.base.ops = &yasl_private_wayland_backend};
    struct wayland_surface surface = {.base.display = &display.base, .conn = &display,
                                      .scale_factor = 1.5};
    struct seat seat = {.conn = &display, .tablet_focus = &surface};
    struct tablet_tool tool = {.seat = &seat, .device_id = 77, .tool_id = 88, .kind = 1};
    seat.tool = &tool;
    tablet_tool_motion(&tool, NULL, wl_fixed_from_int(100), wl_fixed_from_int(120));
    tool_pressure(&tool, NULL, 32768);
    tool_tilt(&tool, NULL, wl_fixed_from_int(-35), wl_fixed_from_int(42));
    tool_distance(&tool, NULL, 16384);
    tool_rotation(&tool, NULL, wl_fixed_from_int(17));
    tool_slider(&tool, NULL, -32768);
    pen_action_push(&tool, YASL_PRIVATE_PEN_ENTER, 0);
    tablet_tool_down(&tool, NULL, 1);
    tool_frame(&tool, NULL, 0);
    struct yasl_private_event pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN);
    assert(pen.pen.action == YASL_PRIVATE_PEN_ENTER && !pen.pen.in_contact);
    assert(pen.pen.device_id == 77 && pen.pen.tool_id == 88);
    pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN);
    assert(pen.pen.action == YASL_PRIVATE_PEN_DOWN && pen.pen.in_contact);
    assert(pen.pen.buttons & YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY));
    assert(pen.pen.x == 150 && pen.pen.y == 180);
    assert(fabs(pen.pen.pressure - (32768.0 / 65535.0)) < 0.00001);
    assert(pen.pen.axes_valid == (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT |
            YASL_PRIVATE_PEN_DISTANCE | YASL_PRIVATE_PEN_ROTATION | YASL_PRIVATE_PEN_TANGENTIAL));
    assert(pen.pen.axes_changed == pen.pen.axes_valid);
    assert(fabs(pen.pen.distance - 16384.0 / 65535.0) < 0.00001);
    assert(pen.pen.rotation == 17 && pen.pen.tangential == -32768.0 / 65535.0);
    struct yasl_private_event ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_MOTION && ev.pointer.device == YASL_PRIVATE_PTR_DEV_PEN);
    assert(ev.pointer.pos.x == 150 && ev.pointer.pos.y == 180 && ev.pointer.pen.pressure == 32768);
    assert(ev.pointer.pen.tilt_x == -35 && ev.pointer.pen.tilt_y == 42);
    ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_DOWN && ev.pointer.button == YASL_PRIVATE_BUTTON_PRIMARY && ev.pointer.pen.pressure == 32768);

    /* Stationary pressure and tilt changes are still motion samples. */
    tool_pressure(&tool, NULL, 100000);
    tool_tilt(&tool, NULL, wl_fixed_from_int(-120), wl_fixed_from_int(120));
    tool_frame(&tool, NULL, 0);
    pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN && pen.pen.action == YASL_PRIVATE_PEN_MOVE);
    assert(pen.pen.axes_changed == (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT));
    ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_MOTION && ev.pointer.pen.pressure == 65535);
    assert(ev.pointer.pos.x == 150 && ev.pointer.pos.y == 180);
    assert(ev.pointer.pen.tilt_x == -90 && ev.pointer.pen.tilt_y == 90);
    tablet_tool_up(&tool, NULL);
    tool_frame(&tool, NULL, 0);
    pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN && pen.pen.action == YASL_PRIVATE_PEN_UP);
    assert(!pen.pen.in_contact && pen.pen.pressure == 0);
    ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_UP && ev.pointer.button == YASL_PRIVATE_BUTTON_PRIMARY && !ev.pointer.pen.pressure);

    tool_type(&tool, NULL, ZWP_TABLET_TOOL_V2_TYPE_ERASER);
    tool_pressure(&tool, NULL, 22000);
    tool_frame(&tool, NULL, 0);
    pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN && pen.pen.tool == 2);
    assert(next_pointer(&surface).pointer.device == YASL_PRIVATE_PTR_DEV_ERASER);
    tool_wheel(&tool, NULL, 0, 1);
    tool_frame(&tool, NULL, 0);
    pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN && pen.pen.axes_changed == YASL_PRIVATE_PEN_WHEEL);
    ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_SCROLL && ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER);
    assert(ev.pointer.scroll.x == 0 && ev.pointer.scroll.y == 1);
    tablet_tool_proximity_out(&tool, NULL);
    pen = next_raw(&surface);
    assert(pen.type == YASL_PRIVATE_EVENT_PEN && pen.pen.action == YASL_PRIVATE_PEN_LEAVE);
    ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_LEAVE && ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER && !ev.pointer.pen.pressure);
    assert(!seat.tool && !seat.tablet_focus && !seat.tool_buttons);
    assert(!(yasl_private_display_caps(&display.base) & YASL_PRIVATE_CAP_TABLET));
    display.tablet_mgr = (void *)&display; /* Capability depends on protocol availability. */
    assert(yasl_private_display_caps(&display.base) & YASL_PRIVATE_CAP_TABLET);

    seat.pointer_focus = &surface;
    /* A compositor may describe the same detent in three ways. value120 wins,
     * and both axes belong to one event, without whole-line truncation. */
    pointer_axis(&seat, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    pointer_axis_discrete(&seat, NULL, WL_POINTER_AXIS_VERTICAL_SCROLL, 1);
    pointer_axis_value120(&seat, NULL, WL_POINTER_AXIS_VERTICAL_SCROLL, 30);
    pointer_axis_value120(&seat, NULL, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -60);
    pointer_frame(&seat, NULL);
    ev = next_pointer(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_SCROLL && ev.pointer.device == YASL_PRIVATE_PTR_DEV_MOUSE);
    assert(ev.pointer.scroll.x == -0.5 && ev.pointer.scroll.y == 0.25);
    assert(ev.pointer.scroll_valid == 1);
    assert(ev.pointer.scroll_source == 1 && ev.pointer.scroll_phase == 0);
    struct yasl_private_event unused;
    assert(!input_queue_pop(&surface.input, &unused));
    pointer_frame(&seat, NULL);
    assert(!input_queue_pop(&surface.input, &unused));

    /* Finger scrolling carries pixel offsets and coherent gesture phases. */
    pointer_axis_source(&seat, NULL, WL_POINTER_AXIS_SOURCE_FINGER);
    pointer_axis(&seat, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(1.25));
    pointer_axis(&seat, NULL, 0, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(-2.5));
    pointer_frame(&seat, NULL);
    ev = next_pointer(&surface);
    assert(ev.pointer.scroll_phase == 1 && ev.pointer.scroll_source == 2);
    assert(ev.pointer.pixel_scroll.x == -3.75 && ev.pointer.pixel_scroll.y == 1.875);
    pointer_axis(&seat, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(1.25));
    pointer_frame(&seat, NULL);
    ev = next_pointer(&surface);
    assert(ev.pointer.scroll_phase == 2 && ev.pointer.scroll.y == 0.125);
    pointer_axis_stop(&seat, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL);
    pointer_frame(&seat, NULL);
    ev = next_pointer(&surface);
    assert(ev.pointer.scroll_phase == 3 && ev.pointer.scroll.x == 0 && ev.pointer.scroll.y == 0);

    /* Stops preserve offsets from the same frame. Repeated tiny movements
     * must be visible immediately rather than held until a whole detent. */
    for (int i = 0; i < 3; i++) {
        pointer_axis_source(&seat, NULL, WL_POINTER_AXIS_SOURCE_FINGER);
        pointer_axis(&seat, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(1.25));
        pointer_axis(&seat, NULL, 0, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(-2.5));
        pointer_axis_stop(&seat, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL);
        pointer_frame(&seat, NULL);
        ev = next_pointer(&surface);
        assert(ev.pointer.scroll.x == -0.25 && ev.pointer.scroll.y == 0.125);
        assert(ev.pointer.scroll_source == 2 && ev.pointer.scroll_phase == 3);
        assert(ev.pointer.pixel_scroll.x == -3.75 && ev.pointer.pixel_scroll.y == 1.875);
    }
    pointer_axis_discrete(&seat, NULL, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 2);
    pointer_frame(&seat, NULL);
    ev = next_pointer(&surface);
    assert(ev.pointer.scroll.x == 2 && ev.pointer.scroll.y == 0);
    pointer_axis_value120(&seat, NULL, WL_POINTER_AXIS_VERTICAL_SCROLL, 60);
    seat_pointer_reset(&seat);
    pointer_frame(&seat, NULL);
    assert(!input_queue_pop(&surface.input, &unused));
    input_queue_clear(&surface.input);
    return 0;
}
