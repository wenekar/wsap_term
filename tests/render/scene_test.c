#define _POSIX_C_SOURCE 200809L
/*
 * Tests of the scene painter of the new renderer. They go through
 * ui_scene_paint and ui_scene_paint_clipped only.
 *
 * The test supplies the glyph cache hooks and the sRGB tables with the stub
 * that the UI tests use. So the test also checks the link contract of the
 * painter.
 *
 * Cross-ISA parity: the painter resolves its ISA once. So the test runs
 * its own binary twice, with WSAP_RENDER_ISA=scalar and with the best
 * level, and compares the hash of the pixels.
 */
#include "../ui/stubs/scene_paint_deps.c"

#include "../paint/scene_text_contract.h"

#include "paint/blend.h"
#include "paint/scene.h"
#include "paint/shape_math.h"
#include "render/scene_paint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FONT ((struct font_instance *)&scene_paint_test_font)

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            abort();                                                         \
        }                                                                    \
    } while (0)

#define R(x_, y_, w_, h_) \
    ((struct rect2i){ .pos = { .x = (x_), .y = (y_) }, .size = { .x = (w_), .y = (h_) } })

static void fill(uint32_t *px, int n, uint32_t v)
{
    for (int i = 0; i < n; i++)
        px[i] = v;
}

/* ---- Images ---------------------------------------------------------- */

static void test_image(void)
{
    enum { W = 8, H = 6, IW = 5, IH = 4 };
    uint32_t src[IW * IH];
    for (int i = 0; i < IW * IH; i++)
        src[i] = 0x40000000u + (uint32_t)i * 0x010203u;
    struct ui_image image = { .pixels = src, .size = { .x = IW, .y = IH } };

    /* Partly off the canvas on the top-left. */
    struct vector2i pos = { .x = -2, .y = -1 };
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_image(&s, pos, &image));

    uint32_t out[W * H];
    fill(out, W * H, 0xFF778899u);
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int ix = x - pos.x, iy = y - pos.y;
            bool in = ix >= 0 && ix < IW && iy >= 0 && iy < IH;
            CHECK(out[y * W + x] ==
                  (in ? (src[iy * IW + ix] | 0xFF000000u) : 0xFF778899u));
        }

    /* Over base 0 the image still lands opaque. */
    fill(out, W * H, 0);
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    CHECK(out[0] == (src[1 * IW + 2] | 0xFF000000u));

    /* A clip rect cuts the copy. */
    struct rect2i clip = R(1, 1, 2, 2);
    fill(out, W * H, 0xFF778899u);
    CHECK(ui_scene_paint_clipped(TEST_FONT, out, W, H, &s, clip));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int ix = x - pos.x, iy = y - pos.y;
            bool in = x >= 1 && x < 3 && y >= 1 && y < 3;
            CHECK(out[y * W + x] ==
                  (in ? (src[iy * IW + ix] | 0xFF000000u) : 0xFF778899u));
        }
    ui_scene_destroy(&s);

    /* Off the right and bottom edges. */
    ui_scene_init(&s);
    CHECK(ui_scene_image(&s, (struct vector2i){ .x = W - 2, .y = H - 1 }, &image));
    fill(out, W * H, 0xFF778899u);
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    CHECK(out[(H - 1) * W + W - 2] == (src[0] | 0xFF000000u));
    CHECK(out[(H - 1) * W + W - 1] == (src[1] | 0xFF000000u));
    CHECK(out[(H - 2) * W + W - 1] == 0xFF778899u);
    ui_scene_destroy(&s);
}

/* ---- Rects ----------------------------------------------------------- */

