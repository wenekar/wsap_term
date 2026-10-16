/* The public event queue must retain FIFO order and discard events targeted
 * at a surface when that surface is destroyed. Native input is injected
 * through the backend queue, without requiring a compositor. */
#include "yasl/yasl.h"
#include "common/backend.h"

#include <assert.h>
#include <string.h>

static struct input_queue *surface_queue(struct yasl_surface *s)
{
    return &s->input;
}

static const struct yasl_private_backend_ops test_ops = {
    .surface_input_queue = surface_queue,
};

static void setup(struct yasl_private_display *display, struct yasl_surface *surface)
{
    *display = (struct yasl_private_display){.ops = &test_ops};
    *surface = (struct yasl_surface){.display = display, .scale_120 = 120};
    assert(yasl_private_events_open(display));
    assert(yasl_private_events_register(surface));
}

static void destroy_surface(struct yasl_surface *surface)
{
    yasl_private_events_unregister(surface);
    input_queue_clear(&surface->input);
}

static void test_fifo_and_destroy(void)
{
    struct yasl_private_display display;
    struct yasl_surface surface;
    setup(&display, &surface);

    assert(input_queue_push(&surface.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_KEY,
        .surface = &surface,
        .key = {.key = YASL_KEY_A, .action = YASL_KEY_PRESS},
    }));
    assert(input_queue_push(&surface.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_TEXT,
        .surface = &surface,
        .text = {.data = "hello", .len = 5},
    }));

    struct yasl_private_event event;
    assert(yasl_private_display_poll_event(&display, &event));
    assert(event.type == YASL_PRIVATE_EVENT_KEY && event.key.key == YASL_KEY_A);
    assert(yasl_private_display_poll_event(&display, &event));
    assert(event.type == YASL_PRIVATE_EVENT_TEXT && event.text.len == 5);
    assert(!strcmp(event.text.data, "hello"));

    /* Queue multiple events, then destroy the target before consumption. */
    assert(input_queue_push(&surface.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_KEY, .surface = &surface,
        .key = {.key = YASL_KEY_B, .action = YASL_KEY_PRESS},
    }));
    assert(input_queue_push(&surface.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_TEXT, .surface = &surface,
        .text = {.data = "discard", .len = 7},
    }));
    destroy_surface(&surface);
    assert(!yasl_private_display_poll_event(&display, &event));
    yasl_private_events_close(&display);

    /* Public event names remain the event stream consumed by applications. */
    assert(YASL_EVENT_KEY != YASL_EVENT_TEXT);
}

int main(void)
{
    test_fifo_and_destroy();
    return 0;
}
