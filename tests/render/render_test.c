#define _POSIX_C_SOURCE 200809L
#include "render/render.h"
#include "../helpers/render_frame_helpers.h"
#include "render/util.h"
#include "paint/damage.h"
#include "paint/scene.h"
#include "ui/damage.h"
#include "font/font.h"
#include "font/glyph_cache.h"
#include "render/backend.h"
#include "paint/framebuffer.h"
#include "render/term_frame.h"
#include "render/scene_paint.h"
#include "term_api.h"
#include "helpers/flood.h"

#undef CELL_W
#undef CELL_H
#define CELL_W (font_instance_metrics(test_font())->cell_w)
#define CELL_H (font_instance_metrics(test_font())->cell_h)
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct framebuffer_history render_history;

static struct renderer *test_renderer(void)
{
    framebuffer_history_release(&render_history);
    return renderer_create();
}

static uint32_t *window_pixels;
static int window_width, window_height;
static bool vector_enabled, vector_accept, vector_submitted;
static bool native_gpu_failure_frame;
extern struct framebuffer_gpu *render_test_gpu_instance(void);
extern void render_test_gpu_redraw_failure(bool fail);
struct framebuffer_gpu { int unused; };
static struct framebuffer_gpu fake_gpu;

enum render_backend_result render_gpu_terminal(struct framebuffer_gpu *g,
                                               const struct terminal_frame *frame,
                                               const struct render_backend_view *view)
{
    (void)frame; (void)view;
    if (g == render_test_gpu_instance()) {
        if (native_gpu_failure_frame)
            return RENDER_BACKEND_DREW;
        return RENDER_BACKEND_SKIP;
    }
    assert(g == &fake_gpu);
    if (!vector_enabled)
        return RENDER_BACKEND_SKIP;
    vector_submitted = vector_accept;
    return vector_accept ? RENDER_BACKEND_DREW : RENDER_BACKEND_FAILED;
}

static struct damage_list test_damage;

static struct framebuffer test_target(void)
{
    test_damage = (struct damage_list){0};
    return (struct framebuffer){
        .pixels = window_pixels,
        .size = { .x = window_width, .y = window_height },
        .damage = &test_damage,
        .gpu = &fake_gpu,
        .history = &render_history,
    };
}

static void draw_terminal_with_history(struct renderer *r,
        struct framebuffer_history *history, uint32_t *pixels,
        int width, int height, int buffer, const struct terminal_frame *grid)
{
    struct damage_list damage = {0};
    struct framebuffer target = {
        .pixels = pixels,
        .size = { .x = width, .y = height },
        .buffer = buffer,
        .damage = &damage,
        .history = history,
    };
    struct render_frame frame = {
        .font = test_font(),
        .has_grid = true,
        .grid = *grid,
    };
    (void)render_draw(r, &target, &frame);
}

static void feed(struct wsap_vtcore *t, const char *s) { wsap_vtcore_feed(t, s, strlen(s), 0); }
static void draw_terminal_test(struct renderer *r, struct framebuffer *rt,
                               struct wsap_vtcore *t, bool cursor_visible)
{
    struct terminal_frame frame;
    test_frame_init(&frame, t, cursor_visible);
    struct render_frame input = {
        .font = test_font(),
        .ui_font = test_font(),
        .grid = frame,
        .has_grid = true,
    };
    (void)render_draw(r, rt, &input);
}

static struct damage_list draw_scene(struct renderer *r, struct framebuffer *target,
                                     const struct ui_scene *scene,
                                     const struct ui_damage *damage,
                                     struct rect2i terminal, uint32_t base)
{
    struct render_frame frame = {
        .ui_font = test_font(),
        .terminal = terminal,
        .base = base,
        .paint = scene,
        .damage = damage,
    };
    return render_draw(r, target, &frame);
}

static struct wsap_vtcore_theme default_theme(void)
{
    struct wsap_vtcore_theme theme;
    wsap_vtcore_theme_default(&theme);
    return theme;
}

#define DEFAULT_FG (default_theme().fg)
#define DEFAULT_BG (default_theme().bg)
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
#if defined(__SSE4_2__)
#include <immintrin.h>
static uint64_t hash(const uint32_t *pixels, size_t count)
{
    const uint64_t *p64 = (const uint64_t *)pixels;
    size_t n64 = (count * sizeof(uint32_t)) / sizeof(uint64_t);
    uint64_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c6 = 0, c7 = 0;
    size_t i = 0;
    for (; i + 8 <= n64; i += 8) {
        c0 = _mm_crc32_u64(c0, p64[i]);
        c1 = _mm_crc32_u64(c1, p64[i+1]);
        c2 = _mm_crc32_u64(c2, p64[i+2]);
        c3 = _mm_crc32_u64(c3, p64[i+3]);
        c4 = _mm_crc32_u64(c4, p64[i+4]);
        c5 = _mm_crc32_u64(c5, p64[i+5]);
        c6 = _mm_crc32_u64(c6, p64[i+6]);
        c7 = _mm_crc32_u64(c7, p64[i+7]);
    }
    for (; i < n64; i++) {
        c0 = _mm_crc32_u64(c0, p64[i]);
    }
    return c0 ^ (c1 * 31) ^ (c2 * 1009) ^ (c3 * 1000003) ^
           (c4 * 1000000007ULL) ^ (c5 * 6364136223846793005ULL) ^
           (c6 * 1442695040888963407ULL) ^ (c7 * 11400714819323198485ULL);
}
#else
static uint64_t hash(const uint32_t *pixels, size_t count)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; i++) h = (h ^ pixels[i]) * UINT64_C(1099511628211);
    return h;
}
#endif

static void incremental_test(void)
{
    struct wsap_vtcore *t = tt_new(40, 5);
    int w = 40 * CELL_W + 3, h = 5 * CELL_H + 2;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *pixels[2] = {malloc(bytes), malloc(bytes)}, *expected = malloc(bytes);
    assert(t && pixels[0] && pixels[1] && expected);
    memset(pixels[0], 0x11, bytes); memset(pixels[1], 0x22, bytes);
    struct renderer *r = test_renderer();
    assert(r);
    const char *steps[] = {
        "\033[?25labcdefghijklmnopqrstuvwxyz\r\n-> === != سلام",
        "\033[31mred\033[0m\r\n\033[3mitalic ->\033[0m",
        "\r\n\033[2mfaint\033[0m bright\r\n日本語 🙂",
        "\r\nscroll one\r\nscroll two\r\nscroll three",
        "\033[?25h\033[2;2H", "\033[?5h", "\033[?5l\033[2J\033[H",
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        feed(t, steps[i]);
        if (i == 3) tt_select(t, 0, 1, 10, 2);
        for (int b = 0; b < 2; b++) {
            memset(expected, 0x33, bytes);
            test_render_paint_full(expected, w, h, true, t);
            test_renderer_paint(r, pixels[b], w, h, b, t, true);
            assert(!memcmp(expected, pixels[b], bytes));
        }
    }
    renderer_destroy(r);
    free(pixels[0]); free(pixels[1]); free(expected); wsap_vtcore_free(t);
}