static void test_translucent_rect(void)
{
    enum { W = 40, H = 5 };
    uint32_t bases[2] = { 0xFF336699u, 0x00000000u };
    for (int b = 0; b < 2; b++) {
        uint32_t out[W * H];
        /* A row of changing colors, so the kernels see many bg values. */
        for (int i = 0; i < W * H; i++)
            out[i] = bases[b] ? bases[b] + (uint32_t)(i % 7) * 0x030201u : 0;
        uint32_t before[W * H];
        memcpy(before, out, sizeof(out));

        struct ui_scene s = {0};
        ui_scene_init(&s);
        CHECK(ui_scene_rect(&s, R(3, 1, 33, 3), 0x80FF8040u));
        CHECK(ui_scene_blend_rect(&s, R(0, 0, W, 1), 0xFFFFFFFFu, 100));
        CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t want = before[y * W + x];
                if (x >= 3 && x < 36 && y >= 1 && y < 4)
                    want = blend_px(0x80FF8040u, want, 0x80);
                if (y == 0)
                    want = blend_px(0xFFFFFFFFu, want, 100);
                CHECK(out[y * W + x] == want);
            }
        ui_scene_destroy(&s);
    }
}

/* ---- Shapes ---------------------------------------------------------- */

static void test_pill(void)
{
    enum { W = 40, H = 20, X = 5, Y = 4, PW = 30, PH = 12 };
    uint32_t out[W * H];
    fill(out, W * H, 0xFF000000u);
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_rounded_rect(&s, R(X, Y, PW, PH), PH / 2, 0xFFFFFFFFu));
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    ui_scene_destroy(&s);

    bool partial = false;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t px = out[y * W + x];
            if (x >= X && x < X + PW)
                CHECK(px == out[y * W + (2 * X + PW - 1 - x)]);
            if (y >= Y && y < Y + PH)
                CHECK(px == out[(2 * Y + PH - 1 - y) * W + x]);
            int cov = x >= X && x < X + PW && y >= Y && y < Y + PH
                ? shape_rrect_cov((float)x, (float)y, X, Y, PW, PH, PH / 2) : 0;
            uint32_t want = cov == 0 ? 0xFF000000u
                : cov == 255 ? 0xFFFFFFFFu
                : blend_px(0xFFFFFFFFu, 0xFF000000u, (uint8_t)cov);
            CHECK(px == want);
            if (cov > 0 && cov < 255)
                partial = true;
        }
    CHECK(partial);
    CHECK(out[(Y + PH / 2) * W + X + PW / 2] == 0xFFFFFFFFu);
    CHECK(out[Y * W + X] == 0xFF000000u);
}

/* The straight edge of an even-height pill is full at the middle rows. */
static void test_pill_middle_edge(void)
{
    enum { W = 40, H = 20, X = 5, Y = 4, PW = 30, PH = 12 };
    uint32_t out[W * H];
    fill(out, W * H, 0xFF000000u);
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_rounded_rect(&s, R(X, Y, PW, PH), PH / 2, 0xFF3060C0u));
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    ui_scene_destroy(&s);
    for (int y = Y + PH / 2 - 1; y <= Y + PH / 2; y++) {
        CHECK(out[y * W + X] == 0xFF3060C0u);
        CHECK(out[y * W + X + PW - 1] == 0xFF3060C0u);
    }
}

static void test_rounded_rect_translucent(void)
{
    enum { W = 24, H = 24 };
    uint32_t out[W * H];
    fill(out, W * H, 0xFF204060u);
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_rounded_rect(&s, R(2, 2, 20, 20), 8, 0x80FFFFFFu));
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    ui_scene_destroy(&s);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            CHECK(out[y * W + x] == out[y * W + (W - 1 - x)]);
            CHECK(out[y * W + x] == out[(H - 1 - y) * W + x]);
        }
    CHECK(out[12 * W + 12] == blend_px(0x80FFFFFFu, 0xFF204060u, 0x80));
    CHECK(out[2 * W + 2] == 0xFF204060u);
}

