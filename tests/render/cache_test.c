#define _GNU_SOURCE
/*
 * Cache independence. The pixels of render_draw() must not depend on the
 * cell cache budget, the thread count, or a cold or a warm cache. Each case
 * draws the same frames with several renderers and compares every frame
 * with a renderer that has no cache and no workers:
 * - a mixed workload, cold and then warm, with a 64 KB budget, 0 and 3
 *   threads, and the default budget;
 * - a hot set of code text between frames of one-off colors (lolcat);
 * - two cell sizes in turn on one renderer, which share one budget;
 * - idle frames, then a repaint from the warm cache.
 * Steady state allocates nothing: a malloc counter runs around
 * render_draw() for frames whose content the renderer has seen before.
 */
#include <dlfcn.h>
#include <stdatomic.h>

#include "helpers/flood.h"
#include "helpers/render_draw.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define SANITIZED 1
#endif

static _Atomic long allocs;
static _Atomic bool counting;

#ifndef SANITIZED
static void count(void)
{
    if (atomic_load_explicit(&counting, memory_order_relaxed))
        atomic_fetch_add_explicit(&allocs, 1, memory_order_relaxed);
}

void *malloc(size_t n)
{
    static void *(*real)(size_t);
    if (!real)
        real = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    count();
    return real(n);
}

void *calloc(size_t n, size_t size)
{
    static void *(*real)(size_t, size_t);
    if (!real)
        real = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    count();
    return real(n, size);
}

void *realloc(void *p, size_t n)
{
    static void *(*real)(void *, size_t);
    if (!real)
        real = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    count();
    return real(p, n);
}

void *aligned_alloc(size_t align, size_t n)
{
    static void *(*real)(size_t, size_t);
    if (!real)
        real = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
    count();
    return real(align, n);
}
#endif

static struct font_instance *fonts[2];
static struct renderer *oracle;

/* ---- Many renderers on one sequence of frames ------------------------ */

#define MAX_SUBJECTS 5

struct subject {
    const char *name;
    struct render_options opt;
    struct renderer *r;
    struct tcanvas canvas[2];
};

struct bench {
    struct subject s[MAX_SUBJECTS];
    int n;
    uint32_t *want;
    size_t cap;
    long frames;
};

static void bench_open(struct bench *b)
{
    static const struct subject subjects[] = {
        { .name = "64K/0", .opt = { .threads = RENDER_NONE, .cache_bytes = 64 << 10 } },
        { .name = "64K/3", .opt = { .threads = 3, .cache_bytes = 64 << 10 } },
        { .name = "16M/0", .opt = { .threads = RENDER_NONE } },
        { .name = "16M/3", .opt = { .threads = 3 } },
        { .name = "none/3", .opt = { .threads = 3, .cache_bytes = RENDER_NONE } },
    };
    *b = (struct bench){ .n = MAX_SUBJECTS };
    for (int i = 0; i < b->n; i++) {
        b->s[i] = subjects[i];
        b->s[i].r = renderer_create(&b->s[i].opt);
        TCHECK(b->s[i].r, "renderer %s", b->s[i].name);
    }
}

static void bench_close(struct bench *b)
{
    for (int i = 0; i < b->n; i++) {
        renderer_destroy(b->s[i].r);
        for (int k = 0; k < 2; k++)
            if (b->s[i].canvas[k].px)
                tcanvas_free(&b->s[i].canvas[k]);
    }
    free(b->want);
}

/* Draws one frame on canvas `slot` of every subject and compares. */
static void bench_frame(struct bench *b, int slot, const struct render_frame *fr,
                        struct vector2i size, const char *what)
{
    size_t n = (size_t)size.x * (size_t)size.y;
    if (n > b->cap) {
        free(b->want);
        b->want = malloc(n * sizeof(*b->want));
        TCHECK(b->want, "buffer");
        b->cap = n;
    }
    tdraw_fresh(oracle, b->want, size, fr);
    for (int i = 0; i < b->n; i++) {
        struct tcanvas *c = &b->s[i].canvas[slot];
        if (!c->px)
            tcanvas_init(c, size.x, size.y);
        else if (!vector2i_eq(c->size, size))
            tcanvas_resize(c, size.x, size.y);
        tdraw(b->s[i].r, c, fr);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: %s frame %ld", b->s[i].name, what, b->frames);
        tcheck_same(msg, c->px, b->want, size);
    }
    b->frames++;
}

static struct vector2i term_size(struct font_instance *fi, int cols, int rows)
{
    const struct font_metrics *m = font_instance_metrics(fi);
    return (struct vector2i){ .x = cols * m->cell_w + 5, .y = rows * m->cell_h + 3 };
}

static void draw_term(struct bench *b, int slot, struct tterm *tt, struct font_instance *fi,
                      bool repaint, const char *what)
{
    const struct terminal_frame *f = tterm_frame(tt, true);
    struct vector2i size = term_size(fi, tt_cols(tt->t), tt_rows(tt->t));
    struct render_frame fr = tframe(f, fi, size);
    fr.repaint = repaint;
    bench_frame(b, slot, &fr, size, what);
}

