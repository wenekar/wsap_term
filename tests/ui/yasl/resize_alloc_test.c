#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/yasl/stats.h"
#include "font/glyph_cache.h"
#include "yasl/yasl.h"
#include "render/render.h"
#include "helpers/render_frame_helpers.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif

static unsigned long allocs __attribute__((unused));
static int counting;

#ifndef SANITIZED

void *malloc(size_t n)
{
    static void *(*real)(size_t);
    if (!real) real = dlsym(RTLD_NEXT, "malloc");
    if (counting) allocs++;
    return real(n);
}

void *calloc(size_t n, size_t sz)
{
    static void *(*real)(size_t, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "calloc");
    if (counting) allocs++;
    return real(n, sz);
}

void *realloc(void *p, size_t n)
{
    static void *(*real)(void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "realloc");
    if (counting) allocs++;
    return real(p, n);
}

#endif

static const int sizes[][2] = {
    { 800, 600 }, { 1024, 768 }, { 640, 480 },
    { 1280, 720 }, { 720, 540 }, { 960, 540 },
};
static const int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

#define MAX_W 1280
#define MAX_H 800

static void paint(struct frame_stats *st, uint8_t *pool, int i, int cw, int ch)
{
    size_t off = (i & 1) ? (size_t)cw * 4 * (size_t)ch : 0;
    frame_stats_tick(st);
    render_begin(st);
        test_render_paint_full((uint32_t *)(pool + off), cw, ch, true, NULL);
    render_end_and_count_frame(st);
}

int main(void)
{
    render_init();

    size_t cap = (size_t)MAX_W * 4 * (size_t)MAX_H * 2;
    uint8_t *pool = malloc(cap);
    assert(pool);
    memset(pool, 0x10, cap);

    struct frame_stats st = { .started_ns = yasl_time_ns(), .frame_min_ms = 1e18 };

    for (int i = 0; i < nsizes * 2; i++)
        paint(&st, pool, i, sizes[i % nsizes][0], sizes[i % nsizes][1]);

    counting = 1;
    unsigned resizes = 0;
    int cw = sizes[0][0], ch = sizes[0][1];
    for (int i = 0; i < 240; i++) {
        if (i % 20 == 0) {
            const int *s = sizes[(i / 20) % nsizes];
            cw = s[0] > MAX_W ? MAX_W : s[0];
            ch = s[1] > MAX_H ? MAX_H : s[1];
            resizes++;
        }
        paint(&st, pool, i, cw, ch);
    }
    counting = 0;

    free(pool);
    render_shutdown();

#ifdef SANITIZED
    printf("resize alloc test: churn ran over %u resizes, allocation count "
           "not checked under sanitizer\n", resizes);
    return 0;
#else
    if (allocs != 0) {
        fprintf(stderr, "resize churn allocated %lu times over %u resizes\n",
                allocs, resizes);
        return 1;
    }
    printf("resize alloc test passed (%u resizes, no allocation)\n", resizes);
    return 0;
#endif
}
