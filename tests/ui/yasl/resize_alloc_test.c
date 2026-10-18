#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/yasl/stats.h"
#include "font/font.h"
#include "yasl/yasl.h"
#include "render/cpu/pool.h"
#include "render/render.h"
#include "render/term_frame.h"
#include "term_api.h"

/*
 * Resize churn allocates nothing. A renderer paints a real terminal at six
 * canvas sizes: a warm-up, then a churn over the same sizes. No call to
 * render_draw() in the churn may allocate.
 *
 * Timing decides which thread paints which row. So a per-thread buffer
 * that grows when its thread first paints a long row makes the churn
 * allocate at random. The case with the most workers makes this likely:
 * most workers paint a text row for the first time at a random frame.
 * The text has long words and glyphs that spill into the next cell.
 */

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif

/* Pool workers allocate too, so the counter is atomic. */
static _Atomic unsigned long allocs __attribute__((unused));
static _Atomic int counting;

static inline void count_alloc(void)
{
    if (atomic_load_explicit(&counting, memory_order_relaxed))
        atomic_fetch_add_explicit(&allocs, 1, memory_order_relaxed);
}

#ifndef SANITIZED

void *malloc(size_t n)
{
    static void *(*real)(size_t);
    if (!real) real = dlsym(RTLD_NEXT, "malloc");
    count_alloc();
    return real(n);
}

void *calloc(size_t n, size_t sz)
{
    static void *(*real)(size_t, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "calloc");
    count_alloc();
    return real(n, sz);
}

void *realloc(void *p, size_t n)
{
    static void *(*real)(void *, size_t);
    if (!real) real = dlsym(RTLD_NEXT, "realloc");
    count_alloc();
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

/*
 * One window frame. The canvas alternates between two buffers and each
 * frame is fresh, as in the window. Every third frame has no history, as a
 * thumbnail has.
 */
static void paint(struct frame_stats *st, struct renderer *r,
                  struct framebuffer_history *history,
                  const struct terminal_frame *grid, uint8_t *pool, int i,
                  int cw, int ch)
{
    size_t off = (i & 1) ? (size_t)cw * 4 * (size_t)ch : 0;
    frame_stats_tick(st);
    render_begin(st);
    struct framebuffer target = {
        .pixels = (uint32_t *)(pool + off),
        .size = { .x = cw, .y = ch },
        .fresh = true,
        .history = i % 3 ? history : NULL,
    };
    struct render_frame frame = {
        .base = 0xFF202020u,
        .grid = { .frame = grid, .rect = { .pos = { .y = 24 }, .size = target.size } },
    };
    (void)render_draw(r, &target, &frame);
    render_end_and_count_frame(st);
}

/* Paints the warm-up and the churn. Returns the allocations of the churn. */
static unsigned long churn(const struct render_options *opt, const struct terminal_frame *grid,
                           uint8_t *pool, unsigned *resizes_out)
{
    struct renderer *r = renderer_create(opt);
    assert(r);
    struct framebuffer_history history = {0};
    struct frame_stats st = { .started_ns = yasl_time_ns(), .frame_min_ms = 1e18 };

    for (int i = 0; i < nsizes * 6; i++)
        paint(&st, r, &history, grid, pool, i, sizes[i % nsizes][0], sizes[i % nsizes][1]);

    atomic_store(&allocs, 0);
    atomic_store(&counting, 1);
    unsigned resizes = 0;
    int cw = sizes[0][0], ch = sizes[0][1];
    for (int i = 0; i < 240; i++) {
        if (i % 20 == 0) {
            const int *s = sizes[(i / 20) % nsizes];
            cw = s[0] > MAX_W ? MAX_W : s[0];
            ch = s[1] > MAX_H ? MAX_H : s[1];
            resizes++;
        }
        paint(&st, r, &history, grid, pool, i, cw, ch);
    }
    atomic_store(&counting, 0);

    framebuffer_history_release(&history);
    renderer_destroy(r);
    *resizes_out = resizes;
    return atomic_load(&allocs);
}

int main(void)
{
    render_init();
    struct wsap_vtcore *term = tt_new(100, 30);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(term && snap);
    tt_puts(term, "resize churn: the quick brown fox jumps over the lazy dog\r\n"
                  "\033[1mbold\033[0m \033[3mitalic\033[0m \033[4munderline\033[0m "
                  "\033[31mred\033[0m \033[48;2;10;20;30mtruecolor\033[0m -> != ==\r\n"
                  "\033[3mWWff\033[0m W \033[1mWW\033[0m \xe2\x97\x8f\xe2\x97\x8f "
                  "\xef\x84\x95\xef\x84\x95");
    assert(wsap_vtcore_snapshot_update(snap, term));
    struct terminal_frame grid;
    terminal_frame_init(&grid, snap, true);

    size_t cap = (size_t)MAX_W * 4 * (size_t)MAX_H * 2;
    uint8_t *pool = malloc(cap);
    assert(pool);
    memset(pool, 0x10, cap);

    static const struct {
        const char *name;
        struct render_options opt;
    } cases[] = {
        { "default threads", {0} },
        { "most workers", { .threads = POOL_WORKERS_MAX } },
    };
    int failed = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        unsigned resizes = 0;
        unsigned long n = churn(&cases[c].opt, &grid, pool, &resizes);
#ifdef SANITIZED
        (void)n;
        printf("resize alloc test, %s: churn ran over %u resizes, allocation count "
               "not checked under sanitizer\n", cases[c].name, resizes);
#else
        if (n != 0) {
            fprintf(stderr, "resize churn, %s: allocated %lu times over %u resizes\n",
                    cases[c].name, n, resizes);
            failed = 1;
        } else {
            printf("resize alloc test, %s: passed (%u resizes, no allocation)\n",
                   cases[c].name, resizes);
        }
#endif
    }

    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(term);
    free(pool);
    render_shutdown();
    return failed;
}