static void mixed_workload(void)
{
    struct bench b;
    bench_open(&b);
    struct tterm tt;
    tterm_open(&tt, 100, 30);
    uint32_t seed = 0x2545F491u;
    static char chunk[8192];
    for (int frame = 0; frame < 60; frame++) {
        enum flood_kind kinds[] = { FLOOD_CODE, FLOOD_COLOR, FLOOD_TRUECOLOR, FLOOD_LOG,
                                    FLOOD_PRINTABLE };
        enum flood_kind kind = kinds[frame % 5];
        size_t len = flood_chunk(kind, &seed, chunk, frame % 3 ? 1500 : sizeof(chunk));
        tt_write(tt.t, chunk, len);
        if (frame % 7 == 3)
            tterm_feed(&tt, "\033[3m日本語 🙂 WAVATAYA\033[0m \033[4:3mcurly\033[0m\033[2m faint\033[0m");
        if (frame % 11 == 5)
            tt_select(tt.t, 2, 4, 40, 9);
        draw_term(&b, 0, &tt, fonts[0], false, "mixed");
        /* The same frame from a warm cache. */
        if (frame % 10 == 9)
            draw_term(&b, 0, &tt, fonts[0], true, "warm repaint");
    }
    tterm_close(&tt);
    bench_close(&b);
}

/* A hot set of code text survives frames of one-off colors. */
static void lolcat(void)
{
    struct bench b;
    bench_open(&b);
    struct tterm hot, flood;
    tterm_open(&hot, 120, 32);
    tterm_open(&flood, 120, 32);
    uint32_t seed = 7;
    static char chunk[8192];
    tt_write(hot.t, chunk, flood_chunk(FLOOD_CODE, &seed, chunk, sizeof(chunk)));
    for (int frame = 0; frame < 40; frame++) {
        tt_write(flood.t, chunk, flood_chunk(FLOOD_TRUECOLOR, &seed, chunk, sizeof(chunk)));
        draw_term(&b, 0, &flood, fonts[0], false, "lolcat");
        draw_term(&b, 1, &hot, fonts[0], frame % 4 == 0, "hot set");
    }
    tterm_close(&hot);
    tterm_close(&flood);
    bench_close(&b);
}

/* Two cell sizes in turn: one cache, two classes, one budget. */
static void two_sizes(void)
{
    struct bench b;
    bench_open(&b);
    struct tterm tt[2];
    uint32_t seed = 99;
    static char chunk[4096];
    for (int i = 0; i < 2; i++)
        tterm_open(&tt[i], 70, 18);
    for (int frame = 0; frame < 40; frame++) {
        for (int i = 0; i < 2; i++) {
            size_t len = flood_chunk(frame % 2 ? FLOOD_COLOR : FLOOD_CODE, &seed, chunk,
                                     sizeof(chunk) / 2);
            tt_write(tt[i].t, chunk, len);
            draw_term(&b, i, &tt[i], fonts[i], false, i ? "big font" : "small font");
        }
    }
    for (int i = 0; i < 2; i++)
        tterm_close(&tt[i]);
    bench_close(&b);
}

/* Idle frames change nothing. A repaint after them gives the same pixels. */
static void idle(void)
{
    struct bench b;
    bench_open(&b);
    struct tterm tt;
    tterm_open(&tt, 90, 24);
    uint32_t seed = 3;
    static char chunk[8192];
    tt_write(tt.t, chunk, flood_chunk(FLOOD_COLOR, &seed, chunk, sizeof(chunk)));
    draw_term(&b, 0, &tt, fonts[0], false, "warm up");
    draw_term(&b, 0, &tt, fonts[0], true, "warm up");
    for (int i = 0; i < 60; i++) {
        const struct terminal_frame *f = tterm_frame(&tt, true);
        struct vector2i size = term_size(fonts[0], 90, 24);
        struct render_frame fr = tframe(f, fonts[0], size);
        for (int s = 0; s < b.n; s++) {
            struct render_result res = tdraw(b.s[s].r, &b.s[s].canvas[0], &fr);
            TCHECK(!res.damage.any, "%s: an idle frame damages", b.s[s].name);
        }
    }
    draw_term(&b, 0, &tt, fonts[0], true, "repaint after idle");
    tterm_close(&tt);
    bench_close(&b);
}

/* ---- Steady state allocates nothing ----------------------------------- */

struct steady {
    struct renderer *r;
    struct tcanvas canvas;
    long frames;
};

static struct render_result steady_draw(struct steady *s, const struct render_frame *fr,
                                        bool history)
{
    atomic_store(&counting, true);
    struct render_result res;
    if (history) {
        res = tdraw(s->r, &s->canvas, fr);
    } else {
        struct framebuffer fb = { .pixels = s->canvas.px, .size = s->canvas.size };
        res = render_draw(s->r, &fb, fr);
    }
    atomic_store(&counting, false);
    s->frames++;
    return res;
}

/*
 * Runs `frames` steps of a cycle of states. The first cycles warm every
 * cache. Then the counter must stay at zero.
 */
