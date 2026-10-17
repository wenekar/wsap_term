#define _POSIX_C_SOURCE 200809L
/*
 * Pixel regressions of the renderer, through render_draw() and the scene
 * painter only. The full-paint oracle is a second renderer with no cell
 * cache and no workers that paints a frame with no history.
 *
 * With --bench, the file is render-bench: the flood cases, the
 * incremental cases on one persistent canvas, and the full-paint cases.
 */
#include "render/render.h"
#include "../helpers/render_frame_helpers.h"
#include "../helpers/render_draw.h"
#include "paint/blend.h"
#include "paint/damage.h"
#include "paint/scene.h"
#include "ui/damage.h"
#include "font/font.h"
#include "font/glyph_cache.h"
#include "paint/framebuffer.h"
#include "render/term_frame.h"
#include "render/scene_paint.h"
#include "term_api.h"
#include "helpers/flood.h"

#define CELL_W (font_instance_metrics(test_font())->cell_w)
#define CELL_H (font_instance_metrics(test_font())->cell_h)
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct renderer *test_renderer(void)
{
    return renderer_create(NULL);
}

static void feed(struct wsap_vtcore *t, const char *s) { wsap_vtcore_feed(t, s, strlen(s), 0); }

static const struct ui_damage no_ui_damage = {0};

/*
 * Paints `grid` in `font` over the whole canvas, with no UI. `history`
 * NULL keeps no state.
 */
static struct render_result draw_grid(struct renderer *r, uint32_t *px, struct vector2i size,
                                      struct framebuffer_history *history, bool fresh,
                                      const struct terminal_frame *grid,
                                      struct font_instance *font)
{
    struct framebuffer target = {
        .pixels = px, .size = size, .fresh = fresh, .history = history,
    };
    struct render_frame frame = {
        .grid = { .frame = grid, .font = font, .rect = { .size = size } },
        .ui_font = font,
        .ui_damage = &no_ui_damage,
    };
    return render_draw(r, &target, &frame);
}

static struct render_result draw_scene(struct renderer *r, uint32_t *px, struct vector2i size,
                                       struct framebuffer_history *history, bool fresh,
                                       const struct ui_scene *scene,
                                       const struct ui_damage *damage, uint32_t base)
{
    struct framebuffer target = {
        .pixels = px, .size = size, .fresh = fresh, .history = history,
    };
    struct render_frame frame = {
        .base = base,
        .ui = scene,
        .ui_font = test_font(),
        .ui_damage = damage,
    };
    return render_draw(r, &target, &frame);
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

/* ---- Lifecycle ---------------------------------------------------------- */

static void renderer_lifecycle_test(void)
{
    struct renderer *r = test_renderer();
    assert(r);
    struct wsap_vtcore *t = tt_new(1, 1);
    assert(t);
    feed(t, "\033[?25l");
    uint32_t pixels[CELL_W * CELL_H];
    test_renderer_paint(r, pixels, CELL_W, CELL_H, t, true);
    for (size_t i = 0; i < sizeof(pixels) / sizeof(*pixels); i++)
        assert(pixels[i] == DEFAULT_BG);
    test_renderer_destroy(r);
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

/* A joined thread can stay in /proc for a short time. */
static bool thread_count_settles(int want)
{
    for (int i = 0; i < 200; i++) {
        if (live_thread_count() == want)
            return true;
        nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
    }
    return false;
}
#endif

/* Each renderer owns its workers: create starts them, destroy joins them. */
static void worker_pool_lifecycle_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 10);
    assert(t);
    feed(t, "\033[?25lParallel rendering lifecycle test");
    int w = 8 * CELL_W, h = 10 * CELL_H;
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *want = malloc(bytes), *got = malloc(bytes);
    assert(want && got);
    test_render_paint_full(want, w, h, true, t);
#ifdef __linux__
    int before = live_thread_count();
#endif
    for (int round = 0; round < 3; round++) {
        struct renderer *r = renderer_create(&(struct render_options){ .threads = 3 });
        assert(r);
#ifdef __linux__
        assert(live_thread_count() == before + 3);
#endif
        memset(got, 0x5a, bytes);
        test_renderer_paint(r, got, w, h, t, true);
        assert(!memcmp(want, got, bytes));
        test_renderer_destroy(r);
#ifdef __linux__
        assert(thread_count_settles(before));
#endif
    }
    struct renderer *r = renderer_create(&(struct render_options){ .threads = RENDER_NONE });
    assert(r);
#ifdef __linux__
    assert(live_thread_count() == before);
#endif
    test_renderer_paint(r, got, w, h, t, true);
    assert(!memcmp(want, got, bytes));
    test_renderer_destroy(r);
    free(want);
    free(got);
    wsap_vtcore_free(t);
}

#ifdef __linux__
/* A font of another scale changes the cell size. It starts no thread. */
static void scale_respawn_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 10);
    assert(t);
    feed(t, "\033[?25lscale churn");
    int w = 8 * CELL_W, h = 10 * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *px = malloc(bytes), *want = malloc(bytes);
    assert(px && want);
    test_render_paint_full(want, w, h, true, t);

    struct renderer *r = test_renderer();
    assert(r);
    struct terminal_frame frame;
    test_frame_init(&frame, t, true);
    struct framebuffer_history history = {0};
    (void)draw_grid(r, px, size, &history, true, &frame, test_font());
    assert(!memcmp(px, want, bytes));

    int before = live_thread_count();
    struct font_instance *big = font_instance_acquire(
        font_instance_size_pt(font_instance_default()), 180);
    assert(big && big != font_instance_default());
    for (int i = 0; i < 8; i++) {
        (void)draw_grid(r, px, size, &history, false, &frame, big);
        (void)draw_grid(r, px, size, NULL, false, &frame, big);
        (void)draw_grid(r, px, size, &history, false, &frame, test_font());
        assert(!memcmp(px, want, bytes));
    }
    assert(live_thread_count() == before);
    font_instance_unref(big);

    framebuffer_history_release(&history);
    renderer_destroy(r);
    free(px);
    free(want);
    wsap_vtcore_free(t);
}
#endif