static void renderer_lifecycle_test(void)
{
    struct renderer *r = test_renderer();
    assert(r);
    struct wsap_vtcore *t = tt_new(1, 1);
    assert(t);
    feed(t, "\033[?25l");
    uint32_t pixels[CELL_W * CELL_H];
    test_renderer_paint(r, pixels, CELL_W, CELL_H, 0, t, true);
    for (size_t i = 0; i < sizeof(pixels) / sizeof(*pixels); i++)
        assert(pixels[i] == DEFAULT_BG);
    renderer_destroy(r);
    wsap_vtcore_free(t);
}

static void worker_pool_lifecycle_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 10);
    assert(t);
    feed(t, "\033[?25lParallel rendering lifecycle test");
    int w = 8 * CELL_W, h = 10 * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *p1 = malloc(bytes), *p2 = malloc(bytes);
    assert(p1 && p2);

    test_render_paint_full(p1, w, h, true, t);

    render_pool_destroy();

    test_render_paint_full(p2, w, h, true, t);
    assert(!memcmp(p1, p2, bytes));

    render_pool_destroy();

    free(p1);
    free(p2);
    wsap_vtcore_free(t);
}

#ifdef __linux__
static int live_thread_count(void)
{
    DIR *d = opendir("/proc/self/task");
    assert(d);
    int n = 0;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

static void scale_respawn_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 10);
    assert(t);
    feed(t, "\033[?25lscale churn");
    int w = 8 * CELL_W, h = 10 * CELL_H;
    uint32_t *px = malloc((size_t)w * h * sizeof(uint32_t));
    assert(px);

    struct renderer *r = test_renderer();
    assert(r);
    test_render_paint_full(px, w, h, true, t);

    int before = live_thread_count();
    struct font_instance *big = font_instance_acquire(
        font_instance_size_pt(font_instance_default()), 180);
    assert(big && big != font_instance_default());
    struct terminal_frame frame;
    test_frame_init(&frame, t, true);
    for (int i = 0; i < 8; i++) {
        render_paint_full_terminal_frame(big, px, w, h, &frame);
        test_render_paint_full(px, w, h, true, t);
    }
    assert(live_thread_count() == before);
    font_instance_unref(big);

    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
}
#endif

static void clipped_target_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    assert(t);
    feed(t, "\033[?25l\033[41mABCDEFGH\r\n\033[4m12345678");
    const int widths[] = {1, CELL_W - 1, CELL_W, 3 * CELL_W + 1};
    for (unsigned i = 0; i < sizeof(widths) / sizeof(*widths); i++) {
        int w = widths[i], h = 2 * CELL_H + 1;
        size_t bytes = (size_t)w * h * sizeof(uint32_t);
        uint32_t *full = malloc(bytes), *incremental = malloc(bytes);
        assert(full && incremental);
        struct renderer *r = test_renderer();
        assert(r);
        test_render_paint_full(full, w, h, true, t);
        test_renderer_paint(r, incremental, w, h, 0, t, true);
        for (size_t p = 0; p < bytes / 4; p++) {
            if (full[p] != incremental[p]) {
                fprintf(stderr, "w=%d first diff at px %zu (x=%zu y=%zu): full=%08x incr=%08x\n",
                        w, p, p % (size_t)w, p / (size_t)w, full[p], incremental[p]);
                for (size_t q = p > 8 ? p - 8 : 0; q < p + 8 && q < bytes/4; q++)
                    fprintf(stderr, "  px %4zu (x=%3zu y=%2zu): full=%08x incr=%08x\n",
                            q, q % (size_t)w, q / (size_t)w, full[q], incremental[q]);
                break;
            }
        }
        assert(!memcmp(full, incremental, bytes));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (x >= w / CELL_W * CELL_W || y >= 2 * CELL_H)
                    assert(full[(size_t)y * w + x] == DEFAULT_BG);
        renderer_destroy(r);
        free(full);
        free(incremental);
    }
    wsap_vtcore_free(t);
}

static void style_cache_test(void)
{
    struct wsap_vtcore *plain = tt_new(2, 1), *bold = tt_new(2, 1);
    assert(plain && bold);

    feed(plain, "\033[?25l\xe1\x80\x80");
    feed(bold, "\033[?25l\033[1m\xe1\x80\x80");
    int w = 2 * CELL_W, h = CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *expected = malloc(bytes), *actual = malloc(bytes);
    assert(expected && actual);
    glyph_cache_rebuild(test_font());
    test_render_paint_full(expected, w, h, true, plain);
    test_render_paint_full(actual, w, h, true, bold);
    test_render_paint_full(actual, w, h, true, plain);
    assert(!memcmp(expected, actual, bytes));
    free(expected);
    free(actual);
    wsap_vtcore_free(plain);
    wsap_vtcore_free(bold);
}

static void presentation_test(void)
{
    static const char *const text[] = {
        "\xe2\x98\xba\xef\xb8\x8f",
        "\xe2\x98\xba\xef\xb8\x8e",
    };
    struct wsap_vtcore *t = tt_new(2, 1);
    assert(t);
    wsap_vtcore_set_theme(t, &(struct wsap_vtcore_theme){ .fg = 0xFFFFFFFF, .bg = 0xFF000000 });
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    int w = 2 * CELL_W, h = CELL_H;
    uint32_t *pixels = malloc((size_t)w * h * sizeof(*pixels));
    assert(pixels);
    for (int i = 0; i < 2; i++) {
        feed(t, "\033[?25l\033[2J\033[H");
        feed(t, text[i]);
        test_render_paint_full(pixels, w, h, false, t);
        bool colored = false;
        bool ink = false;
        for (int p = 0; p < w * h; p++) {
            unsigned r = (pixels[p] >> 16) & 255;
            unsigned g = (pixels[p] >> 8) & 255;
            unsigned b = pixels[p] & 255;
            colored |= r != g || g != b;
            ink |= r || g || b;
        }
        assert(ink);
        assert(colored == (i == 0));
    }
    font_set_aa_mode(FONT_AA_AUTO);
    free(pixels);
    wsap_vtcore_free(t);
}

static void suffix_ligature_test(void)
{
#ifndef __APPLE__
    struct wsap_vtcore *a = tt_new(4, 1), *b = tt_new(4, 1);
    int w = 4 * CELL_W, h = CELL_H;
    uint32_t *pa = calloc((size_t)w * h, 4), *pb = calloc((size_t)w * h, 4);
    assert(a && b && pa && pb);
    feed(a, "\033[?25l\xcd\xb8->");
    feed(b, "\033[?25l ->");
    test_render_paint_full(pa, w, h, true, a); test_render_paint_full(pb, w, h, true, b);
    for (int y = 0; y < h; y++)
        assert(!memcmp(pa + y * w + CELL_W, pb + y * w + CELL_W, 2 * CELL_W * 4));
    free(pa); free(pb); wsap_vtcore_free(a); wsap_vtcore_free(b);
#endif
}

