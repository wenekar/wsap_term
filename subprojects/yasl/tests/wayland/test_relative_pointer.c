#include "platform/wayland/internal.h"
#include <assert.h>

/* Record lock, relative-pointer and cursor requests without a compositor. */
static char lock_objects[4];
static int locks, unlocks, pointer_hides, pointer_shapes;
static struct wl_surface *locked_surface;
static struct wl_pointer *locked_wl_pointer;
static uint32_t lock_lifetime;

static struct zwp_locked_pointer_v1 *lock_pointer(struct zwp_pointer_constraints_v1 *mgr,
        struct wl_surface *surface, struct wl_pointer *pointer,
        struct wl_region *region, uint32_t lifetime)
{
    assert(mgr && surface && pointer && !region);
    locked_surface = surface;
    locked_wl_pointer = pointer;
    lock_lifetime = lifetime;
    return (struct zwp_locked_pointer_v1 *)&lock_objects[locks++ % 4];
}

static int lock_add_listener(struct zwp_locked_pointer_v1 *lock,
                             const struct zwp_locked_pointer_v1_listener *listener, void *data)
{
    assert(lock && listener && data);
    return 0;
}

static void lock_destroy(struct zwp_locked_pointer_v1 *lock)
{
    assert(lock);
    unlocks++;
}

static void pointer_cursor(struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, int32_t x, int32_t y)
{
    assert(pointer && serial && !surface && !x && !y);
    pointer_hides++;
}