static void test_circle(void)
{
    enum { W = 40, R_ = 10 };
    uint32_t out[W * W];
    fill(out, W * W, 0xFF000000u);
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_circle(&s, (struct vector2i){ .x = 20, .y = 20 }, R_,
                          0xFFFFFFFFu, 255));
    CHECK(ui_scene_paint(TEST_FONT, out, W, W, &s));
    ui_scene_destroy(&s);
    int lit = 0;
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            CHECK(out[y * W + x] == out[y * W + (W - 1 - x)]);
            CHECK(out[y * W + x] == out[(W - 1 - y) * W + x]);
            CHECK(out[y * W + x] == out[x * W + y]);
            lit += out[y * W + x] != 0xFF000000u;
        }
    CHECK(out[20 * W + 20] == 0xFFFFFFFFu);
    CHECK(out[5 * W + 5] == 0xFF000000u);
    CHECK(lit > 250 && lit < 400);
}

static void test_shadow(void)
{
    enum { W = 100, H = 80, BX = 30, BY = 25, BW = 40, BH = 30, BLUR = 10 };
    uint32_t out[W * H];
    fill(out, W * H, 0xFFFFFFFFu);
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_shadow(&s, R(BX, BY, BW, BH), 8, BLUR, 0xFF000000u));
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    ui_scene_destroy(&s);

    int ext = (BLUR * 3 + 1) / 2;
    int x0 = BX - ext, y0 = BY - ext, w = BW + 2 * ext, h = BH + 2 * ext;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            uint32_t a = out[y * W + x];
            CHECK(a == out[y * W + (2 * x0 + w - 1 - x)]);
            CHECK(a == out[(2 * y0 + h - 1 - y) * W + x]);
        }
    /* Outside the painted area nothing changes. */
    CHECK(out[(y0 - 1) * W + BX + 5] == 0xFFFFFFFFu);
    CHECK(out[(BY + 5) * W + x0 - 1] == 0xFFFFFFFFu);

    /* From the inside to the outside, the shadow fades. */
    int row = BY + BH / 2, col = BX + BW / 2;
    uint32_t last = 0;
    for (int x = col; x >= x0; x--) {
        uint32_t g = (out[row * W + x] >> 8) & 255;
        CHECK(g >= last);
        last = g;
    }
    last = 0;
    for (int y = row; y >= y0; y--) {
        uint32_t g = (out[y * W + col] >> 8) & 255;
        CHECK(g >= last);
        last = g;
    }
    CHECK(((out[row * W + col] >> 8) & 255) < 8);
    CHECK(((out[row * W + x0] >> 8) & 255) > 247);
}

/* ---- Text ------------------------------------------------------------ */

#define TG_W 4
#define TG_H 4
static uint8_t tg_alpha[TG_H][TG_W];
static uint8_t tg_subpx[TG_H][TG_W * 3];
static uint8_t *tg_alpha_rows[TG_H];
static uint8_t *tg_subpx_rows[TG_H];
static struct glyph tg_glyph[2];

static const struct glyph *tg_hook(uint32_t cp, uint16_t flags)
{
    (void)flags;
    return &tg_glyph[cp == 'S'];
}

static void tg_build(void)
{
    for (int y = 0; y < TG_H; y++) {
        for (int x = 0; x < TG_W; x++) {
            tg_alpha[y][x] = (uint8_t)(x * 70 + y * 5 + (x == 3 ? 12 : 0));
            tg_subpx[y][x * 3 + 0] = (uint8_t)(x * 80 + 7);
            tg_subpx[y][x * 3 + 1] = (uint8_t)(y * 60 + x * 10);
            tg_subpx[y][x * 3 + 2] = (uint8_t)(255 - x * 70 - y);
        }
        tg_alpha_rows[y] = tg_alpha[y];
        tg_subpx_rows[y] = tg_subpx[y];
    }
    tg_glyph[0] = (struct glyph){ .width = TG_W, .alpha = tg_alpha_rows };
    tg_glyph[1] = (struct glyph){ .width = TG_W, .subpx = tg_subpx_rows,
                                  .has_subpx_ink = true };
}

