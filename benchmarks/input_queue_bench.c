#define _POSIX_C_SOURCE 200809L
#include "input/input.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static struct input_queue queue;

static uint64_t round_trip(const struct input_event *ev, long iterations)
{
    struct input_event in = *ev, out;
    uint64_t sink = 0;
    for (long i = 0; i < iterations; i++) {

        in.key.codepoint = (uint32_t)i;
        __asm__ volatile("" : "+m"(in));
        assert(input_queue_add(&queue, &in) != INPUT_PUSH_DROPPED);
        assert(input_queue_take(&queue, &out));
        __asm__ volatile("" : : "m"(out));
        sink += (uint64_t)out.kind + out.key.codepoint;
    }
    return sink;
}

static double drain_paste(const char *text, size_t len, long iterations)
{
    struct input_event out;
    uint64_t start = now_ns();
    for (long i = 0; i < iterations; i++) {
        assert(input_queue_push_paste(&queue, text, len) == len);
        while (input_queue_take(&queue, &out))
            input_event_release(&out);
    }
    return (double)(now_ns() - start) / (double)iterations;
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 5000000;
    size_t paste_len = argc > 2 ? (size_t)atol(argv[2]) : 64 * 1024;
    if (iterations < 1 || paste_len < 1 || paste_len > 16 * 1024 * 1024) {
        fprintf(stderr, "usage: %s [iterations:1..] [paste_bytes:1..16Mi]\n",
                argv[0]);
        return 1;
    }

    const struct input_event key = {
        .kind = INPUT_KEY,
        .key = {.key = INPUT_KEY_RETURN, .action = INPUT_PRESS},
    };
    const struct input_event motion = {
        .kind = INPUT_MOUSE,
        .mouse = {.kind = PTR_MOTION, .button = 1},
    };

    uint64_t sink = 0;
    uint64_t start = now_ns();
    sink += round_trip(&key, iterations);
    double key_ns = (double)(now_ns() - start) / (double)iterations;

    start = now_ns();
    sink += round_trip(&motion, iterations);
    double motion_ns = (double)(now_ns() - start) / (double)iterations;

    char *text = malloc(paste_len);
    assert(text);
    memset(text, 'x', paste_len);
    long paste_iterations = iterations / 1000 + 1;
    double paste_ns = drain_paste(text, paste_len, paste_iterations);
    free(text);
    input_queue_reset(&queue);

    printf("event %zu bytes\n", sizeof(struct input_event));
    printf("key   push+pop   %6.2f ns/event\n", key_ns);
    printf("motion push+pop  %6.2f ns/event\n", motion_ns);
    printf("paste %zu KiB    %6.2f us/paste (%.0f MiB/s)\n", paste_len / 1024,
           paste_ns / 1000.0,
           (double)paste_len / paste_ns * 1e9 / (1024.0 * 1024.0));
    return sink == 0;
}