/* ---- Targets ------------------------------------------------------------ */

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
        test_renderer_paint(r, incremental, w, h, t, true);
        tcheck_same("clipped target", incremental, full, (struct vector2i){ .x = w, .y = h });
        test_renderer_paint(r, incremental, w, h, t, true);
        tcheck_same("clipped target, again", incremental, full,
                    (struct vector2i){ .x = w, .y = h });
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (x >= w / CELL_W * CELL_W || y >= 2 * CELL_H)
                    assert(full[(size_t)y * w + x] == DEFAULT_BG);
        test_renderer_destroy(r);
        free(full);
        free(incremental);
    }
    wsap_vtcore_free(t);
}

/* The frame claims 300 rows, but the snapshot has 4. */
static void oversized_frame_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    assert(t);
    feed(t, "\033[?25lABCD");
    int w = 8 * CELL_W, h = 300 * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    uint32_t *px = malloc((size_t)w * h * sizeof(*px));
    uint32_t *expected = malloc((size_t)w * h * sizeof(*expected));
    assert(px && expected);
    struct renderer *r = test_renderer();
    assert(r);

    struct terminal_frame frame;
    test_frame_init(&frame, t, true);
    frame.rows = 300;

    struct framebuffer_history history = {0};
    (void)test_draw_once(test_oracle(), expected, w, h, &frame);
    for (int i = 0; i < 4; i++) {
        (void)draw_grid(r, px, size, &history, i == 0, &frame, test_font());
        tcheck_same("oversized frame", px, expected, size);
    }
    framebuffer_history_release(&history);
    free(expected);
    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
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
    struct framebuffer_history history = {0};
    assert(t && snap && r && px && expected);
    feed(t, "resume me");
    assert(wsap_vtcore_snapshot_update(snap, t));
    struct terminal_frame grid;
    terminal_frame_init(&grid, snap, false);
    struct framebuffer target = {
        .pixels = px, .size = { .x = w, .y = h }, .fresh = true, .history = &history,
    };
    struct render_frame frame = test_grid_frame(&grid, w, h);
    (void)test_draw_once(test_oracle(), expected, w, h, &grid);
    (void)render_draw(r, &target, &frame);
    assert(!memcmp(px, expected, count * sizeof(*px)));

    target.fresh = false;
    frame = (struct render_frame){ .base = 0xFF123456u };
    struct render_result res = render_draw(r, &target, &frame);
    assert(res.damage.all);
    for (size_t i = 0; i < count; i++)
        assert(px[i] == frame.base);

    frame = test_grid_frame(&grid, w, h);
    res = render_draw(r, &target, &frame);
    assert(res.damage.all);
    assert(!memcmp(px, expected, count * sizeof(*px)));
    framebuffer_history_release(&history);
    wsap_vtcore_snapshot_free(snap);
    renderer_destroy(r);
    free(px); free(expected); wsap_vtcore_free(t);
}

static void repaint_emits_damage_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 2);
    feed(t, "ABCD");
    int w = 8 * CELL_W, h = 2 * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
    assert(px);
    struct framebuffer_history history = {0};
    struct framebuffer target = { .pixels = px, .size = size, .fresh = true, .history = &history };
    static const struct ui_damage none = {0};
    struct terminal_frame grid;
    test_frame_init(&grid, t, false);
    struct render_frame frame = test_grid_frame(&grid, w, h);
    frame.ui_damage = &none;
    struct renderer *r = test_renderer();
    (void)render_draw(r, &target, &frame);

    target.fresh = false;
    struct render_result res = render_draw(r, &target, &frame);
    assert(!res.damage.any);

    frame.repaint = true;
    res = render_draw(r, &target, &frame);
    assert(res.damage.any);
    assert(res.damage.all || (res.damage.bounds.width == w && res.damage.bounds.height == h));
    assert(tdamage_area(&res.damage, size) == (long)w * h);

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
    struct framebuffer target = { .pixels = px, .size = { .x = w, .y = h } };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect(&scene, (struct rect2i){ .size = { .x = w, .y = h } }, 0xFF112233u));
    struct terminal_frame grid;
    test_frame_init(&grid, t, false);
    struct render_frame frame = test_grid_frame(&grid, w, h);
    frame.ui = &scene;
    struct renderer *r = test_renderer();
    for (int i = 0; i < 2; i++) {
        memset(px, 0, (size_t)w * h * sizeof(uint32_t));
        struct render_result res = render_draw(r, &target, &frame);
        assert(res.damage.all);
        for (int p = 0; p < w * h; p++)
            assert(px[p] == 0xFF112233u);
    }

    renderer_destroy(r);
    ui_scene_destroy(&scene);
    free(px);
    wsap_vtcore_free(t);
}

/*
 * A popup canvas over base 0. NULL UI damage repaints the scene. Empty UI
 * damage trusts the canvas, so a canvas that the caller cleared must be
 * fresh.
 */