static void run_drift_test(void)
{
    const char *s = "../Terminal-Test";
    struct wsap_vtcore *t = tt_new(32, 1);
    int w = 32 * CELL_W, h = CELL_H;
    uint32_t *px = calloc((size_t)w * h, 4);
    assert(t && px);
    feed(t, "\033[?25l");
    feed(t, s);
    test_render_paint_full(px, w, h, true, t);
    for (int c = 0; c < 16; c++) {
        int ink = 0;
        for (int y = 0; y < h; y++)
            for (int x = c * CELL_W; x < (c + 1) * CELL_W; x++)
                if (px[(size_t)y * w + x] != DEFAULT_BG) ink++;
        assert(ink > 0);
    }
    struct wsap_vtcore *t2 = tt_new(32, 1);
    uint32_t *px2 = calloc((size_t)w * h, 4);
    assert(t2 && px2);
    feed(t2, "\033[?25lt");
    test_render_paint_full(px2, w, h, true, t2);
    for (int y = 0; y < h; y++)
        assert(!memcmp(px + (size_t)y * w + 15 * CELL_W, px2 + (size_t)y * w,
                       CELL_W * sizeof(uint32_t)));
    free(px2); wsap_vtcore_free(t2);
    free(px); wsap_vtcore_free(t);
}

static void cache_churn_test(void)
{
    struct wsap_vtcore *t = tt_new(40, 2), *noise = tt_new(40, 2);
    int w = 40 * CELL_W, h = 2 * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *expected = malloc(bytes), *actual = malloc(bytes);
    assert(t && noise && expected && actual);
    feed(t, "\033[?25l->===!= سلام 日本語\r\n\033[3mitalic -> !=\033[0m");
    test_render_paint_full(expected, w, h, true, t);
    uint32_t seed = 1;
    for (int i = 0; i < 256; i++) {
        char text[40];
        for (size_t j = 0; j < sizeof(text); j++) {
            seed = seed * 1664525u + 1013904223u;
            text[j] = (char)('!' + (seed >> 16) % 94);
        }
        feed(noise, "\033[H");
        tt_write(noise, text, sizeof(text));
    test_render_paint_full(actual, w, h, true, noise);
    }
    test_render_paint_full(actual, w, h, true, t);
    assert(!memcmp(expected, actual, bytes));
    glyph_cache_rebuild(test_font());
    test_render_paint_full(actual, w, h, true, t);
    assert(!memcmp(expected, actual, bytes));
    free(expected); free(actual); wsap_vtcore_free(t); wsap_vtcore_free(noise);
}

static void oversized_frame_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    assert(t);
    feed(t, "\033[?25lABCD");
    int w = 8 * CELL_W, h = 300 * CELL_H;
    uint32_t *px = malloc((size_t)w * h * sizeof(*px));
    assert(px);
    struct renderer *r = test_renderer();
    assert(r);

    struct terminal_frame frame;
    test_frame_init(&frame, t, true);
    frame.rows = 300;

    for (int b = 0; b < FRAMEBUFFER_BUFFERS; b++)
        draw_terminal_with_history(r, &render_history, px, w, h, b, &frame);
    uint32_t *expected = malloc((size_t)w * h * sizeof(*expected));
    assert(expected);
    render_paint_full_terminal_frame(test_font(), expected, w, h, &frame);
    assert(!memcmp(px, expected, (size_t)w * h * sizeof(*px)));
    free(expected);
    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
}

static double srgb_to_lin(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double lin_to_srgb(double l)
{
    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}
static double lum_lin(uint32_t rgb)
{
    return 0.2126 * srgb_to_lin(((rgb >> 16) & 255) / 255.0) +
           0.7152 * srgb_to_lin(((rgb >> 8) & 255) / 255.0) +
           0.0722 * srgb_to_lin((rgb & 255) / 255.0);
}

static double corrected_cov(uint32_t fg, uint32_t bg, double a)
{
    double fg_l = lum_lin(fg), bg_l = lum_lin(bg);
    if (fabs(fg_l - bg_l) <= 0.001) return a;
    double naive = lin_to_srgb(fg_l) * a + lin_to_srgb(bg_l) * (1 - a);
    double c = (srgb_to_lin(naive) - bg_l) / (fg_l - bg_l);
    return fmin(1.0, fmax(0.0, c));
}

static void linear_blend_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 1);
    int w = 8 * CELL_W, h = CELL_H;
    uint32_t *px = calloc((size_t)w * h, 4);
    assert(t && px);
    feed(t, "\033[?25l\033[2;4m \033[0m");
    test_render_paint_full(px, w, h, true, t);
    uint32_t uln = px[(size_t)(h - 2) * w];
    for (int x = 0; x < CELL_W; x++)
        assert(px[(size_t)(h - 2) * w + x] == uln);
    double a = corrected_cov(DEFAULT_FG, DEFAULT_BG, 128 / 255.0);
    a = lround(a * 255) / 255.0;
    for (int k = 0; k < 3; k++) {
        unsigned fgc = (DEFAULT_FG >> (8 * k)) & 0xFF;
        unsigned bgc = (DEFAULT_BG >> (8 * k)) & 0xFF;
        double lin = srgb_to_lin(fgc / 255.0) * a +
                     srgb_to_lin(bgc / 255.0) * (1 - a);
        unsigned want = (unsigned)(lin_to_srgb(lin) * 255.0 + 0.5);
        unsigned got = (uln >> (8 * k)) & 0xFF;
        assert(got + 3 >= want && got <= want + 3);
    }
    assert(px[(size_t)7 * CELL_W] == DEFAULT_BG);
    free(px); wsap_vtcore_free(t);

    t = tt_new(8, 1);
    px = calloc((size_t)w * h, 4);
    assert(t && px);
    feed(t, "\033[?25l\033[1mMW#@");
    test_render_paint_full(px, w, h, true, t);
    bool full = false;
    for (int i = 0; i < w * h; i++)
        if (px[i] == DEFAULT_FG) full = true;
    assert(full);
    assert(px[(size_t)7 * CELL_W] == DEFAULT_BG);
    free(px); wsap_vtcore_free(t);
}