static void cursor_shape(struct wp_cursor_shape_device_v1 *device,
                         uint32_t serial, uint32_t shape)
{
    assert(device && serial && shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    pointer_shapes++;
}

static uint32_t pointer_version(struct wl_pointer *pointer)
{
    assert(pointer);
    return WL_POINTER_FRAME_SINCE_VERSION;
}

static void pointer_release(struct wl_pointer *pointer) { assert(pointer); }
static void shape_device_destroy(struct wp_cursor_shape_device_v1 *device) { assert(device); }

#define zwp_pointer_constraints_v1_lock_pointer lock_pointer
#define zwp_locked_pointer_v1_add_listener lock_add_listener
#define zwp_locked_pointer_v1_destroy lock_destroy
#define wl_pointer_set_cursor pointer_cursor
#define wp_cursor_shape_device_v1_set_shape cursor_shape
#define wp_cursor_shape_device_v1_destroy shape_device_destroy
#define wl_pointer_get_version pointer_version
#define wl_pointer_release pointer_release
#include "../../src/platform/wayland/pointer.c"

static struct yasl_private_event next_event(struct wayland_surface *w)
{
    struct yasl_private_event event;
    assert(input_queue_pop(&w->input, &event));
    return event;
}

static void move_to(struct seat *s, int x, int y)
{
    pointer_motion(s, s->pointer, 0, wl_fixed_from_int(x), wl_fixed_from_int(y));
    pointer_frame(s, s->pointer);
}

static void relative_move(struct seat *s, double dx, double dy)
{
    relative_motion(s, NULL, 0, 0, wl_fixed_from_double(dx), wl_fixed_from_double(dy), 0, 0);
}

int main(void)
{
    struct wayland_display display = {.base.ops = &yasl_private_wayland_backend};
    struct wayland_surface surface = {.base.display = &display.base, .conn = &display,
        .scale_factor = 2, .cursor = YASL_PRIVATE_CURSOR_TEXT};
    surface.surface = (struct wl_surface *)&surface;
    wl_surface_set_user_data(surface.surface, &surface);
    display.surfaces = &surface;
    struct seat seat = {.conn = &display, .cursor_shape = -1};
    seat.pointer = (struct wl_pointer *)&seat;
    seat.cursor_shape_dev = (struct wp_cursor_shape_device_v1 *)&seat;
    display.seats = &seat;
    struct yasl_private_event event;

    /* Without both protocols the surface stays in absolute mode. */
    assert(!yasl_private_surface_set_pointer_mode(&surface.base, YASL_PRIVATE_POINTER_RELATIVE));
    assert(!surface.relative && !locks);
    display.relative_pointer_mgr = (struct zwp_relative_pointer_manager_v1 *)&display;
    display.pointer_constraints = (struct zwp_pointer_constraints_v1 *)&display;

    pointer_enter(&seat, seat.pointer, 10, surface.surface, wl_fixed_from_int(5), wl_fixed_from_int(5));
    pointer_frame(&seat, seat.pointer);
    assert(pointer_shapes == 1);
    event = next_event(&surface);
    assert(event.type == YASL_PRIVATE_EVENT_POINTER_MOTION && event.pointer.pos.x == 10);

    /* Relative mode locks persistently on the pointer's seat and hides the cursor. */
    assert(yasl_private_surface_set_pointer_mode(&surface.base, YASL_PRIVATE_POINTER_RELATIVE));
    assert(locks == 1 && locked_surface == surface.surface && locked_wl_pointer == seat.pointer);
    assert(lock_lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    assert(pointer_hides == 1 && surface.lock_seat == &seat);

    /* Until the compositor activates the lock, motion is absolute with a queue delta. */
    relative_move(&seat, 3, 3);
    move_to(&seat, 7, 5);
    event = next_event(&surface);
    assert(event.pointer.pos.x == 14 && event.pointer.pos.y == 10);
    assert(event.pointer.delta.x == 4 && event.pointer.delta.y == 0);
    assert(!input_queue_pop(&surface.input, &event));

    /* Locked: relative samples in device pixels, position frozen at the lock. */
    pointer_locked(&surface, surface.pointer_lock);
    relative_move(&seat, 3.5, -2);
    relative_move(&seat, 1000, 0);
    event = next_event(&surface);
    assert(event.type == YASL_PRIVATE_EVENT_POINTER_MOTION);
    assert(event.pointer.pos.x == 14 && event.pointer.pos.y == 10);
    assert(event.pointer.delta.x == 7 && event.pointer.delta.y == -4);
    event = next_event(&surface);
    assert(event.pointer.delta.x == 2000 && event.pointer.pos.x == 14);

    /* Deactivation (e.g. focus loss) returns to absolute motion without unlocking. */
    pointer_unlocked(&surface, surface.pointer_lock);
    relative_move(&seat, 5, 5);
    assert(!input_queue_pop(&surface.input, &event));
    move_to(&seat, 8, 5);
    event = next_event(&surface);
    assert(event.pointer.delta.x == 2 && event.pointer.delta.y == 0);
    assert(unlocks == 0 && surface.pointer_lock);
    pointer_locked(&surface, surface.pointer_lock);

    /* Returning to normal destroys the lock and restores the chosen cursor. */
    assert(yasl_private_surface_set_pointer_mode(&surface.base, YASL_PRIVATE_POINTER_NORMAL));
    assert(unlocks == 1 && !surface.pointer_lock && !surface.input.relative);
    assert(pointer_shapes == 2);
    assert(yasl_private_surface_set_pointer_mode(&surface.base, YASL_PRIVATE_POINTER_NORMAL));
    assert(unlocks == 1);
    assert(yasl_private_surface_set_pointer_mode(&surface.base, YASL_PRIVATE_POINTER_RELATIVE));
    assert(locks == 2 && pointer_hides == 2);
    pointer_locked(&surface, surface.pointer_lock);

    /* Losing the seat's pointer hands the lock to another seat with a pointer. */
    struct seat other = {.conn = &display, .cursor_shape = -1};
    other.pointer = (struct wl_pointer *)&other;
    seat.next = &other;
    seat_drop_pointer(&seat);
    assert(unlocks == 2 && locks == 3 && locked_wl_pointer == other.pointer);
    assert(surface.lock_seat == &other && !surface.input.relative);

    /* Surface destruction releases the lock through surface_unlock_pointer(). */
    surface_unlock_pointer(&surface);
    assert(unlocks == 3 && !surface.lock_seat && !surface.pointer_lock);
    input_queue_clear(&surface.input);
    return 0;
}