static void popup_cleared_target_test(void)
{
    enum { W = 32, H = 16 };
    uint32_t px[W * H];
    struct vector2i size = { .x = W, .y = H };
    struct renderer *r = test_renderer();
    assert(r);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect(&scene,
        (struct rect2i){ .pos = { .x = 4, .y = 4 },
                         .size = { .x = 10, .y = 8 } }, 0xFF112233u));
    const uint32_t ink = 0xFF112233u;
    static const struct ui_damage none = {0};
    struct framebuffer_history history = {0};

    for (int i = 0; i < 3; i++) {
        memset(px, 0, sizeof(px));
        (void)draw_scene(r, px, size, &history, i == 0, &scene, NULL, 0);
        assert(px[6 * W + 6] == ink);
        assert(px[0] == 0);
    }

    memset(px, 0, sizeof(px));
    struct render_result res = draw_scene(r, px, size, &history, false, &scene, &none, 0);
    assert(!res.damage.any);
    assert(px[6 * W + 6] == 0);

    res = draw_scene(r, px, size, &history, true, &scene, &none, 0);
    assert(res.damage.all);
    assert(px[6 * W + 6] == ink);

    framebuffer_history_release(&history);
    renderer_destroy(r);
    ui_scene_destroy(&scene);
}

/* An opaque UI rect over a grid row stays on top when that row repaints. */
static void overlay_grid_repaint_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    assert(t);
    feed(t, "\033[?25l");
    feed(t, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
    int w = 8 * CELL_W, h = 4 * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *px = calloc(1, bytes), *want = malloc(bytes);
    assert(px && want);
    struct renderer *r = test_renderer();
    assert(r);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    const uint32_t ink = 0xFF112233u;
    assert(ui_scene_rect(&scene,
        (struct rect2i){ .pos = { .x = 0, .y = CELL_H },
                         .size = { .x = w, .y = CELL_H } }, ink));

    const size_t probe = (size_t)(CELL_H + 3) * w + (w - 5);
    static const struct ui_damage none = {0};
    struct framebuffer_history history = {0};
    struct framebuffer target = { .pixels = px, .size = size, .fresh = true, .history = &history };

    struct terminal_frame grid;
    test_frame_init(&grid, t, false);
    struct render_frame input = test_grid_frame(&grid, w, h);
    input.ui = &scene;
    (void)render_draw(r, &target, &input);
    assert(px[probe] == ink);

    feed(t, "\033[2;1HZZZZ");
    target.fresh = false;
    input.ui_damage = &none;
    test_frame_init(&grid, t, false);
    (void)render_draw(r, &target, &input);
    assert(px[probe] == ink);
    (void)tdraw_fresh(test_oracle(), want, size, &input);
    tcheck_same("overlay over a repainted row", px, want, size);

    framebuffer_history_release(&history);
    renderer_destroy(r);
    ui_scene_destroy(&scene);
    free(px);
    free(want);
    wsap_vtcore_free(t);
}

/* ---- Scroll and surfaces ------------------------------------------------ */

/*
 * One line scrolls in per step. The persistent canvas equals a fresh paint,
 * and the rows that only moved show the pixels they showed one row lower.
 * A second canvas sees every other frame, so it scrolls by two lines.
 */
static void scroll_pixels_test(void)
{
    struct wsap_vtcore *t = tt_new(40, 8);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    struct renderer *r = test_renderer();
    int w = 40 * CELL_W + 3, h = 8 * CELL_H + 5;
    struct vector2i size = { .x = w, .y = h };
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *px = calloc(1, bytes), *px2 = calloc(1, bytes);
    uint32_t *full = calloc(1, bytes), *before = calloc(1, bytes);
    assert(t && snap && r && px && px2 && full && before);
    for (int i = 0; i < 12; i++) {
        char line[80];
        snprintf(line, sizeof(line), "row %02d -> café 日本語\r\n", i);
        feed(t, line);
    }
    struct framebuffer_history history = {0}, history2 = {0};
    bool had_selection = false;
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
        (void)test_draw_once(test_oracle(), full, w, h, &frame);
        memcpy(before, px, bytes);
        struct render_result res = draw_grid(r, px, size, &history, step == 0, &frame, test_font());
        tcheck_same("scroll", px, full, size);
        tcheck_damage("scroll", before, px, size, &res.damage);
        if (step && !had_selection && !frame.has_selection) {
            /* Rows 0..5 now show what rows 1..6 showed. */
            for (int y = 0; y < 6 * CELL_H; y++)
                assert(!memcmp(px + (size_t)y * w, before + (size_t)(y + CELL_H) * w,
                               (size_t)w * sizeof(uint32_t)));
        }
        had_selection = frame.has_selection;
        res = draw_grid(r, px, size, &history, false, &frame, test_font());
        assert(!res.damage.any);
        tcheck_same("scroll, idle", px, full, size);
        if (step % 2 == 0) {
            (void)draw_grid(r, px2, size, &history2, step == 0, &frame, test_font());
            tcheck_same("scroll, second canvas", px2, full, size);
        }
    }
    framebuffer_history_release(&history);
    framebuffer_history_release(&history2);
    renderer_destroy(r);
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
    free(px); free(px2); free(full); free(before);
}