static void text_scene(struct ui_scene *s, const char *text, uint32_t color)
{
    ui_scene_init(s);
    struct ui_text_style st = { .advance = TG_W, .cell_h = TG_H };
    CHECK(ui_scene_text_run(s, R(2, 1, 8, TG_H), st, color, text, strlen(text)));
}

static void test_text_blend(void)
{
    scene_paint_glyph_hook = tg_hook;
    tg_build();
    enum { W = 16, H = 8 };
    uint32_t colors[2] = { 0xFFF0E0D0u, 0x80F0E0D0u };
    uint32_t bases[3] = { 0x00000000u, 0xFF203040u, 0x60102030u };
    for (int c = 0; c < 2; c++)
        for (int b = 0; b < 3; b++)
            for (int rep = 0; rep < 4; rep++) {
                /* The repeats make the pair LUTs build and hit. */
                uint32_t out[W * H];
                fill(out, W * H, bases[b]);
                struct ui_scene s = {0};
                text_scene(&s, "AS", colors[c]);
                CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
                ui_scene_destroy(&s);
                unsigned op = colors[c] >> 24;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        uint32_t want = bases[b];
                        int tx = x - 2, ty = y - 1;
                        if (tx >= 0 && tx < 2 * TG_W && ty >= 0 && ty < TG_H) {
                            int gi = tx / TG_W, gx = tx % TG_W;
                            if (gi == 0) {
                                unsigned a = tg_alpha[ty][gx];
                                a = op == 255 ? a : (a * op + 127) / 255;
                                want = blend_px(colors[c], want, (uint8_t)a);
                            } else {
                                unsigned v[3];
                                for (int k = 0; k < 3; k++) {
                                    v[k] = tg_subpx[ty][gx * 3 + k];
                                    v[k] = op == 255 ? v[k]
                                        : (v[k] * op + 127) / 255;
                                }
                                want = blend_subpx(colors[c], want, (uint8_t)v[0],
                                                   (uint8_t)v[1], (uint8_t)v[2]);
                            }
                        }
                        CHECK(out[y * W + x] == want);
                    }
            }
}

/* The alpha of the output over base 0 follows blend_px. */
static void test_text_alpha_over_base_zero(void)
{
    scene_paint_glyph_hook = tg_hook;
    tg_build();
    enum { W = 16, H = 8 };
    uint32_t out[W * H];
    fill(out, W * H, 0);
    struct ui_scene s = {0};
    text_scene(&s, "A", 0xFFFFFFFFu);
    CHECK(ui_scene_paint(TEST_FONT, out, W, H, &s));
    ui_scene_destroy(&s);
    for (int ty = 0; ty < TG_H; ty++)
        for (int tx = 0; tx < TG_W; tx++) {
            unsigned a = tg_alpha[ty][tx];
            uint32_t px = out[(1 + ty) * W + 2 + tx];
            CHECK((px >> 24) == (blend_px(0xFFFFFFFFu, 0, (uint8_t)a) >> 24));
        }
}

static uint32_t contract_target[SCENE_CELL_H][SCENE_WIDTH];

static bool contract_paint(void *ctx, const struct ui_scene *scene)
{
    (void)ctx;
    memset(contract_target, 0, sizeof(contract_target));
    return ui_scene_paint(TEST_FONT, &contract_target[0][0], SCENE_WIDTH,
                          SCENE_CELL_H, scene);
}

static bool contract_inked(void *ctx, int x, int y)
{
    (void)ctx;
    return (contract_target[y][x] & 0x00FFFFFFu) != 0;
}

/* Text runs: wide cells, replacement of bad bytes, and an empty scene. */
static void test_text_contract(void)
{
    scene_paint_glyph_hook = scene_contract_glyph;
    struct scene_backend b = {
        .name = "ui_scene_paint (new)",
        .paint = contract_paint,
        .inked = contract_inked,
    };
    scene_text_contract_check(&b);
}

/* ---- Clips ----------------------------------------------------------- */