static void color_glyph_blend_test(void)
{
    struct wsap_vtcore *t = tt_new(2, 1);
    assert(t);
    struct glyph glyph = {0};
    assert(font_instance_render_glyph(test_font(), 0x1f600, FONT_STYLE_WIDE, &glyph));
    assert(glyph.has_color);
    int w = 2 * CELL_W, h = CELL_H;
    uint32_t *pixels = malloc((size_t)w * h * sizeof(*pixels));
    assert(pixels);
    for (int faint = 0; faint < 2; faint++) {
        feed(t, faint ? "\033[H\033[2m" : "\033[?25l");
        feed(t, "\xf0\x9f\x98\x80");
        test_render_paint_full(pixels, w, h, true, t);
        bool translucent = false;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < glyph.width; x++) {
                int dx = glyph.x_offset + x;
                if (dx < 0 || dx >= w) continue;
                uint32_t p = *glyph_color_at(&glyph, y, x);
                unsigned alpha = p >> 24;
                if (!alpha || alpha == 255) continue;
                translucent = true;
                double coverage = (faint ? (alpha + 1) / 2 : alpha) / 255.0;
                for (int shift = 0; shift <= 16; shift += 8) {
                    double source = fmin(1, ((p >> shift) & 255) / (double)alpha);
                    double background = ((DEFAULT_BG >> shift) & 255) / 255.0;
                    double linear = srgb_to_lin(source) * coverage +
                                    srgb_to_lin(background) * (1 - coverage);
                    int want = (int)lround(lin_to_srgb(linear) * 255);
                    int got = (pixels[(size_t)y * w + dx] >> shift) & 255;
                    assert(abs(got - want) <= 2);
                }
            }
        }
        if (!translucent) {
            unsigned clear = 0, opaque = 0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < glyph.width; x++) {
                    unsigned alpha = *glyph_color_at(&glyph, y, x) >> 24;
                    clear += alpha == 0;
                    opaque += alpha == 255;
                }
            fprintf(stderr, "color glyph %ux%u at x %d in a %dx%d cell: "
                    "%u clear and %u opaque pixels\n", glyph.width, glyph.height,
                    glyph.x_offset, w, h, clear, opaque);
        }
        assert(translucent);
    }
    glyph_release(&glyph);
    free(pixels);
    wsap_vtcore_free(t);
}

static void frame_colors_test(int rows)
{
    static const char line[] =
        "A->!= \xc3\xa9\xd0\x96\xce\xb1 \xe6\x97\xa5\xe6\x9c\xac www";
    static const char colors_a[] = "\033]10;#ffffff\007\033]11;#1e1e2e\007";
    static const char colors_b[] = "\033]10;#203040\007\033]11;#e8d8c8\007";
    int cols = 24;
    struct wsap_vtcore *t = tt_new(cols, rows);
    assert(t);
    feed(t, "\033[?25l");
    feed(t, colors_a);
    for (int row = 0; row < rows; row++) {
        feed(t, line);
        if (row + 1 < rows)
            feed(t, "\r\n");
    }
    int w = cols * CELL_W, h = rows * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *at_a = malloc(bytes), *full = malloc(bytes), *incr = malloc(bytes);
    uint32_t *incr_b = malloc(bytes), *fresh = malloc(bytes);
    assert(at_a && full && incr && incr_b && fresh);
    struct renderer *r = test_renderer();
    assert(r);

    test_render_paint_full(at_a, w, h, true, t);
    test_renderer_paint(r, incr, w, h, 0, t, true);
    feed(t, colors_b);
    test_renderer_paint(r, incr, w, h, 0, t, true);
    memcpy(incr_b, incr, bytes);

    feed(t, colors_a);
    test_renderer_paint(r, incr, w, h, 0, t, true);
    test_render_paint_full(full, w, h, true, t);
    feed(t, colors_b);
    test_render_paint_full(full, w, h, true, t);

    glyph_cache_rebuild(test_font());
    test_render_paint_full(fresh, w, h, true, t);
    assert(memcmp(at_a, fresh, bytes) != 0);
    assert(!memcmp(full, fresh, bytes));
    assert(!memcmp(incr_b, fresh, bytes));

    renderer_destroy(r);
    free(at_a); free(full); free(incr); free(incr_b); free(fresh);
    wsap_vtcore_free(t);
}

static void theme_recolors_existing_text_test(void)
{
    uint32_t saved_bg = DEFAULT_BG;

    struct wsap_vtcore *t = tt_new(4, 1);
    assert(t);
    feed(t, "\033[?25l \033[44m \033[0m\033[31m \033[38;2;1;2;3m\033[48;2;4;5;6m ");
    int w = 4 * CELL_W, h = CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *pixels = malloc(bytes), *expected = malloc(bytes);
    assert(pixels && expected);
    struct renderer *r = test_renderer();
    assert(r);
    test_renderer_paint(r, pixels, w, h, 0, t, true);
    assert(pixels[0] == saved_bg);

    struct wsap_vtcore_theme light = default_theme();
    light.fg = 0xFF101010u;
    light.bg = 0xFFF0F0F0u;
    light.ansi[4] = 0xFF0000C0u;
    wsap_vtcore_set_theme(t, &light);
    test_renderer_paint(r, pixels, w, h, 0, t, true);
    test_render_paint_full(expected, w, h, true, t);
    assert(!memcmp(pixels, expected, bytes));
    assert(pixels[0] == 0xFFF0F0F0u);
    assert(pixels[CELL_W] == 0xFF0000C0u);
    assert(pixels[3 * CELL_W] == 0xFF040506u);

    renderer_destroy(r);
    free(pixels);
    free(expected);
    wsap_vtcore_free(t);
}

static void theme_redraw_test(void)
{
    struct wsap_vtcore *t = tt_new(2, 1);
    assert(t);
    int w = 2 * CELL_W, h = CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *pixels = malloc(bytes), *expected = malloc(bytes);
    assert(pixels && expected);
    struct renderer *r = test_renderer();
    assert(r);
    test_renderer_paint(r, pixels, w, h, 0, t, true);
    feed(t, "\033]12;#ff0000\007");
    test_renderer_paint(r, pixels, w, h, 0, t, true);
    test_render_paint_full(expected, w, h, true, t);
    assert(!memcmp(pixels, expected, bytes));

    feed(t, "\033[?25l\033]10;#00ff00\007\033]11;#0000ff\007\033[0m\033[2J\033[HA");
    test_renderer_paint(r, pixels, w, h, 0, t, true);
    test_render_paint_full(expected, w, h, true, t);
    assert(!memcmp(pixels, expected, bytes));
    renderer_destroy(r);
    free(pixels);
    free(expected);
    wsap_vtcore_free(t);
    glyph_cache_rebuild(test_font());
}

static void flood_benchmark(enum flood_kind kind, const char *name)
{
    int cols = 160, rows = 50, frames = getenv("FLOOD_FRAMES") ? atoi(getenv("FLOOD_FRAMES")) : 200;
    struct wsap_vtcore *t = tt_new(cols, rows);
    struct renderer *r = test_renderer();
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    int w = cols * CELL_W, h = rows * CELL_H;
    uint32_t *px = malloc((size_t)w * h * sizeof(*px));
    assert(t && r && snap && px);
    feed(t, "\033[?25l");
    uint32_t seed = 0x9e3779b9u;
    static char chunk[8192];
    double total = 0, worst = 0;
    for (int f = 0; f < frames + 10; f++) {
        tt_write(t, chunk, flood_chunk(kind, &seed, chunk, sizeof(chunk)));
        assert(wsap_vtcore_snapshot_update(snap, t));
        struct terminal_frame frame;
        terminal_frame_init(&frame, snap, false);
        double begin = now();
        draw_terminal_with_history(r, &render_history, px, w, h, f & 1, &frame);
        double dt = now() - begin;
        if (f >= 10) {
            total += dt;
            if (dt > worst) worst = dt;
        }
    }
    printf("flood-%-10s %.3f ms/frame worst=%.3f ms\n", name, total * 1000 / frames,
           worst * 1000);
    wsap_vtcore_snapshot_free(snap);
    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
}