/* Two canvases with their own histories do not see each other's frames. */
static void surface_histories_are_independent_test(void)
{
    struct wsap_vtcore *a = tt_new(12, 3);
    struct wsap_vtcore *b = tt_new(12, 3);
    struct wsap_vtcore_snapshot *sa = wsap_vtcore_snapshot_new();
    struct wsap_vtcore_snapshot *sb = wsap_vtcore_snapshot_new();
    struct renderer *r = test_renderer();
    const int w = 12 * CELL_W, h = 3 * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *pa = calloc(1, bytes), *pb = calloc(1, bytes);
    uint32_t *ea = malloc(bytes), *eb = malloc(bytes);
    struct framebuffer_history ha = {0}, hb = {0};
    assert(a && b && sa && sb && r && pa && pb && ea && eb);
    feed(a, "AAAA");
    feed(b, "BBBB");
    assert(wsap_vtcore_snapshot_update(sa, a));
    assert(wsap_vtcore_snapshot_update(sb, b));
    struct terminal_frame fa, fb;
    terminal_frame_init(&fa, sa, false);
    terminal_frame_init(&fb, sb, false);
    (void)test_draw_once(test_oracle(), ea, w, h, &fa);
    (void)test_draw_once(test_oracle(), eb, w, h, &fb);
    (void)draw_grid(r, pa, size, &ha, true, &fa, test_font());
    (void)draw_grid(r, pb, size, &hb, true, &fb, test_font());
    assert(!memcmp(pa, ea, bytes));
    assert(!memcmp(pb, eb, bytes));
    struct render_result res = draw_grid(r, pa, size, &ha, false, &fa, test_font());
    assert(!res.damage.any);
    assert(!memcmp(pa, ea, bytes));
    assert(!memcmp(pb, eb, bytes));

    /* A changes. B draws its own unchanged frame next: no damage. */
    feed(a, "\r\nmore");
    assert(wsap_vtcore_snapshot_update(sa, a));
    terminal_frame_init(&fa, sa, false);
    (void)test_draw_once(test_oracle(), ea, w, h, &fa);
    res = draw_grid(r, pa, size, &ha, false, &fa, test_font());
    assert(res.damage.any && !res.damage.all);
    assert(!memcmp(pa, ea, bytes));
    res = draw_grid(r, pb, size, &hb, false, &fb, test_font());
    assert(!res.damage.any);
    assert(!memcmp(pb, eb, bytes));

    /* A gets a new size and a new font. B is still current. */
    struct font_instance *other = font_instance_acquire(
        font_instance_size_pt(test_font()) + 2,
        font_instance_scale_120(test_font()));
    const int rw = w + 7, rh = h + 5;
    struct vector2i rsize = { .x = rw, .y = rh };
    size_t resized_bytes = (size_t)rw * rh * sizeof(uint32_t);
    uint32_t *resized = calloc(1, resized_bytes);
    uint32_t *resized_expected = malloc(resized_bytes);
    assert(other && resized && resized_expected);
    (void)draw_grid(test_oracle(), resized_expected, rsize, NULL, false, &fa, other);
    (void)draw_grid(r, resized, rsize, &ha, true, &fa, other);
    assert(!memcmp(resized, resized_expected, resized_bytes));
    res = draw_grid(r, pb, size, &hb, false, &fb, test_font());
    assert(!res.damage.any);
    assert(!memcmp(pb, eb, bytes));
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

/* ---- Text --------------------------------------------------------------- */

/* A glyph in one style does not replace the glyph of another style. */
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

    struct renderer *r = test_renderer();
    assert(r);
    for (int i = 0; i < 3; i++) {
        test_renderer_paint(r, actual, w, h, plain, true);
        assert(!memcmp(expected, actual, bytes));
        test_renderer_paint(r, actual, w, h, bold, true);
    }
    test_renderer_destroy(r);
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

/* Many one-off frames through a cached renderer do not change known text. */
static void cache_churn_test(void)
{
    struct wsap_vtcore *t = tt_new(40, 2), *noise = tt_new(40, 2);
    int w = 40 * CELL_W, h = 2 * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *expected = malloc(bytes), *actual = malloc(bytes);
    assert(t && noise && expected && actual);
    feed(t, "\033[?25l->===!= سلام 日本語\r\n\033[3mitalic -> !=\033[0m");
    test_render_paint_full(expected, w, h, true, t);
    struct renderer *r = test_renderer();
    assert(r);
    struct terminal_frame frame;
    test_frame_init(&frame, t, true);
    (void)draw_grid(r, actual, size, NULL, false, &frame, test_font());
    (void)draw_grid(r, actual, size, NULL, false, &frame, test_font());
    tcheck_same("cache churn, warm", actual, expected, size);
    uint32_t seed = 1;
    for (int i = 0; i < 256; i++) {
        char text[40];
        for (size_t j = 0; j < sizeof(text); j++) {
            seed = seed * 1664525u + 1013904223u;
            text[j] = (char)('!' + (seed >> 16) % 94);
        }
        feed(noise, "\033[H");
        char pen[32];
        snprintf(pen, sizeof(pen), "\033[38;5;%um", (seed >> 8) % 256);
        feed(noise, pen);
        tt_write(noise, text, sizeof(text));
        test_render_paint_full(actual, w, h, true, noise);
        test_frame_init(&frame, noise, true);
        (void)draw_grid(r, actual, size, NULL, false, &frame, test_font());
    }
    test_render_paint_full(actual, w, h, true, t);
    tcheck_same("cache churn, oracle", actual, expected, size);
    test_frame_init(&frame, t, true);
    (void)draw_grid(r, actual, size, NULL, false, &frame, test_font());
    tcheck_same("cache churn, cached", actual, expected, size);
    glyph_cache_rebuild(test_font());
    test_render_paint_full(actual, w, h, true, t);
    tcheck_same("cache churn, rebuilt oracle", actual, expected, size);
    test_frame_init(&frame, t, true);
    (void)draw_grid(r, actual, size, NULL, false, &frame, test_font());
    tcheck_same("cache churn, rebuilt", actual, expected, size);
    renderer_destroy(r);
    free(expected); free(actual); wsap_vtcore_free(t); wsap_vtcore_free(noise);
}

/* ---- Blend accuracy ----------------------------------------------------- */

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
    /* A faint underline: full coverage, halved. */
    feed(t, "\033[?25l\033[2;4m \033[0m");
    test_render_paint_full(px, w, h, true, t);
    int lines = 0;
    for (int y = 0; y < h; y++) {
        uint32_t uln = px[(size_t)y * w];
        if (uln == DEFAULT_BG)
            continue;
        lines++;
        for (int x = 0; x < CELL_W; x++)
            assert(px[(size_t)y * w + x] == uln);
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
    }
    assert(lines > 0);
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
    test_renderer_paint(r, incr, w, h, t, true);
    feed(t, colors_b);
    test_renderer_paint(r, incr, w, h, t, true);
    memcpy(incr_b, incr, bytes);

    feed(t, colors_a);
    test_renderer_paint(r, incr, w, h, t, true);
    test_render_paint_full(full, w, h, true, t);
    feed(t, colors_b);
    test_render_paint_full(full, w, h, true, t);

    glyph_cache_rebuild(test_font());
    test_render_paint_full(fresh, w, h, true, t);
    assert(memcmp(at_a, fresh, bytes) != 0);
    assert(!memcmp(full, fresh, bytes));
    assert(!memcmp(incr_b, fresh, bytes));
    test_renderer_paint(r, incr, w, h, t, true);
    assert(!memcmp(incr, fresh, bytes));

    test_renderer_destroy(r);
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
    test_renderer_paint(r, pixels, w, h, t, true);
    assert(pixels[0] == saved_bg);

    struct wsap_vtcore_theme light = default_theme();
    light.fg = 0xFF101010u;
    light.bg = 0xFFF0F0F0u;
    light.ansi[4] = 0xFF0000C0u;
    wsap_vtcore_set_theme(t, &light);
    test_renderer_paint(r, pixels, w, h, t, true);
    test_render_paint_full(expected, w, h, true, t);
    assert(!memcmp(pixels, expected, bytes));
    assert(pixels[0] == 0xFFF0F0F0u);
    assert(pixels[CELL_W] == 0xFF0000C0u);
    assert(pixels[3 * CELL_W] == 0xFF040506u);

    test_renderer_destroy(r);
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
    test_renderer_paint(r, pixels, w, h, t, true);
    feed(t, "\033]12;#ff0000\007");
    test_renderer_paint(r, pixels, w, h, t, true);
    test_render_paint_full(expected, w, h, true, t);
    assert(!memcmp(pixels, expected, bytes));

    feed(t, "\033[?25l\033]10;#00ff00\007\033]11;#0000ff\007\033[0m\033[2J\033[HA");
    test_renderer_paint(r, pixels, w, h, t, true);
    struct glyph glyph = {0};
    assert(font_instance_render_glyph(test_font(), 'A', 0, &glyph));
    assert(!glyph.has_color && glyph.x_offset == 0 && glyph.width == CELL_W);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < CELL_W; x++) {
            double a = corrected_cov(0xff00ff00, 0xff0000ff,
                                     glyph.alpha[y][x] / 255.0);
            a = lround(a * 255) / 255.0;
            int green = (int)lround(lin_to_srgb(a) * 255);
            int blue = (int)lround(lin_to_srgb(1 - a) * 255);
            uint32_t got = pixels[(size_t)y * w + x];
            assert(((got >> 16) & 255) == 0);
            assert(abs((int)((got >> 8) & 255) - green) <= 3);
            assert(abs((int)(got & 255) - blue) <= 3);
        }
    }
    glyph_release(&glyph);
    test_renderer_destroy(r);
    free(pixels);
    free(expected);
    wsap_vtcore_free(t);
    glyph_cache_rebuild(test_font());
}

