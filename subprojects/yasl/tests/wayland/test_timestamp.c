/* A compositor input event keeps its source time through the input queue.
 * An event with no source time gets the queue time instead. */
#include "platform/wayland/internal.h"
#include <assert.h>

#include "../../src/platform/wayland/pointer.c"

static struct yasl_private_event next(struct wayland_surface *w,
                                      enum yasl_private_event_type type)
{
    struct yasl_private_event ev;
    assert(input_queue_pop(&w->input, &ev));
    assert(ev.type == type && ev.surface == &w->base);
    return ev;
}

int main(void)
{
    struct wayland_display display = {.base.ops = &yasl_private_wayland_backend};
    struct wayland_surface surface = {.base.display = &display.base, .conn = &display,
        .scale_factor = 1};
    struct seat seat = {.conn = &display, .pointer_focus = &surface};

    /* Motion keeps the compositor time, in milliseconds. */
    pointer_motion(&seat, NULL, 1234, wl_fixed_from_int(3), wl_fixed_from_int(4));
    pointer_frame(&seat, NULL);
    struct yasl_private_event ev = next(&surface, YASL_PRIVATE_EVENT_POINTER_MOTION);
    assert(ev.time_ns == 1234000000ull);

    /* A press waits for its release, and keeps the time of the press. */
    pointer_button(&seat, NULL, 1, 2000, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    pointer_button(&seat, NULL, 2, 3000, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    pointer_frame(&seat, NULL);
    ev = next(&surface, YASL_PRIVATE_EVENT_POINTER_DOWN);
    assert(ev.time_ns == 2000000000ull);
    ev = next(&surface, YASL_PRIVATE_EVENT_POINTER_UP);
    assert(ev.time_ns == 3000000000ull);

    /* Scroll offsets keep the time of the axis event in their frame. */
    pointer_axis(&seat, NULL, 4000, 0, wl_fixed_from_int(10));
    pointer_frame(&seat, NULL);
    ev = next(&surface, YASL_PRIVATE_EVENT_POINTER_SCROLL);
    assert(ev.time_ns == 4000000000ull);

    /* An event with no source time gets the queue time. */
    uint64_t before = yasl_private_loop_now_ns();
    struct yasl_private_event synthesized = {
        .type = YASL_PRIVATE_EVENT_INPUT_RESET, .surface = &surface.base,
    };
    assert(input_queue_push(&surface.input, &synthesized));
    assert(input_queue_pop(&surface.input, &ev));
    assert(ev.type == YASL_PRIVATE_EVENT_INPUT_RESET);
    assert(ev.time_ns >= before && ev.time_ns <= yasl_private_loop_now_ns());

    free(seat.ptr_presses.ev);
    input_queue_clear(&surface.input);
    return 0;
}