static void benchmark(void)
{
    const char *only = getenv("FLOOD");
    for (unsigned i = 0; i < sizeof(flood_kinds) / sizeof(flood_kinds[0]); i++)
        if (!only || !strcmp(only, flood_kinds[i].name))
            flood_benchmark(flood_kinds[i].kind, flood_kinds[i].name);
    if (only)
        return;

    const char *cases[] = {"aaaaaaaaaaaaaaaaaaaaaaaa", "->===!=<=->===!=<=", "abc日本語->سلام🙂xyz"};
    const char *names[] = {"plain", "ligatures", "mixed"};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        struct wsap_vtcore *t = tt_new(120, 20);
        feed(t, "\033[?25l");
        for (int i = 0; i < 150; i++) feed(t, cases[c]);
        int w = 120 * CELL_W, h = 20 * CELL_H;
        uint32_t *pixels = malloc((size_t)w * h * 4);
        assert(t && pixels);
        test_render_paint_full(pixels, w, h, true, t);
        double start = now();
        for (int i = 0; i < 10; i++) test_render_paint_full(pixels, w, h, true, t);
        printf("%-10s %.3f ms/frame pixels=%016llx\n", names[c],
               (now() - start) * 100, (unsigned long long)hash(pixels, (size_t)w * h));
        free(pixels); wsap_vtcore_free(t);
    }
    struct wsap_vtcore *t = tt_new(120, 40);
    int w = 120 * CELL_W, h = 40 * CELL_H;
    uint32_t *pixels = malloc((size_t)w * h * sizeof(*pixels));
    assert(t && pixels);
    uint32_t seed = 1;
    uint64_t checksum = 0;
    double start = now();
    for (int frame = 0; frame < 100; frame++) {
        char bytes[8192];
        for (size_t i = 0; i < sizeof(bytes); i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            bytes[i] = (char)seed;
        }
        tt_write(t, bytes, sizeof(bytes));
        test_render_paint_full(pixels, w, h, true, t);
        checksum ^= hash(pixels, (size_t)w * h);
    }
    printf("random     %.3f ms/frame pixels=%016llx\n",
           (now() - start) * 10, (unsigned long long)checksum);
    free(pixels); wsap_vtcore_free(t);

    {
        struct wsap_vtcore *st = tt_new(120, 40);
        int sw = 120 * CELL_W, sh = 40 * CELL_H;
        uint32_t *spx = malloc((size_t)sw * sh * sizeof(*spx));
        struct renderer *sr = test_renderer();
        struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
        uint32_t *full = malloc((size_t)sw * sh * sizeof(*full));
        assert(st && spx && sr && snap && full);
        feed(st, "\033[?25l");
        for (int i = 0; i < 40; i++)
            feed(st, "The quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n");
        assert(wsap_vtcore_snapshot_update(snap, st));
        struct terminal_frame frame;
        terminal_frame_init(&frame, snap, false);
        struct framebuffer_history history = {0};
        draw_terminal_with_history(sr, &history, spx, sw, sh, 0, &frame);
        double shift_time = 0, full_time = 0;
        for (int n = 0; n < 100; n++) {
            feed(st, "A new scrolled line in the terminal grid 0123456789 abcdefghijklmnopqrstuvwxyz\r\n");
            assert(wsap_vtcore_snapshot_update(snap, st));
            terminal_frame_init(&frame, snap, false);
            assert(frame.scrolled == 1);
            double begin = now();
            draw_terminal_with_history(sr, &history, spx, sw, sh, 0, &frame);
            shift_time += now() - begin;
            begin = now();
            render_paint_full_terminal_frame(test_font(), full, sw, sh, &frame);
            full_time += now() - begin;
            assert(!memcmp(spx, full, (size_t)sw * sh * sizeof(*spx)));
        }
        printf("scroll     %.3f ms/frame full=%.3f ms/frame gain=%.2fx pixels=%016llx\n",
               shift_time * 10, full_time * 10, full_time / shift_time,
               (unsigned long long)hash(spx, (size_t)sw * sh));
        wsap_vtcore_snapshot_free(snap);
        framebuffer_history_release(&history);
        free(full);
        renderer_destroy(sr);
        free(spx);
        wsap_vtcore_free(st);
    }
}
static void scroll_pixels_test(void)
{
    struct wsap_vtcore *t = tt_new(40, 8);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    struct renderer *r = test_renderer();
    int w = 40 * CELL_W + 3, h = 8 * CELL_H + 5;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *px[2] = { calloc(1, bytes), calloc(1, bytes) };
    uint32_t *full = calloc(1, bytes);
    assert(t && snap && r && px[0] && px[1] && full);
    for (int i = 0; i < 12; i++) {
        char line[80];
        snprintf(line, sizeof(line), "row %02d -> café 日本語\r\n", i);
        feed(t, line);
    }
    for (int step = 0; step < 14; step++) {
        if (step) {
            char line[80];
            snprintf(line, sizeof(line), "next %02d -> 日本語\r\n", step);
            feed(t, line);
        }
        if (step == 4)
            tt_select(t, 1, 2, 10, 4);
        if (step == 7)
            wsap_vtcore_select_clear(t);
        assert(wsap_vtcore_snapshot_update(snap, t));
        struct terminal_frame frame;
        terminal_frame_init(&frame, snap, true);
        if (step)
            assert(frame.scrolled == 1);
        render_paint_full_terminal_frame(test_font(), full, w, h, &frame);
        int buf = step % 2;
        draw_terminal_with_history(r, &render_history, px[buf],
                                   w, h, buf, &frame);
        assert(!memcmp(px[buf], full, bytes));
        draw_terminal_with_history(r, &render_history, px[buf],
                                   w, h, buf, &frame);
        assert(!memcmp(px[buf], full, bytes));
        draw_terminal_with_history(r, &render_history, px[1 - buf],
                                   w, h, 1 - buf, &frame);
        assert(!memcmp(px[1 - buf], full, bytes));
    }
    renderer_destroy(r);
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
    free(px[0]); free(px[1]); free(full);
}