static void test_failed_scene_is_not_painted(void)
{
    uint32_t pixels[4];
    fill(pixels, 4, 0xFF010203u);
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_push_clip(&s, R(0, 0, 2, 2)));
    CHECK(!ui_scene_is_valid(&s));
    CHECK(!ui_scene_paint(TEST_FONT, pixels, 2, 2, &s));
    for (int i = 0; i < 4; i++)
        CHECK(pixels[i] == 0xFF010203u);
    ui_scene_destroy(&s);
}

static void test_paint_clip(void)
{
    enum { W = 16, H = 8 };
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_rect(&s, R(0, 0, W, H), 0xFF112233u));
    CHECK(ui_scene_push_clip(&s, R(4, 2, 4, 4)));
    CHECK(ui_scene_rect(&s, R(2, 0, 8, 8), 0xFF445566u));
    CHECK(ui_scene_pop_clip(&s));

    uint32_t full[W * H], clipped[W * H];
    fill(full, W * H, 0xAAAAAAAAu);
    fill(clipped, W * H, 0xAAAAAAAAu);
    CHECK(ui_scene_paint(TEST_FONT, full, W, H, &s));
    struct rect2i clip = R(6, 1, 5, 5);
    CHECK(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s, clip));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bool in = x >= 6 && x < 11 && y >= 1 && y < 6;
            CHECK(clipped[y * W + x] == (in ? full[y * W + x] : 0xAAAAAAAAu));
        }
    /* An empty clip paints nothing. */
    fill(clipped, W * H, 0xAAAAAAAAu);
    CHECK(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s, (struct rect2i){0}));
    for (int i = 0; i < W * H; i++)
        CHECK(clipped[i] == 0xAAAAAAAAu);
    /* A clip as large as the canvas changes nothing. */
    fill(clipped, W * H, 0xAAAAAAAAu);
    CHECK(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s, R(0, 0, W, H)));
    CHECK(!memcmp(clipped, full, sizeof(full)));
    /* A clip that sticks out of the canvas is cut to the canvas. */
    fill(clipped, W * H, 0xAAAAAAAAu);
    CHECK(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s, R(-5, -5, 100, 100)));
    CHECK(!memcmp(clipped, full, sizeof(full)));
    ui_scene_destroy(&s);
}

static void test_rounded_rect_clipping_keeps_shape(void)
{
    uint32_t clipped[10 * 10] = {0};
    uint32_t reference[20 * 20] = {0};
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_rounded_rect(&s, R(-3, -3, 10, 10), 4, 0xFFFFFFFFu));
    CHECK(ui_scene_paint(TEST_FONT, clipped, 10, 10, &s));
    ui_scene_destroy(&s);
    ui_scene_init(&s);
    CHECK(ui_scene_rounded_rect(&s, R(2, 2, 10, 10), 4, 0xFFFFFFFFu));
    CHECK(ui_scene_paint(TEST_FONT, reference, 20, 20, &s));
    ui_scene_destroy(&s);
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++)
            CHECK(clipped[y * 10 + x] == reference[(y + 5) * 20 + x + 5]);
}

static void test_node_draws_under_clip(void)
{
    static int node;
    struct ui_scene s = {0};
    ui_scene_init(&s);
    struct rect2i clip = R(2, 2, 2, 2);
    CHECK(ui_scene_rect(&s, R(0, 0, 8, 8), 0xFF000000u));
    CHECK(ui_scene_push_clip(&s, clip));
    CHECK(ui_scene_begin_node(&s, &node, clip.pos));
    CHECK(ui_scene_rect(&s, R(0, 0, 4, 4), 0xFFFF0000u));
    CHECK(ui_scene_end_scope(&s));
    CHECK(ui_scene_pop_clip(&s));
    uint32_t pixels[64] = {0};
    CHECK(ui_scene_paint(TEST_FONT, pixels, 8, 8, &s));
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            CHECK(pixels[y * 8 + x] ==
                  (x >= 2 && x < 4 && y >= 2 && y < 4 ? 0xFFFF0000u : 0xFF000000u));
    ui_scene_destroy(&s);
}