/*
 * A cell with explicit colors that equal the theme colors must look like a
 * cell with the theme colors. Cells in the theme colors use the default
 * pair tiles. Cells in explicit colors go through the cell cache or the
 * direct compose. Some of these glyphs draw into the next cell: italic
 * tips, an icon, a circle, and LCD fringes.
 */
static void explicit_theme_colors_test(enum font_aa_mode aa)
{
    enum { COLS = 96, ROWS = 5 };
    char ascii[95];
    for (int i = 0; i < 94; i++)
        ascii[i] = (char)('!' + i);
    ascii[94] = 0;
    char text[1024];
    snprintf(text, sizeof(text),
             "%s\r\n\033[1m%s\033[22m\r\n\033[3m%s\r\nWAWAVATAYA7A%%A\033[23m\r\n"
             "\xef\x84\x95" "A \xef\x84\x95" " B\xe2\x97\x8f" "C\xe2\x97\x8f\xe2\x97\x8f" "D",
             ascii, ascii, ascii);

    struct wsap_vtcore_theme theme = default_theme();
    struct wsap_vtcore_theme other = theme;
    other.fg = 0xFF305070u;
    other.bg = 0xFFD0C0B0u;
    char pen[96];
    snprintf(pen, sizeof(pen), "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um\033[2J\033[H",
             (theme.fg >> 16) & 255, (theme.fg >> 8) & 255, theme.fg & 255,
             (theme.bg >> 16) & 255, (theme.bg >> 8) & 255, theme.bg & 255);

    struct wsap_vtcore *themed = tt_new(COLS, ROWS), *explicit = tt_new(COLS, ROWS);
    assert(themed && explicit);
    wsap_vtcore_set_theme(explicit, &other);
    feed(themed, "\033[?25l");
    feed(themed, text);
    feed(explicit, "\033[?25l");
    feed(explicit, pen);
    feed(explicit, text);

    font_set_aa_mode(aa);
    int w = COLS * CELL_W, h = ROWS * CELL_H;
    struct vector2i size = { .x = w, .y = h };
    size_t bytes = (size_t)w * h * sizeof(uint32_t);
    uint32_t *want = malloc(bytes), *got = malloc(bytes);
    assert(want && got);
    test_render_paint_full(want, w, h, false, explicit);
    test_render_paint_full(got, w, h, false, themed);
    for (int i = 0; i < w * h; i++) {
        if (got[i] == want[i])
            continue;
        fprintf(stderr, "AA mode %d: cell %d,%d pixel %d,%d is %08x, explicit colors give %08x\n",
                aa, i % w / CELL_W, i / w / CELL_H, i % w % CELL_W, i / w % CELL_H,
                got[i], want[i]);
        break;
    }
    assert(!memcmp(want, got, bytes));

    /* A cached renderer: a cold compose, an admission, and a cache hit. */
    struct renderer *r = test_renderer();
    assert(r);
    struct terminal_frame frame;
    for (int i = 0; i < 3; i++) {
        test_frame_init(&frame, explicit, false);
        (void)draw_grid(r, got, size, NULL, false, &frame, test_font());
        tcheck_same("explicit colors, cached", got, want, size);
        test_frame_init(&frame, themed, false);
        (void)draw_grid(r, got, size, NULL, false, &frame, test_font());
        tcheck_same("theme colors, cached", got, want, size);
    }
    renderer_destroy(r);
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    free(want);
    free(got);
    wsap_vtcore_free(themed);
    wsap_vtcore_free(explicit);
}

