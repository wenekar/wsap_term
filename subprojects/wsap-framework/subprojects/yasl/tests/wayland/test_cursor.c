#include "platform/wayland/internal.h"
#include <assert.h>

/* Record cursor requests without a compositor or tablet hardware. */
static int pointer_hides, tool_hides, pointer_shapes, tool_shapes;
static uint32_t pointer_serial, tool_serial, pointer_shape, tool_shape;
static struct wp_cursor_shape_device_v1 *pointer_device;

static void pointer_cursor(struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, int32_t x, int32_t y)
{
    assert(pointer && !surface && !x && !y);
    pointer_hides++;
    pointer_serial = serial;
}

static void tool_cursor(struct zwp_tablet_tool_v2 *tool, uint32_t serial,
                        struct wl_surface *surface, int32_t x, int32_t y)
{
    assert(tool && !surface && !x && !y);
    tool_hides++;
    tool_serial = serial;
}

static void cursor_shape(struct wp_cursor_shape_device_v1 *device,
                         uint32_t serial, uint32_t shape)
{
    if (device == pointer_device) {
        pointer_shapes++;
        pointer_serial = serial;
        pointer_shape = shape;
    } else {
        assert(device);
        tool_shapes++;
        tool_serial = serial;
        tool_shape = shape;
    }
}

static uint32_t pointer_version(struct wl_pointer *pointer)
{
    assert(pointer);
    return WL_POINTER_FRAME_SINCE_VERSION;
}

#define wl_pointer_set_cursor pointer_cursor
#define zwp_tablet_tool_v2_set_cursor tool_cursor
#define wp_cursor_shape_device_v1_set_shape cursor_shape
#define wl_pointer_get_version pointer_version
#include "../../src/platform/wayland/pointer.c"

int main(void)
{
    struct wayland_display display = {.base.ops = &yasl_private_wayland_backend};
    struct wayland_surface surface = {.base.display = &display.base, .conn = &display,
        .scale_factor = 1, .cursor = YASL_PRIVATE_CURSOR_TEXT};
    surface.surface = (struct wl_surface *)&surface;
    wl_surface_set_user_data(surface.surface, &surface);
    display.surfaces = &surface;
    struct seat seat = {.conn = &display};
    seat.pointer = (struct wl_pointer *)&seat;
    display.seats = &seat;
    struct tablet_tool tool = {.seat = &seat};
    tool.proxy = (struct zwp_tablet_tool_v2 *)&tool;

    /* The example selects hidden before either input device has entered. */
    yasl_private_surface_set_cursor(&surface.base, YASL_PRIVATE_CURSOR_HIDDEN);
    assert(!pointer_hides && !tool_hides);
    pointer_enter(&seat, seat.pointer, 10, surface.surface, 0, 0);
    assert(pointer_hides == 1 && pointer_serial == 10);
    tablet_tool_proximity_in(&tool, tool.proxy, 11, NULL, surface.surface);
    assert(tool_hides == 1 && tool_serial == 11);

    /* Hiding also works with no cursor-shape protocol, and preserves input. */
    pointer_motion(&seat, seat.pointer, 0, wl_fixed_from_int(12), wl_fixed_from_int(34));
    pointer_frame(&seat, seat.pointer);
    tablet_tool_motion(&tool, tool.proxy, wl_fixed_from_int(56), wl_fixed_from_int(78));
    tool_frame(&tool, tool.proxy, 0);
    struct yasl_private_event event;
    assert(input_queue_pop(&surface.input, &event));
    assert(event.type == YASL_PRIVATE_EVENT_POINTER_MOTION && event.pointer.device == YASL_PRIVATE_PTR_DEV_MOUSE);
    assert(event.pointer.pos.x == 12 && event.pointer.pos.y == 34);
    assert(input_queue_pop(&surface.input, &event));
    assert(event.type == YASL_PRIVATE_EVENT_PEN && event.pen.action == YASL_PRIVATE_PEN_ENTER);
    assert(input_queue_pop(&surface.input, &event));
    assert(event.type == YASL_PRIVATE_EVENT_POINTER_MOTION && event.pointer.device == YASL_PRIVATE_PTR_DEV_PEN);
    assert(event.pointer.pos.x == 56 && event.pointer.pos.y == 78);
    assert(!input_queue_pop(&surface.input, &event));

    /* Each new entry needs a request with its own serial. */
    pointer_leave(&seat, seat.pointer, 12, surface.surface);
    pointer_frame(&seat, seat.pointer);
    tablet_tool_proximity_out(&tool, tool.proxy);
    pointer_enter(&seat, seat.pointer, 13, surface.surface, 0, 0);
    tool.eraser = true;
    tablet_tool_proximity_in(&tool, tool.proxy, 14, NULL, surface.surface);
    assert(pointer_hides == 2 && pointer_serial == 13);
    assert(tool_hides == 2 && tool_serial == 14);

    /* Restore a visible shape on both currently focused devices. */
    pointer_device = seat.cursor_shape_dev = (struct wp_cursor_shape_device_v1 *)&seat;
    tool.shape_dev = (struct wp_cursor_shape_device_v1 *)&tool;
    yasl_private_surface_set_cursor(&surface.base, YASL_PRIVATE_CURSOR_ARROW);
    assert(pointer_shapes == 1 && tool_shapes == 1);
    assert(pointer_shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    assert(tool_shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    yasl_private_surface_set_cursor(&surface.base, YASL_PRIVATE_CURSOR_HIDDEN);
    assert(pointer_hides == 3 && tool_hides == 3);
    assert(pointer_shapes == 1 && tool_shapes == 1);

    /* Updating an unfocused surface must not alter the active cursor. */
    struct wayland_surface other = {.base.display = &display.base, .conn = &display};
    yasl_private_surface_set_cursor(&other.base, YASL_PRIVATE_CURSOR_CROSSHAIR);
    assert(pointer_shapes == 1 && tool_shapes == 1);
    yasl_private_surface_set_cursor(&surface.base, (enum yasl_private_cursor)-1);
    yasl_private_surface_set_cursor(&surface.base, (enum yasl_private_cursor)(YASL_PRIVATE_CURSOR_HIDDEN + 1));
    assert(pointer_hides == 3 && tool_hides == 3);
    assert(pointer_shapes == 1 && tool_shapes == 1);
    input_queue_clear(&surface.input);
    return 0;
}