/* ---- Cross-ISA parity ------------------------------------------------ */

#define PW_ 96
#define PH_ 64

/* One scene that uses every op. Returns an FNV-1a hash of the pixels. */
static uint64_t parity_hash(void)
{
    scene_paint_glyph_hook = tg_hook;
    tg_build();
    static uint32_t out[PW_ * PH_];
    static uint32_t img[6 * 6];
    for (int i = 0; i < 36; i++)
        img[i] = 0x90000000u + (uint32_t)i * 0x070503u;
    struct ui_image image = { .pixels = img, .size = { .x = 6, .y = 6 } };
    for (int i = 0; i < PW_ * PH_; i++) {
        unsigned v = (unsigned)i * 2654435761u;
        out[i] = (i < PW_ * 8) ? 0u : 0xFF000000u | (v >> 8);
    }
    struct ui_scene s = {0};
    ui_scene_init(&s);
    CHECK(ui_scene_rect(&s, R(2, 2, 60, 20), 0xC0FF8800u));
    CHECK(ui_scene_blend_rect(&s, R(10, 5, 70, 40), 0xFF2040C0u, 77));
    CHECK(ui_scene_shadow(&s, R(20, 20, 40, 24), 8, 12, 0xA0000000u));
    CHECK(ui_scene_rounded_rect(&s, R(15, 12, 50, 30), 9, 0xB0E0E0FFu));
    CHECK(ui_scene_rounded_rect(&s, R(30, 40, 60, 20), 10, 0xFF30A050u));
    CHECK(ui_scene_circle(&s, (struct vector2i){ .x = 70, .y = 20 }, 12,
                          0xFFFFFFFFu, 200));
    CHECK(ui_scene_push_clip(&s, R(5, 3, 80, 20)));
    struct ui_text_style st = { .advance = TG_W, .cell_h = TG_H };
    CHECK(ui_scene_text_run(&s, R(6, 4, 8, TG_H), st, 0xFFF0E0D0u, "ASAS", 4));
    CHECK(ui_scene_text_run(&s, R(6, 12, 8, TG_H), st, 0x90F0E0D0u, "SASA", 4));
    CHECK(ui_scene_pop_clip(&s));
    CHECK(ui_scene_image(&s, (struct vector2i){ .x = 88, .y = 58 }, &image));
    CHECK(ui_scene_paint(TEST_FONT, out, PW_, PH_, &s));
    ui_scene_destroy(&s);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < PW_ * PH_; i++)
        for (int k = 0; k < 4; k++)
            h = (h ^ ((out[i] >> (8 * k)) & 255)) * 1099511628211ull;
    return h;
}

static uint64_t child_hash(const char *self, const char *isa)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "WSAP_RENDER_ISA=%s \"%s\" --hash", isa, self);
    FILE *f = popen(cmd, "r");
    CHECK(f);
    unsigned long long h = 0;
    CHECK(fscanf(f, "%llx", &h) == 1);
    CHECK(pclose(f) == 0);
    return h;
}

static void test_cross_isa(const char *self)
{
    uint64_t scalar = child_hash(self, "scalar");
    uint64_t best = child_hash(self, "auto");
    CHECK(scalar == best);
    CHECK(scalar != 0);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--hash")) {
        printf("%llx\n", (unsigned long long)parity_hash());
        return 0;
    }
    test_image();
    test_translucent_rect();
    test_pill();
    test_pill_middle_edge();
    test_rounded_rect_translucent();
    test_circle();
    test_shadow();
    test_text_blend();
    test_text_alpha_over_base_zero();
    test_text_contract();
    test_failed_scene_is_not_painted();
    test_paint_clip();
    test_rounded_rect_clipping_keeps_shape();
    test_node_draws_under_clip();
    (void)parity_hash();
    test_cross_isa(argv[0]);
    puts("PASS: scene painter (new renderer)");
    return 0;
}