/* ---- Scenes ------------------------------------------------------------- */

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
    struct vector2i size = { .x = 8, .y = 6 };
    struct framebuffer_history history = {0};
    struct ui_damage_tracker tracker = {0};
    struct ui_damage damage;
    ui_damage_track(&tracker, &scene, &damage);
    struct renderer *r = test_renderer();
    assert(r);
    (void)draw_scene(r, px, size, &history, true, &scene, &damage, 0);
    assert(px[1 * 8 + 2] == src[0]);
    assert(px[3 * 8 + 4] == src[2 * 4 + 2]);
    assert(px[1 * 8 + 5] == 0);
    assert(px[4 * 8 + 2] == 0);

    uint32_t before = px[1 * 8 + 2];
    src[0] = 0xFFABCDEFu;
    img.gen = 2;
    ui_scene_init(&scene);
    assert(ui_scene_push_clip(&scene, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 5, .y = 10 } }));
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 2, .y = 1 }, &img));
    assert(ui_scene_pop_clip(&scene));
    ui_damage_track(&tracker, &scene, &damage);
    struct render_result res = draw_scene(r, px, size, &history, false, &scene, &damage, 0);
    assert(px[1 * 8 + 2] == src[0]);
    assert(px[1 * 8 + 2] != before);
    assert(tdamage_has(&res.damage, (struct vector2i){ .x = 2, .y = 1 }));
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
    unsigned blended = blend_channel(255, 0, 128);
    assert(blended > 180 && blended < 195);
    uint32_t px = blend_px(0xFFFFFFFFu, 0xFF000000u, 128);
    assert((px & 0xFF) == 128);
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

/* ---- render-bench ------------------------------------------------------- */

/* The renderer of the full-paint cases: the defaults, and no history. */
static struct renderer *bench_full_renderer;

static void bench_paint_full(uint32_t *px, int w, int h, bool cursor, struct wsap_vtcore *t)
{
    struct terminal_frame frame;
    test_frame_init(&frame, t, cursor);
    (void)test_draw_once(bench_full_renderer, px, w, h, &frame);
}

/* One frame on a persistent canvas, with no UI. */
static void bench_draw(struct renderer *r, struct framebuffer_history *history, uint32_t *px,
                       int w, int h, const struct terminal_frame *grid, bool fresh)
{
    struct framebuffer target = {
        .pixels = px, .size = { .x = w, .y = h }, .fresh = fresh, .history = history,
    };
    struct render_frame frame = test_grid_frame(grid, w, h);
    frame.ui_damage = &no_ui_damage;
    (void)render_draw(r, &target, &frame);
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
    struct framebuffer_history history = {0};
    double total = 0, worst = 0;
    for (int f = 0; f < frames + 10; f++) {
        tt_write(t, chunk, flood_chunk(kind, &seed, chunk, sizeof(chunk)));
        assert(wsap_vtcore_snapshot_update(snap, t));
        struct terminal_frame frame;
        terminal_frame_init(&frame, snap, false);
        double begin = now();
        bench_draw(r, &history, px, w, h, &frame, f == 0);
        double dt = now() - begin;
        if (f >= 10) {
            total += dt;
            if (dt > worst) worst = dt;
        }
    }
    printf("flood-%-10s %.3f ms/frame worst=%.3f ms\n", name, total * 1000 / frames,
           worst * 1000);
    framebuffer_history_release(&history);
    wsap_vtcore_snapshot_free(snap);
    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
}