static void surface_histories_are_independent_test(void)
{
    struct wsap_vtcore *a = tt_new(12, 3);
    struct wsap_vtcore *b = tt_new(12, 3);
    struct wsap_vtcore_snapshot *sa = wsap_vtcore_snapshot_new();
    struct wsap_vtcore_snapshot *sb = wsap_vtcore_snapshot_new();
    struct renderer *r = test_renderer();
    const int w = 12 * CELL_W, h = 3 * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *pa = calloc(1, bytes), *pb = calloc(1, bytes);
    uint32_t *ea = malloc(bytes), *eb = malloc(bytes);
    struct framebuffer_history ha = {0}, hb = {0};
    struct damage_list da = {0}, db = {0};
    assert(a && b && sa && sb && r && pa && pb && ea && eb);
    feed(a, "AAAA");
    feed(b, "BBBB");
    assert(wsap_vtcore_snapshot_update(sa, a));
    assert(wsap_vtcore_snapshot_update(sb, b));
    struct terminal_frame fa, fb;
    terminal_frame_init(&fa, sa, false);
    terminal_frame_init(&fb, sb, false);
    render_paint_full_terminal_frame(test_font(), ea, w, h, &fa);
    render_paint_full_terminal_frame(test_font(), eb, w, h, &fb);
    struct framebuffer ta = {
        .pixels = pa, .size = { .x = w, .y = h }, .buffer = 0,
        .damage = &da, .history = &ha,
    };
    struct framebuffer tb = {
        .pixels = pb, .size = { .x = w, .y = h }, .buffer = 0,
        .damage = &db, .history = &hb,
    };
    struct render_frame frame = { .font = test_font(), .has_grid = true };
    frame.grid = fa;
    (void)render_draw(r, &ta, &frame);
    frame.grid = fb;
    (void)render_draw(r, &tb, &frame);
    assert(!memcmp(pa, ea, bytes));
    assert(!memcmp(pb, eb, bytes));
    frame.grid = fa;
    (void)render_draw(r, &ta, &frame);
    assert(!memcmp(pa, ea, bytes));
    assert(!memcmp(pb, eb, bytes));

    struct font_instance *other = font_instance_acquire(
        font_instance_size_pt(test_font()) + 2,
        font_instance_scale_120(test_font()));
    const int rw = w + 7, rh = h + 5;
    size_t resized_bytes = (size_t)rw * rh * sizeof(uint32_t);
    uint32_t *resized = calloc(1, resized_bytes);
    uint32_t *resized_expected = malloc(resized_bytes);
    assert(other && resized && resized_expected);
    ta.pixels = resized;
    ta.size = (struct vector2i){ .x = rw, .y = rh };
    frame.font = other;
    render_paint_full_terminal_frame(other, resized_expected, rw, rh, &fa);
    (void)render_draw(r, &ta, &frame);
    assert(!memcmp(resized, resized_expected, resized_bytes));
    font_instance_unref(other);
    free(resized); free(resized_expected);
    framebuffer_history_release(&ha);
    framebuffer_history_release(&hb);
    wsap_vtcore_snapshot_free(sa);
    wsap_vtcore_snapshot_free(sb);
    renderer_destroy(r);
    free(pa); free(pb); free(ea); free(eb);
    wsap_vtcore_free(a); wsap_vtcore_free(b);
}

static void empty_frame_clears_and_invalidates_grid_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 3);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    struct renderer *r = test_renderer();
    const int w = 8 * CELL_W, h = 3 * CELL_H;
    size_t count = (size_t)w * h;
    uint32_t *px = calloc(count, sizeof(*px));
    uint32_t *expected = malloc(count * sizeof(*expected));
    struct damage_list damage = {0};
    struct framebuffer_history history = {0};
    assert(t && snap && r && px && expected);
    feed(t, "resume me");
    assert(wsap_vtcore_snapshot_update(snap, t));
    struct terminal_frame grid;
    terminal_frame_init(&grid, snap, false);
    struct framebuffer target = {
        .pixels = px, .size = { .x = w, .y = h }, .buffer = 0,
        .damage = &damage, .history = &history,
    };
    struct render_frame frame = {
        .font = test_font(), .grid = grid, .has_grid = true,
    };
    render_paint_full_terminal_frame(test_font(), expected, w, h, &grid);
    (void)render_draw(r, &target, &frame);
    assert(!memcmp(px, expected, count * sizeof(*px)));

    frame = (struct render_frame){ .base = 0xFF123456u };
    damage = (struct damage_list){0};
    (void)render_draw(r, &target, &frame);
    assert(damage.all);
    for (size_t i = 0; i < count; i++)
        assert(px[i] == frame.base);

    frame = (struct render_frame){
        .font = test_font(), .grid = grid, .has_grid = true,
    };
    damage = (struct damage_list){0};
    (void)render_draw(r, &target, &frame);
    assert(!memcmp(px, expected, count * sizeof(*px)));
    framebuffer_history_release(&history);
    wsap_vtcore_snapshot_free(snap);
    renderer_destroy(r);
    free(px); free(expected); wsap_vtcore_free(t);
}

static void gpu_failure_retries_cpu_frame_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 3);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    struct renderer *r = test_renderer();
    const int w = 8 * CELL_W, h = 3 * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *px = calloc(1, bytes);
    uint32_t *expected = malloc(bytes);
    struct damage_list damage = {0};
    struct framebuffer_history history = {0};
    int native_drawable;
    assert(t && snap && r && px && expected);
    feed(t, "GPU retry");
    assert(wsap_vtcore_snapshot_update(snap, t));
    struct terminal_frame grid;
    terminal_frame_init(&grid, snap, false);
    render_paint_full_terminal_frame(test_font(), expected, w, h, &grid);
    struct framebuffer target = {
        .pixels = px, .size = { .x = w, .y = h }, .buffer = 0,
        .damage = &damage, .drawable = &native_drawable, .transaction = true,
        .history = &history,
    };
    struct render_frame frame = {
        .font = test_font(), .grid = grid, .has_grid = true,
    };
    native_gpu_failure_frame = true;
    render_test_gpu_redraw_failure(true);
    (void)render_draw(r, &target, &frame);
    native_gpu_failure_frame = false;
    assert(target.presented);
    assert(!memcmp(px, expected, bytes));
    framebuffer_history_release(&history);
    wsap_vtcore_snapshot_free(snap);
    renderer_destroy(r);
    free(px); free(expected); wsap_vtcore_free(t);
}

static void popup_cleared_target_test(void)
{
    const int w = 32, h = 16;
    uint32_t px[w * h];
    window_pixels = px;
    window_width = w;
    window_height = h;
    struct renderer *r = test_renderer();
    assert(r);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect(&scene,
        (struct rect2i){ .pos = { .x = 4, .y = 4 },
                         .size = { .x = 10, .y = 8 } }, 0xFF112233u));
    const uint32_t ink = 0xFF112233u;

    struct framebuffer t = test_target();

    memset(px, 0, sizeof(px));
    draw_scene(r, &t, &scene, NULL, (struct rect2i){0}, 0);
    assert(px[6 * w + 6] == ink);

    memset(px, 0, sizeof(px));
    draw_scene(r, &t, &scene, NULL, (struct rect2i){0}, 0);
    assert(px[6 * w + 6] == ink);

    memset(px, 0, sizeof(px));
    draw_scene(r, &t, &scene, NULL, (struct rect2i){0}, 0);
    assert(px[6 * w + 6] == ink);

    memset(px, 0, sizeof(px));
    draw_scene(r, &t, &scene, &(struct ui_damage){0}, (struct rect2i){0}, 0);
    assert(px[6 * w + 6] == 0);

    renderer_destroy(r);
    ui_scene_destroy(&scene);
    window_pixels = NULL;
}