static void steady_case(const char *name, int threads, int cols, int rows,
                        void (*step)(struct tterm *tt, struct ui_scene *scene, int i,
                                     bool *fresh, bool *history))
{
    struct steady s = { .r = renderer_create(&(struct render_options){ .threads = threads }) };
    TCHECK(s.r, "renderer");
    struct tterm tt;
    tterm_open(&tt, cols, rows);
    struct vector2i size = term_size(fonts[0], cols, rows);
    tcanvas_init(&s.canvas, size.x, size.y);
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    long counted = 0;
    for (int i = 0; i < 400; i++) {
        bool fresh = false, history = true;
        step(&tt, &scene, i, &fresh, &history);
        const struct terminal_frame *f = tterm_frame(&tt, true);
        struct render_frame fr = tframe(f, fonts[0], size);
        fr.ui = &scene;
        fr.ui_damage = NULL;
        s.canvas.fresh |= fresh;
        if (i == 200)
            atomic_store(&allocs, 0);
        steady_draw(&s, &fr, history);
        if (i >= 200)
            counted++;
    }
    long n = atomic_load(&allocs);
    printf("steady %-10s %ld frames, %ld allocations\n", name, counted, n);
    TCHECK(n == 0, "%s: %ld allocations in %ld steady frames", name, n, counted);
    ui_scene_destroy(&scene);
    tcanvas_free(&s.canvas);
    tterm_close(&tt);
    renderer_destroy(s.r);
}

static void step_typing(struct tterm *tt, struct ui_scene *scene, int i, bool *fresh,
                        bool *history)
{
    (void)scene;
    (void)fresh;
    (void)history;
    if (i == 0)
        tterm_feed(tt, "fn main() -> i32 { let x = a != b; }\r\n\033[3mitalic WAVA\033[0m");
    tterm_feed(tt, i % 2 ? "\033[2;3Hb" : "\033[2;3Ha");
}

static void step_full(struct tterm *tt, struct ui_scene *scene, int i, bool *fresh,
                      bool *history)
{
    (void)scene;
    (void)history;
    uint32_t seed = (uint32_t)(i % 3) + 1;
    static char chunk[8192];
    tterm_feed(tt, "\033[H\033[2J");
    tt_write(tt->t, chunk, flood_chunk(FLOOD_CODE, &seed, chunk, sizeof(chunk)));
    *fresh = true;
}

static void step_scroll(struct tterm *tt, struct ui_scene *scene, int i, bool *fresh,
                        bool *history)
{
    (void)fresh;
    (void)history;
    static const char *const lines[] = { "alpha -> beta\r\n", "gamma != delta\r\n",
                                         "\033[3mepsilon WAVA\033[0m\r\n" };
    tterm_feed(tt, lines[i % 3]);
    /* A scrollbar that moves on each scroll. */
    ui_scene_destroy(scene);
    ui_scene_init(scene);
    TCHECK(ui_scene_rounded_rect(scene, trect(300, 10 + (i % 5) * 7, 5, 40), 2, 0x80A6ADC8u),
           "scrollbar");
}

static void step_overlay(struct tterm *tt, struct ui_scene *scene, int i, bool *fresh,
                         bool *history)
{
    (void)fresh;
    (void)history;
    if (i == 0)
        tterm_feed(tt, "\033[3mWWWW fjord\033[0m \xef\x84\x95 text under the overlay");
    ui_scene_destroy(scene);
    ui_scene_init(scene);
    TCHECK(ui_scene_blend_rect(scene, trect(13 + (i % 4) * 9, 2, 33, 30), 0xFF2050A0u, 100),
           "overlay");
}

static void step_no_history(struct tterm *tt, struct ui_scene *scene, int i, bool *fresh,
                            bool *history)
{
    (void)scene;
    (void)fresh;
    if (i == 0)
        tterm_feed(tt, "thumbnail \033[38;5;33mcolors\033[0m 日本");
    *history = false;
}

int main(void)
{
    TCHECK(render_init(), "render_init");
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    fonts[0] = font_instance_default();
    fonts[1] = font_instance_acquire(font_instance_size_pt(fonts[0]), 180);
    TCHECK(fonts[0] && fonts[1], "fonts");
    oracle = renderer_create(&(struct render_options){
        .threads = RENDER_NONE, .cache_bytes = RENDER_NONE });
    TCHECK(oracle, "oracle");

    mixed_workload();
    lolcat();
    two_sizes();
    idle();
#ifndef SANITIZED
    steady_case("typing", 0, 60, 8, step_typing);
    steady_case("full", 3, 160, 50, step_full);
    steady_case("scroll", 3, 60, 12, step_scroll);
    steady_case("overlay", 0, 60, 6, step_overlay);
    steady_case("no-history", 3, 40, 4, step_no_history);
#else
    puts("steady state allocation checks skipped under a sanitizer");
#endif

    renderer_destroy(oracle);
    font_instance_unref(fonts[1]);
    render_shutdown();
    puts("PASS: pixels do not depend on the cell cache or the threads");
    return 0;
}