static bool bench_want(const char *only, const char *name)
{
    return !only || !strcmp(only, name);
}

enum bench_kind { BENCH_PLAIN, BENCH_OVERLAY, BENCH_STALE };

/* One bench case. The step function changes the terminal before each frame. */
struct bench_case {
    const char *name;
    const char *setup;
    bool code_screen;
    enum bench_kind kind;
    void (*step)(struct wsap_vtcore *t, int frame, uint32_t *seed);
};

static void bench_step_typing(struct wsap_vtcore *t, int f, uint32_t *seed)
{
    (void)seed;
    char c = (char)('a' + f % 26);
    tt_write(t, &c, 1);
}

static void bench_step_idle(struct wsap_vtcore *t, int f, uint32_t *seed)
{
    (void)t; (void)f; (void)seed;
}

static void bench_step_region(struct wsap_vtcore *t, int f, uint32_t *seed)
{
    (void)seed;
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "    region line %06d: if (x != y && a <= b) return f(x);\r\n", f);
    tt_write(t, line, (size_t)n);
}

static void bench_step_scroll(struct wsap_vtcore *t, int f, uint32_t *seed)
{
    (void)seed;
    char line[128];
    int n = snprintf(line, sizeof(line),
                     "A new scrolled line in the terminal grid %06d abcdefghijklmnopqrstuvwxyz\r\n", f);
    tt_write(t, line, (size_t)n);
}

static void bench_step_italic(struct wsap_vtcore *t, int f, uint32_t *seed)
{
    (void)f;
    static char chunk[4096];
    tt_write(t, chunk, flood_chunk(FLOOD_CODE, seed, chunk, sizeof(chunk)));
}

static void bench_step_cjk(struct wsap_vtcore *t, int f, uint32_t *seed)
{
    (void)f;
    static const char *const tokens[] = {
        "日本語のテキスト", "漢字", "한국어", "🙂", "🚀", "👍", "🎉🎉", "fn main() ", "x = y + 1; ",
        "ascii text ", "中文字符", "🦀 rust ",
    };
    static char chunk[4096];
    size_t n = 0;
    while (n + 64 < sizeof(chunk)) {
        *seed ^= *seed << 13;
        *seed ^= *seed >> 17;
        *seed ^= *seed << 5;
        const char *tok = tokens[*seed % (sizeof(tokens) / sizeof(tokens[0]))];
        size_t len = strlen(tok);
        memcpy(chunk + n, tok, len);
        n += len;
        if (*seed % 9 == 0) {
            memcpy(chunk + n, "\r\n", 2);
            n += 2;
        }
    }
    tt_write(t, chunk, n);
}

static void bench_run(const struct bench_case *c)
{
    int cols = 160, rows = 50;
    int frames = getenv("BENCH_FRAMES") ? atoi(getenv("BENCH_FRAMES")) : 2000;
    int warmup = 20;
    struct wsap_vtcore *t = tt_new(cols, rows);
    struct renderer *r = test_renderer();
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    int w = cols * CELL_W, h = rows * CELL_H;
    uint32_t *px = malloc((size_t)w * h * sizeof(*px));
    assert(t && r && snap && px);
    feed(t, "\033[?25l");
    uint32_t seed = 0x9e3779b9u;
    if (c->code_screen) {
        static char chunk[8192];
        tt_write(t, chunk, flood_chunk(FLOOD_CODE, &seed, chunk, sizeof(chunk)));
        feed(t, "\033[H");
    }
    if (c->setup)
        feed(t, c->setup);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    if (c->kind == BENCH_OVERLAY) {
        /* A thin translucent rounded bar at the right edge, like the scrollbar thumb. */
        bool ok = ui_scene_rounded_rect(&scene,
            (struct rect2i){ .pos = { .x = w - 6, .y = h / 4 },
                             .size = { .x = 4, .y = h / 3 } }, 2, 0x80A6ADC8u);
        assert(ok);
        (void)ok;
    }

    struct framebuffer_history history = {0};
    double total = 0, worst = 0;
    for (int f = 0; f < frames + warmup; f++) {
        c->step(t, f, &seed);
        assert(wsap_vtcore_snapshot_update(snap, t));
        struct terminal_frame grid;
        terminal_frame_init(&grid, snap, false);
        /* fresh-every-frame is what the window does in this milestone. */
        struct framebuffer target = {
            .pixels = px,
            .size = { .x = w, .y = h },
            .fresh = c->kind == BENCH_STALE || f == 0,
            .history = &history,
        };
        struct render_frame frame = test_grid_frame(&grid, w, h);
        frame.ui_damage = &no_ui_damage;
        if (c->kind == BENCH_OVERLAY) {
            frame.ui = &scene;
            /* The overlay changes only in the first frame. NULL means every UI pixel changed. */
            frame.ui_damage = f ? &no_ui_damage : NULL;
        }
        double begin = now();
        (void)render_draw(r, &target, &frame);
        double dt = now() - begin;
        if (f >= warmup) {
            total += dt;
            if (dt > worst) worst = dt;
        }
    }
    printf("%-18s %.4f ms/frame worst=%.3f ms\n", c->name, total * 1000 / frames,
           worst * 1000);
    ui_scene_destroy(&scene);
    framebuffer_history_release(&history);
    wsap_vtcore_snapshot_free(snap);
    renderer_destroy(r);
    free(px);
    wsap_vtcore_free(t);
}