static void overlay_grid_repaint_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    assert(t);
    feed(t, "\033[?25l");
    feed(t, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
    int w = 8 * CELL_W, h = 4 * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(*window_pixels);
    window_pixels = malloc(bytes);
    window_width = w;
    window_height = h;
    assert(window_pixels);
    struct renderer *r = test_renderer();
    assert(r);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    const uint32_t ink = 0xFF112233u;
    assert(ui_scene_rect(&scene,
        (struct rect2i){ .pos = { .x = 0, .y = CELL_H },
                         .size = { .x = w, .y = CELL_H } }, ink));

    struct framebuffer rt = test_target();
    const size_t probe = (size_t)(CELL_H + 3) * w + (w - 5);

    const struct rect2i grid = { .size = { .x = w, .y = h } };
    const struct ui_damage none = {0};

    memset(window_pixels, 0, bytes);
    struct render_frame input = {
        .font = test_font(), .ui_font = test_font(),
        .terminal = grid, .paint = &scene,
        .grid = {0},
    };
    test_frame_init(&input.grid, t, false);
    input.has_grid = true;
    (void)render_draw(r, &rt, &input);
    assert(window_pixels[probe] == ink);

    feed(t, "\033[2;1HZZZZ");
    input.damage = &none;
    test_frame_init(&input.grid, t, false);
    (void)render_draw(r, &rt, &input);
    assert(window_pixels[probe] == ink);

    renderer_destroy(r);
    ui_scene_destroy(&scene);
    free(window_pixels);
    window_pixels = NULL;
    wsap_vtcore_free(t);
}

static void vector_fallback_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 2);
    assert(t);
    feed(t, "\033[?25lABC");
    window_width = 8 * CELL_W;
    window_height = 2 * CELL_H;
    size_t bytes = (size_t)window_width * window_height * 4;
    window_pixels = malloc(bytes);
    uint32_t *expected = malloc(bytes);
    assert(window_pixels && expected);
    memset(window_pixels, 0x55, bytes);
    struct renderer *r = test_renderer();
    assert(r);
    vector_enabled = vector_accept = true;
    vector_submitted = false;
    { struct framebuffer rt = test_target(); draw_terminal_test(r, &rt, t, true); }
    assert(vector_submitted);
    for (size_t i = 0; i < bytes / 4; i++) assert(window_pixels[i] == 0x55555555);

    vector_accept = false;
    memset(window_pixels, 0x55, bytes);
    { struct framebuffer rt = test_target(); draw_terminal_test(r, &rt, t, true); }
    for (size_t i = 0; i < bytes / 4; i++) assert(window_pixels[i] == 0x55555555);

    vector_accept = true;
    const char *steps[] = { "\r\033[4mABC", "\033[0m\r日本語", "\r\xc2\x80" };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(*steps); i++) {
        feed(t, steps[i]);
        vector_submitted = false;
        memset(window_pixels, 0x77, bytes);
        { struct framebuffer rt = test_target(); draw_terminal_test(r, &rt, t, true); }
        assert(vector_submitted);
        for (size_t j = 0; j < bytes / 4; j++) assert(window_pixels[j] == 0x77777777);
    }
    vector_enabled = false;
    renderer_destroy(r);
    free(expected); free(window_pixels); window_pixels = NULL;
    wsap_vtcore_free(t);
}

static void cpu_target_skips_gpu_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 2);
    assert(t);
    feed(t, "\033[?25lABC");
    window_width = 8 * CELL_W;
    window_height = 2 * CELL_H;
    size_t n = (size_t)window_width * window_height;
    window_pixels = malloc(n * sizeof(*window_pixels));
    assert(window_pixels);
    for (size_t i = 0; i < n; i++)
        window_pixels[i] = 0x77777777u;
    struct renderer *r = test_renderer();
    assert(r);
    vector_enabled = vector_accept = true;
    vector_submitted = false;
    struct framebuffer rt = test_target();
    rt.gpu = NULL;
    draw_terminal_test(r, &rt, t, true);
    assert(!vector_submitted);
    size_t painted = 0;
    for (size_t i = 0; i < n; i++)
        painted += window_pixels[i] != 0x77777777u;
    assert(painted > 0);
    vector_enabled = false;
    renderer_destroy(r);
    free(window_pixels);
    window_pixels = NULL;
    wsap_vtcore_free(t);
}

static void image_op_paints_and_clips_test(void)
{
    uint32_t src[4 * 3];
    for (int i = 0; i < 12; i++)
        src[i] = 0xFF000000u | (uint32_t)(i + 1);
    struct ui_image img = { .pixels = src, .size = { .x = 4, .y = 3 }, .gen = 1 };

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_push_clip(&scene, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 5, .y = 10 } }));
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 2, .y = 1 }, &img));
    assert(ui_scene_pop_clip(&scene));
    assert(ui_scene_is_valid(&scene));

    uint32_t px[8 * 6] = {0};
    struct damage_list changed = {0};
    struct framebuffer_history history = {0};
    struct framebuffer target = {
        .pixels = px,
        .size = { .x = 8, .y = 6 },
        .damage = &changed,
        .history = &history,
    };
    struct ui_damage_tracker tracker = {0};
    struct ui_damage damage;
    ui_damage_track(&tracker, &scene, &damage);
    struct render_frame frame = {
        .ui_font = test_font(),
        .paint = &scene,
        .damage = &damage,
    };
    struct renderer *r = test_renderer();
    assert(r);
    (void)render_draw(r, &target, &frame);
    assert(px[1 * 8 + 2] == src[0]);
    assert(px[3 * 8 + 4] == src[2 * 4 + 2]);
    assert(px[1 * 8 + 5] == 0);
    assert(px[4 * 8 + 2] == 0);

    uint32_t before = px[1 * 8 + 2];
    src[0] = 0xFFABCDEFu;
    img.gen = 2;
    changed = (struct damage_list){0};
    ui_scene_init(&scene);
    assert(ui_scene_push_clip(&scene, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 5, .y = 10 } }));
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 2, .y = 1 }, &img));
    assert(ui_scene_pop_clip(&scene));
    ui_damage_track(&tracker, &scene, &damage);
    frame.damage = &damage;
    (void)render_draw(r, &target, &frame);
    assert(px[1 * 8 + 2] == src[0]);
    assert(px[1 * 8 + 2] != before);
    ui_damage_tracker_destroy(&tracker);
    framebuffer_history_release(&history);
    renderer_destroy(r);

    ui_scene_init(&scene);
    assert(ui_scene_push_clip(&scene, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 2, .y = 2 } }));
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 4, .y = 4 }, &img));
    assert(ui_scene_pop_clip(&scene));
    uint32_t clip_px[8 * 6] = {0};
    assert(ui_scene_paint_clipped(test_font(), clip_px, 8, 6, &scene,
                                  (struct rect2i){ .size = { .x = 8, .y = 6 } }));
    for (int i = 0; i < 48; i++)
        assert(clip_px[i] == 0);

    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 6, .y = 4 }, &img));
    assert(ui_scene_paint_clipped(test_font(), clip_px, 8, 6, &scene,
                                  (struct rect2i){ .size = { .x = 10, .y = 10 } }));
    assert(clip_px[4 * 8 + 6] == src[0]);
    assert(clip_px[4 * 8 + 7] == src[1]);
    assert(clip_px[5 * 8 + 6] == src[4]);
    assert(clip_px[5 * 8 + 7] == src[5]);
    ui_scene_destroy(&scene);
}

