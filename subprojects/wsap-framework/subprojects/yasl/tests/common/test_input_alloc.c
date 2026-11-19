#include "common/input.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* This executable compiles input.c with malloc redirected here. */
static int allocations = -1;
void *yasl_private_test_malloc(size_t bytes)
{
    if (allocations == 0) return NULL;
    if (allocations > 0) allocations--;
    return calloc(1, bytes);
}

/* input.c timestamps events, but this executable links no event loop. */
uint64_t yasl_private_loop_now_ns(void) { return 1; }

int main(void)
{
    struct yasl_surface *surface = (struct yasl_surface *)(uintptr_t)1;
    for (int failure = 0; failure < 2; failure++) {
        struct input_queue q = {0};
        struct yasl_private_event in = {.surface = surface, .type = YASL_PRIVATE_EVENT_PASTE,
                                     .text = {"payload", 7}}, out;
        /* Fail either payload allocation or the initial event-array allocation. */
        allocations = failure;
        assert(!input_queue_push(&q, &in));
        assert(input_queue_pop(&q, &out) && out.type == YASL_PRIVATE_EVENT_INPUT_RESET);
        assert(out.surface == surface);
        assert(!input_queue_pop(&q, &out));
        input_queue_clear(&q);

        allocations = -1;
        assert(input_queue_push(&q, &in));
        assert(input_queue_pop(&q, &out));
        const char *last = out.text.data;
        for (size_t i = 0; i < q.capacity; i++) assert(input_queue_push(&q, &in));
        allocations = failure;
        assert(!input_queue_push(&q, &in));
        assert(strcmp(last, "payload") == 0);
        assert(!q.count && q.reset_pending);
        allocations = -1;
        in = (struct yasl_private_event){.surface = surface, .type = YASL_PRIVATE_EVENT_KEY,
                                     .key.action = YASL_KEY_RELEASE};
        assert(input_queue_push(&q, &in));
        assert(input_queue_pop(&q, &out) && out.type == YASL_PRIVATE_EVENT_INPUT_RESET);
        assert(input_queue_pop(&q, &out) && out.key.action == YASL_KEY_RELEASE);
        input_queue_clear(&q);

        /* A lost event clears held buttons along with the stream. */
        in = (struct yasl_private_event){.surface = surface, .type = YASL_PRIVATE_EVENT_POINTER_DOWN};
        assert(input_queue_push(&q, &in) && q.buttons);
        for (size_t i = 1; i < q.capacity; i++) assert(input_queue_push(&q, &in));
        allocations = 0;
        assert(!input_queue_push(&q, &in));
        allocations = -1;
        assert(!q.buttons);
        input_queue_clear(&q);
    }
    return 0;
}