static void benchmark(void)
{
    bench_full_renderer = test_renderer();
    assert(bench_full_renderer);
    const char *flood = getenv("FLOOD");
    const char *only = getenv("BENCH_CASE");
    for (unsigned i = 0; i < sizeof(flood_kinds) / sizeof(flood_kinds[0]); i++) {
        char full[32];
        snprintf(full, sizeof(full), "flood-%s", flood_kinds[i].name);
        if (flood ? !strcmp(flood, flood_kinds[i].name) : bench_want(only, full))
            flood_benchmark(flood_kinds[i].kind, flood_kinds[i].name);
    }
    if (flood) {
        renderer_destroy(bench_full_renderer);
        return;
    }

    static const struct bench_case bench_cases[] = {
        { "typing", NULL, true, BENCH_PLAIN, bench_step_typing },
        { "idle", NULL, true, BENCH_PLAIN, bench_step_idle },
        { "scroll-region", "\033[5;45r\033[45;1H", true, BENCH_PLAIN, bench_step_region },
        { "overlay-scroll", "\033[50;1H", true, BENCH_OVERLAY, bench_step_scroll },
        { "italic", "\033[3m", false, BENCH_PLAIN, bench_step_italic },
        { "cjk-emoji", NULL, false, BENCH_PLAIN, bench_step_cjk },
        { "fresh-every-frame", NULL, true, BENCH_STALE, bench_step_typing },
    };
    for (unsigned i = 0; i < sizeof(bench_cases) / sizeof(bench_cases[0]); i++)
        if (bench_want(only, bench_cases[i].name))
            bench_run(&bench_cases[i]);

    const char *cases[] = {"aaaaaaaaaaaaaaaaaaaaaaaa", "->===!=<=->===!=<=", "abc日本語->سلام🙂xyz"};
    const char *names[] = {"plain", "ligatures", "mixed"};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (!bench_want(only, names[c]))
            continue;
        struct wsap_vtcore *t = tt_new(120, 20);
        feed(t, "\033[?25l");
        for (int i = 0; i < 150; i++) feed(t, cases[c]);
        int w = 120 * CELL_W, h = 20 * CELL_H;
        uint32_t *pixels = malloc((size_t)w * h * 4);
        assert(t && pixels);
        bench_paint_full(pixels, w, h, true, t);
        double start = now();
        for (int i = 0; i < 10; i++) bench_paint_full(pixels, w, h, true, t);
        printf("%-10s %.3f ms/frame pixels=%016llx\n", names[c],
               (now() - start) * 100, (unsigned long long)hash(pixels, (size_t)w * h));
        free(pixels); wsap_vtcore_free(t);
    }
    if (bench_want(only, "random")) {
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
            bench_paint_full(pixels, w, h, true, t);
            checksum ^= hash(pixels, (size_t)w * h);
        }
        printf("random     %.3f ms/frame pixels=%016llx\n",
               (now() - start) * 10, (unsigned long long)checksum);
        free(pixels); wsap_vtcore_free(t);

    }

    if (bench_want(only, "scroll")) {
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
        bench_draw(sr, &history, spx, sw, sh, &frame, true);
        double shift_time = 0, full_time = 0;
        for (int n = 0; n < 100; n++) {
            feed(st, "A new scrolled line in the terminal grid 0123456789 abcdefghijklmnopqrstuvwxyz\r\n");
            assert(wsap_vtcore_snapshot_update(snap, st));
            terminal_frame_init(&frame, snap, false);
            double begin = now();
            bench_draw(sr, &history, spx, sw, sh, &frame, false);
            shift_time += now() - begin;
            begin = now();
            (void)test_draw_once(bench_full_renderer, full, sw, sh, &frame);
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
    renderer_destroy(bench_full_renderer);
    bench_full_renderer = NULL;
}

/* The tests that run again with a font at 1.5 times the scale. */
static void scaled_font_pass(void)
{
    test_font_override = font_instance_acquire(
        font_instance_size_pt(font_instance_default()), 180);
    assert(test_font_override);
    explicit_theme_colors_test(FONT_AA_GRAYSCALE);
    explicit_theme_colors_test(FONT_AA_RGB);
    clipped_target_test(); style_cache_test();
    scroll_pixels_test();
    suffix_ligature_test(); run_drift_test(); cache_churn_test();
    linear_blend_test();
    font_instance_unref(test_font_override);
    test_font_override = NULL;
}

int main(int argc, char **argv)
{
    uninitialized_srgb_luts_test();
    render_init();
    if (argc > 1 && !strcmp(argv[1], "--bench")) benchmark();
    else {
        /* The pixel checks expect grayscale glyphs. Set them here, so the
         * host fontconfig settings do not change the results. */
        font_set_aa_mode(FONT_AA_GRAYSCALE);
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
        explicit_theme_colors_test(FONT_AA_GRAYSCALE);
        explicit_theme_colors_test(FONT_AA_RGB);
        frame_colors_test(2);
        frame_colors_test(6);
        scroll_pixels_test();
        surface_histories_are_independent_test();
        empty_frame_clears_and_invalidates_grid_test();
        suffix_ligature_test(); run_drift_test(); cache_churn_test();
        linear_blend_test();
        color_glyph_blend_test();
        oversized_frame_test();
        popup_cleared_target_test();
        overlay_grid_repaint_test();
        scaled_font_pass();
        puts("PASS: renderer pixel regressions");
    }
    test_frame_helpers_fini();
    render_shutdown();
}