static void blend_rect_alpha_test(void)
{
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_blend_rect(&scene, (struct rect2i){
        .size = { .x = 2, .y = 2 } }, 0x112233, 255));
    uint32_t px[4 * 4];
    for (int i = 0; i < 16; i++)
        px[i] = 0xFF000000u;
    assert(ui_scene_paint_clipped(test_font(), px, 4, 4, &scene,
                                  (struct rect2i){ .size = { .x = 4, .y = 4 } }));
    assert(px[0] == 0xFF112233u && px[1 * 4 + 1] == 0xFF112233u);
    assert(px[2 * 4 + 2] == 0xFF000000u);

    ui_scene_init(&scene);
    assert(ui_scene_blend_rect(&scene, (struct rect2i){
        .size = { .x = 2, .y = 2 } }, 0x000000, 254));
    for (int i = 0; i < 16; i++)
        px[i] = 0xFFFFFFFFu;
    assert(ui_scene_paint_clipped(test_font(), px, 4, 4, &scene,
                                  (struct rect2i){ .size = { .x = 4, .y = 4 } }));
    assert(px[0] == 0xFF000000u);
    assert(px[1 * 4 + 1] == 0xFF000000u);
    ui_scene_destroy(&scene);
}

static void uninitialized_srgb_luts_test(void)
{
    unsigned blended = render_blend_channel(255, 0, 128);
    assert(blended > 180 && blended < 195);
    uint32_t px = render_blend_px(0xFFFFFFFFu, 0xFF000000u, 128);
    assert((px & 0xFF) == 128);
}

static void repaint_emits_damage_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 2);
    feed(t, "ABCD");
    int w = 8 * CELL_W, h = 2 * CELL_H;
    uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
    assert(px);
    struct damage_list damage = {0};
    struct framebuffer_history history = {0};
    struct framebuffer target = {
        .pixels = px,
        .size = { .x = w, .y = h },
        .damage = &damage,
        .history = &history,
    };
    struct render_frame frame = {
        .font = test_font(),
        .terminal = { .size = { .x = w, .y = h } },
        .has_grid = true,
    };
    test_frame_init(&frame.grid, t, false);
    struct renderer *r = test_renderer();
    (void)render_draw(r, &target, &frame);

    damage = (struct damage_list){0};
    frame.repaint = true;
    struct damage_list dmg = render_draw(r, &target, &frame);
    assert(!rect2i_is_empty(dmg.bounds));
    assert(dmg.bounds.width == w && dmg.bounds.height == h);

    framebuffer_history_release(&history);
    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
}

static void null_history_draw_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 2);
    feed(t, "ABCD");
    int w = 8 * CELL_W, h = 2 * CELL_H;
    uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
    assert(px);
    struct damage_list damage = {0};
    struct framebuffer target = {
        .pixels = px,
        .size = { .x = w, .y = h },
        .buffer = 0,
        .damage = &damage,
        .history = NULL,
    };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect(&scene, (struct rect2i){ .size = { .x = w, .y = h } }, 0xFF112233u));
    struct render_frame frame = {
        .font = test_font(),
        .ui_font = test_font(),
        .terminal = { .size = { .x = w, .y = h } },
        .has_grid = true,
        .paint = &scene,
    };
    test_frame_init(&frame.grid, t, false);
    struct renderer *r = test_renderer();
    (void)render_draw(r, &target, &frame);
    assert(px[0] == 0xFF112233u);

    renderer_destroy(r);
    ui_scene_destroy(&scene);
    free(px);
    wsap_vtcore_free(t);
}

static void pill_rounded_rect_symmetry_test(void)
{
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rounded_rect(&scene, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 10, .y = 20 } }, 5, 0xFFFFFFFFu));
    uint32_t px[10 * 20] = {0};
    assert(ui_scene_paint(test_font(), px, 10, 20, &scene));
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 5; x++) {
            assert(px[y * 10 + x] == px[y * 10 + (9 - x)]);
        }
    }
    for (int y = 0; y < 10; y++) {
        for (int x = 0; x < 10; x++) {
            assert(px[y * 10 + x] == px[(19 - y) * 10 + x]);
        }
    }
    assert(px[4] != 0 && px[5] != 0);
    ui_scene_destroy(&scene);
}

static void single_threaded_render_shutdown_cleanup_test(void)
{
    render_init();
    struct wsap_vtcore *t = tt_new(8, 2);
    feed(t, "\033[?25lABCDEFGH");
    int w = 8 * CELL_W, h = 2 * CELL_H;
    uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
    assert(t && px);

    setenv("WSAP_NO_PARALLEL", "1", 1);
    struct terminal_frame frame;
    test_frame_init(&frame, t, false);
    render_paint_full_terminal_frame(test_font(), px, w, h, &frame);
    unsetenv("WSAP_NO_PARALLEL");

    render_shutdown();
    free(px);
    wsap_vtcore_free(t);
}

int main(int argc, char **argv)
{
    uninitialized_srgb_luts_test();
    single_threaded_render_shutdown_cleanup_test();
    render_init();
    if (argc > 1 && !strcmp(argv[1], "--bench")) benchmark();
    else {
        image_op_paints_and_clips_test();
        blend_rect_alpha_test();
        repaint_emits_damage_test();
        null_history_draw_test();
        pill_rounded_rect_symmetry_test();
        renderer_lifecycle_test(); worker_pool_lifecycle_test();
#ifdef __linux__
        scale_respawn_test();
#endif
        clipped_target_test(); style_cache_test();
        presentation_test();
        theme_redraw_test();
        theme_recolors_existing_text_test();
        frame_colors_test(2);
        frame_colors_test(6);
        scroll_pixels_test();
        surface_histories_are_independent_test();
        empty_frame_clears_and_invalidates_grid_test();
        gpu_failure_retries_cpu_frame_test();
        incremental_test(); suffix_ligature_test(); run_drift_test(); cache_churn_test();
        linear_blend_test();
        color_glyph_blend_test();
        oversized_frame_test();
        popup_cleared_target_test();
        overlay_grid_repaint_test();
        vector_fallback_test();
        cpu_target_skips_gpu_test();
        test_font_override = font_instance_acquire(
            font_instance_size_pt(font_instance_default()), 180);
        assert(test_font_override);
        clipped_target_test(); style_cache_test();
        scroll_pixels_test();
        incremental_test(); suffix_ligature_test(); run_drift_test(); cache_churn_test();
        linear_blend_test();
        vector_fallback_test();
        font_instance_unref(test_font_override);
        test_font_override = NULL;
        puts("PASS: renderer pixel regressions");
    }
    framebuffer_history_release(&render_history);
    render_shutdown();
}
