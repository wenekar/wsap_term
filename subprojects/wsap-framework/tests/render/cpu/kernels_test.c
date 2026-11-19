#define _DEFAULT_SOURCE
#include "render/cpu/kernels.h"
#include "render/cpu/isa.h"
#include "paint/blend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/* ---- Levels ---------------------------------------------------------- */

static int levels[4];
static int level_count;

static void find_levels(void)
{
    static const int all[] = {
        RENDER_ISA_SCALAR, RENDER_ISA_AVX2, RENDER_ISA_AVX512, RENDER_ISA_NEON,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (render_isa_resolve(all[i]) == all[i])
            levels[level_count++] = all[i];
}

/* ---- Random numbers -------------------------------------------------- */

static uint64_t rng_state = 0x5EED5EED12345678ull;

static uint64_t rnd(void)
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint32_t rnd32(void)
{
    return (uint32_t)rnd();
}

static int rnd_below(int n)
{
    return (int)(rnd() % (uint64_t)n);
}

/* ---- Reference formulas (paint/blend.h and render.c) ----------------- */

/* The color glyph formula of src/render/render.c blend_premul_glyph_px(). */
static uint32_t ref_premul(uint32_t premul, uint32_t bg, bool faint)
{
    unsigned a = premul >> 24;
    if (!a) return bg;
    unsigned coverage = faint ? (a + 1) / 2 : a;
    uint32_t result = 0xFF000000;
    for (int shift = 0; shift <= 16; shift += 8) {
        unsigned channel = (((premul >> shift) & 255) * 255 + a / 2) / a;
        if (channel > 255) channel = 255;
        result |= blend_channel(channel, (bg >> shift) & 255, coverage) << shift;
    }
    return result;
}

/* ---- Guarded memory -------------------------------------------------- */

/* Usable bytes [mem, mem + len). A PROT_NONE page follows mem + len. */
struct guarded {
    uint8_t *mem;
    size_t len;
};

static struct guarded guarded_new(size_t len)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t body = (len + page - 1) / page * page;
    uint8_t *p = mmap(NULL, body + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "mmap of %zu bytes", body + page);
    CHECK(!mprotect(p + body, page, PROT_NONE), "mprotect");
    return (struct guarded){ .mem = p + body - len, .len = len };
}

#define DST_WORDS 12288
#define GUARD_WORDS 40
#define IN_BYTES (DST_WORDS * 4)

static struct guarded dst_mem, cov_mem, src_mem;
static uint32_t snap[DST_WORDS];

/*
 * The test writes guard words around dst and checks them after each run.
 * at_end puts the last pixel next to the guard page, so a write or read past
 * the end faults.
 */
static uint32_t *win_lo, *win_hi;

static uint32_t *dst_place(size_t words, bool at_end)
{
    uint32_t *base = (uint32_t *)(void *)dst_mem.mem;
    size_t total = dst_mem.len / 4;
    CHECK(words + 2 * GUARD_WORDS + 16 <= total, "case too large: %zu", words);
    uint32_t *dst = at_end ? base + total - words
                           : base + GUARD_WORDS + (size_t)rnd_below(16);
    win_lo = dst - GUARD_WORDS;
    win_hi = dst + words + GUARD_WORDS;
    if (win_hi > base + total)
        win_hi = base + total;
    for (uint32_t *p = win_lo; p < win_hi; p++)
        *p = rnd32();
    return dst;
}

static void dst_snapshot(void)
{
    memcpy(snap, win_lo, (size_t)(win_hi - win_lo) * 4);
}

/* Check that no word in the window outside the given rows changed. */
static void dst_check_guards(const char *what, int isa, const uint32_t *dst, size_t stride,
                             int cw, int ch)
{
    for (const uint32_t *p = win_lo; p < win_hi; p++) {
        ptrdiff_t off = p - dst;
        bool inside = false;
        if (off >= 0 && cw > 0 && stride > 0) {
            size_t y = (size_t)off / stride, x = (size_t)off % stride;
            inside = y < (size_t)ch && x < (size_t)cw;
        }
        if (!inside)
            CHECK(*p == snap[p - win_lo], "%s [%s]: word at offset %td changed",
                  what, render_isa_name(isa), off);
    }
}

static const uint8_t *in_place(struct guarded *g, const void *src, size_t bytes, bool at_end,
                               size_t align)
{
    size_t max_off = g->len - bytes;
    CHECK(bytes + 128 <= g->len, "input too large: %zu", bytes);
    size_t off = at_end ? max_off : (64 + (size_t)rnd_below(64)) / align * align;
    uint8_t *p = g->mem + off;
    if (bytes)
        memcpy(p, src, bytes);
    return p;
}

/* ---- Corpus ---------------------------------------------------------- */

enum { COV_ZERO, COV_FULL, COV_RANDOM, COV_RAMP, COV_MIXED, COV_RUNS, COV_EDGE, COV_KINDS };

static void gen_cov(uint8_t *c, int len, int kind)
{
    int run = 0;
    uint8_t val = 0;
    for (int i = 0; i < len; i++) {
        switch (kind) {
        case COV_ZERO: c[i] = 0; break;
        case COV_FULL: c[i] = 255; break;
        case COV_RANDOM: c[i] = (uint8_t)rnd(); break;
        case COV_RAMP: c[i] = (uint8_t)(len > 1 ? i * 255 / (len - 1) : 128); break;
        case COV_MIXED: {
            int r = rnd_below(4);
            c[i] = r == 0 ? 0 : r == 1 ? 255 : (uint8_t)rnd();
            break;
        }
        case COV_RUNS:
            if (!run) {
                run = 1 + rnd_below(12);
                int r = rnd_below(3);
                val = r == 0 ? 0 : r == 1 ? 255 : (uint8_t)rnd();
            }
            c[i] = val;
            run--;
            break;
        default: {
            static const uint8_t edge[] = { 0, 1, 2, 127, 128, 129, 253, 254, 255 };
            c[i] = edge[rnd_below((int)sizeof(edge))];
            break;
        }
        }
    }
}

static uint32_t gen_color(void)
{
    switch (rnd_below(8)) {
    case 0: return 0xFF000000u;
    case 1: return 0xFFFFFFFFu;
    case 2: { uint32_t g = (uint32_t)rnd_below(256); return 0xFF000000u | g * 0x010101u; }
    case 3: return rnd32();
    default: return 0xFF000000u | (rnd32() & 0xFFFFFFu);
    }
}

/* A fg and bg pair: random, equal, near-equal, black on white, and so on. */
static void gen_pair(uint32_t *fg, uint32_t *bg)
{
    *fg = gen_color();
    switch (rnd_below(6)) {
    case 0: *bg = *fg; break;
    case 1: *bg = *fg ^ (1u << rnd_below(24)); break;
    case 2: *fg = 0xFF000000u; *bg = 0xFFFFFFFFu; break;
    case 3: *fg = 0xFFFFFFFFu; *bg = 0xFF000000u; break;
    default: *bg = gen_color(); break;
    }
}

static uint32_t with_alpha(uint32_t c, int kind)
{
    switch (kind) {
    case 0: return c | 0xFF000000u;
    case 1: return c & 0x00FFFFFFu;
    case 2: return (c & 0x00FFFFFFu) | (uint32_t)rnd_below(256) << 24;
    default: return c;
    }
}

/* Initial dst pixels. any_alpha also makes pixels with alpha below 255. */
static void gen_dst(uint32_t *d, int n, uint32_t bg, bool any_alpha)
{
    int kind = rnd_below(any_alpha ? 6 : 4);
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0: case 1: d[i] = bg; break;
        case 2: d[i] = rnd_below(4) ? bg : 0xFF000000u | rnd32(); break;
        case 3: d[i] = 0xFF000000u | rnd32(); break;
        case 4: d[i] = rnd32(); break;
        default: d[i] = with_alpha(bg, 1 + rnd_below(2)); break;
        }
    }
}

static int gen_len(int trial)
{
    static const int large[] = { 127, 128, 129, 255, 256, 257, 1000, 1280, 4099 };
    if (trial <= 80)
        return trial;
    return large[(trial - 81) % (int)(sizeof(large) / sizeof(large[0]))];
}

#define TRIALS (81 + 9)
#define REPEATS 8

static uint32_t init_px[DST_WORDS], want_px[DST_WORDS], src_px[DST_WORDS];
static uint8_t cov_buf[DST_WORDS * 3];

static void check_row(const char *what, int isa, const uint32_t *dst, int n)
{
    for (int i = 0; i < n; i++)
        CHECK(dst[i] == want_px[i],
              "%s [%s] n=%d i=%d: got %08x want %08x (dst was %08x)",
              what, render_isa_name(isa), n, i, dst[i], want_px[i], init_px[i]);
}

/* ---- Random corpus per kernel ---------------------------------------- */

static void corpus_fill(void)
{
    for (int t = 0; t < TRIALS; t++) {
        for (int r = 0; r < REPEATS; r++) {
            int n = gen_len(t);
            uint32_t color = rnd32();
            for (int i = 0; i < n; i++)
                want_px[i] = color;
            bool at_end = r & 1;
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place((size_t)n, at_end);
                dst_snapshot();
                render_kernels_for(levels[l])->fill(dst, n, color);
                check_row("fill", levels[l], dst, n);
                dst_check_guards("fill", levels[l], dst, (size_t)n, n, 1);
            }
        }
    }
}

static void corpus_blit_tile(void)
{
    for (int cw = 1; cw <= 40; cw++) {
        for (int r = 0; r < 6; r++) {
            int ch = 1 + rnd_below(r < 3 ? 24 : 40);
            size_t stride = (size_t)cw + (size_t)rnd_below(24);
            size_t words = (size_t)cw * (size_t)ch;
            for (size_t i = 0; i < words; i++)
                src_px[i] = rnd32();
            bool at_end = r & 1;
            const uint32_t *tile = (const uint32_t *)(const void *)in_place(
                &src_mem, src_px, words * 4, rnd_below(2), 4);
            size_t span = stride * (size_t)(ch - 1) + (size_t)cw;
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place(span, at_end);
                dst_snapshot();
                render_kernels_for(levels[l])->blit_tile(dst, stride, tile, cw, ch);
                for (size_t y = 0; y < (size_t)ch; y++)
                    for (size_t x = 0; x < (size_t)cw; x++)
                        CHECK(dst[y * stride + x] == src_px[y * (size_t)cw + x],
                              "blit_tile [%s] cw=%d ch=%d x=%zu y=%zu",
                              render_isa_name(levels[l]), cw, ch, x, y);
                dst_check_guards("blit_tile", levels[l], dst, stride, cw, ch);
                CHECK(!memcmp(tile, src_px, words * 4), "blit_tile changed its tile");
            }
        }
    }
    /* Zero sizes write nothing. */
    for (int l = 0; l < level_count; l++) {
        uint32_t *dst = dst_place(64, false);
        dst_snapshot();
        render_kernels_for(levels[l])->blit_tile(dst, 8, src_px, 0, 4);
        render_kernels_for(levels[l])->blit_tile(dst, 8, src_px, 8, 0);
        dst_check_guards("blit_tile zero", levels[l], dst, 8, 0, 0);
    }
}

static void corpus_alpha_lut(void)
{
    uint32_t lut[256];
    for (int t = 0; t < TRIALS; t++) {
        for (int r = 0; r < REPEATS; r++) {
            int n = gen_len(t);
            uint32_t fg, bg;
            gen_pair(&fg, &bg);
            bg |= 0xFF000000u;
            if (rnd_below(3)) {
                for (int a = 0; a < 256; a++)
                    lut[a] = blend_px(fg, bg, (uint8_t)a);
            } else {
                for (int a = 0; a < 256; a++)
                    lut[a] = rnd32();
            }
            gen_cov(cov_buf, n, r % COV_KINDS);
            for (int i = 0; i < n; i++)
                want_px[i] = lut[cov_buf[i]];
            const uint8_t *cov = in_place(&cov_mem, cov_buf, (size_t)n, r & 2, 1);
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place((size_t)n, r & 1);
                for (int i = 0; i < n; i++)
                    dst[i] = init_px[i] = rnd32();
                dst_snapshot();
                render_kernels_for(levels[l])->compose_alpha_lut(dst, cov, n, lut);
                check_row("compose_alpha_lut", levels[l], dst, n);
                dst_check_guards("compose_alpha_lut", levels[l], dst, (size_t)n, n, 1);
                CHECK(!memcmp(cov, cov_buf, (size_t)n), "compose_alpha_lut changed cov");
            }
        }
    }
}

static void gen_subpx_cov(uint8_t *c, int n, int kind)
{
    if (kind == COV_KINDS) {
        gen_cov(c, n, COV_MIXED);
        for (int i = n - 1; i >= 0; i--)
            c[3 * i] = c[3 * i + 1] = c[3 * i + 2] = c[i];
        return;
    }
    gen_cov(c, 3 * n, kind);
}

static void corpus_subpx_lut(void)
{
    static struct subpx_lut lut;
    for (int t = 0; t < TRIALS; t++) {
        for (int r = 0; r < REPEATS; r++) {
            int n = gen_len(t);
            uint32_t fg, bg;
            gen_pair(&fg, &bg);
            bg = with_alpha(bg, rnd_below(2) ? 0 : 3);
            if (rnd_below(4)) {
                blend_build_subpx_lut(&lut, fg, bg);
            } else {
                for (int i = 0; i < 256; i++) {
                    lut.r[i] = rnd32();
                    lut.g[i] = rnd32();
                    lut.b[i] = rnd32();
                }
            }
            gen_subpx_cov(cov_buf, n, r % (COV_KINDS + 1));
            for (int i = 0; i < n; i++)
                want_px[i] = blend_subpx_lut(&lut, bg, cov_buf[3 * i], cov_buf[3 * i + 1],
                                             cov_buf[3 * i + 2]);
            const uint8_t *cov = in_place(&cov_mem, cov_buf, (size_t)n * 3, r & 2, 1);
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place((size_t)n, r & 1);
                for (int i = 0; i < n; i++)
                    dst[i] = init_px[i] = rnd32();
                dst_snapshot();
                render_kernels_for(levels[l])->compose_subpx_lut(dst, cov, n, &lut, bg);
                check_row("compose_subpx_lut", levels[l], dst, n);
                dst_check_guards("compose_subpx_lut", levels[l], dst, (size_t)n, n, 1);
                CHECK(!memcmp(cov, cov_buf, (size_t)n * 3), "compose_subpx_lut changed cov");
            }
        }
    }
}

static uint32_t gen_premul(int kind)
{
    unsigned a = (unsigned)rnd_below(256);
    switch (kind) {
    case 0: {
        unsigned r = a ? (unsigned)rnd_below((int)a + 1) : 0;
        unsigned g = a ? (unsigned)rnd_below((int)a + 1) : 0;
        unsigned b = a ? (unsigned)rnd_below((int)a + 1) : 0;
        return a << 24 | r << 16 | g << 8 | b;
    }
    case 1: return rnd32();
    case 2: return rnd_below(2) ? 0 : rnd32() & 0x00FFFFFFu;
    default: return 0xFF000000u | rnd32();
    }
}

static void corpus_color(void)
{
    for (int t = 0; t < TRIALS; t++) {
        for (int r = 0; r < REPEATS; r++) {
            int n = gen_len(t);
            bool faint = r & 4;
            int kind = rnd_below(5);
            for (int i = 0; i < n; i++)
                src_px[i] = gen_premul(kind == 4 ? rnd_below(4) : kind);
            uint32_t bg = gen_color();
            gen_dst(init_px, n, bg, true);
            for (int i = 0; i < n; i++)
                want_px[i] = ref_premul(src_px[i], init_px[i], faint);
            const uint32_t *premul = (const uint32_t *)(const void *)in_place(
                &src_mem, src_px, (size_t)n * 4, r & 2, 4);
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place((size_t)n, r & 1);
                memcpy(dst, init_px, (size_t)n * 4);
                dst_snapshot();
                render_kernels_for(levels[l])->compose_color(dst, premul, n, faint);
                check_row("compose_color", levels[l], dst, n);
                dst_check_guards("compose_color", levels[l], dst, (size_t)n, n, 1);
                CHECK(!memcmp(premul, src_px, (size_t)n * 4), "compose_color changed its input");
            }
        }
    }
}

static void corpus_blend_any(bool subpx)
{
    const char *what = subpx ? "blend_any_subpx" : "blend_any";
    for (int t = 0; t < TRIALS; t++) {
        for (int r = 0; r < REPEATS; r++) {
            int n = gen_len(t);
            uint32_t fg, bg;
            gen_pair(&fg, &bg);
            gen_dst(init_px, n, bg, true);
            size_t bytes = (size_t)n * (subpx ? 3 : 1);
            if (subpx)
                gen_subpx_cov(cov_buf, n, r % (COV_KINDS + 1));
            else
                gen_cov(cov_buf, n, r % COV_KINDS);
            for (int i = 0; i < n; i++)
                want_px[i] = subpx
                    ? blend_subpx(fg, init_px[i], cov_buf[3 * i], cov_buf[3 * i + 1],
                                  cov_buf[3 * i + 2])
                    : blend_px(fg, init_px[i], cov_buf[i]);
            const uint8_t *cov = in_place(&cov_mem, cov_buf, bytes, r & 2, 1);
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place((size_t)n, r & 1);
                memcpy(dst, init_px, (size_t)n * 4);
                dst_snapshot();
                const struct render_kernels *k = render_kernels_for(levels[l]);
                if (subpx)
                    k->blend_any_subpx(dst, cov, n, fg);
                else
                    k->blend_any(dst, cov, n, fg);
                check_row(what, levels[l], dst, n);
                dst_check_guards(what, levels[l], dst, (size_t)n, n, 1);
                CHECK(!memcmp(cov, cov_buf, bytes), "%s changed cov", what);
            }
        }
    }
}

static void corpus_blend_rect(void)
{
    static const uint8_t edge_a[] = { 0, 1, 2, 127, 128, 254, 255 };
    for (int t = 0; t < TRIALS; t++) {
        for (int r = 0; r < REPEATS; r++) {
            int n = gen_len(t);
            uint32_t color, bg;
            gen_pair(&color, &bg);
            uint8_t a = rnd_below(2) ? edge_a[rnd_below((int)sizeof(edge_a))]
                                     : (uint8_t)rnd();
            gen_dst(init_px, n, bg, true);
            for (int i = 0; i < n; i++)
                want_px[i] = blend_px(color, init_px[i], a);
            for (int l = 0; l < level_count; l++) {
                uint32_t *dst = dst_place((size_t)n, r & 1);
                memcpy(dst, init_px, (size_t)n * 4);
                dst_snapshot();
                render_kernels_for(levels[l])->blend_rect(dst, n, color, a);
                check_row("blend_rect", levels[l], dst, n);
                dst_check_guards("blend_rect", levels[l], dst, (size_t)n, n, 1);
            }
        }
    }
}

/* ---- Exhaustive checks ----------------------------------------------- */

/* The lane forms that the SIMD variants use in place of divisions. */
static void exhaustive_lane_forms(void)
{
    for (uint32_t t = 0; t < (1u << 24) - 1; t++)
        CHECK(render_div255_wide(t) == t / 255, "div255_wide(%u)", t);
    for (uint32_t x = 0; x < 65535; x++)
        CHECK(render_div255_narrow(x) == x / 255, "div255_narrow(%u)", x);
    for (uint32_t rgb = 0; rgb < (1u << 24); rgb++)
        CHECK(render_lum16_lanes(rgb) == blend_lum16(rgb), "lum16(%06x)", rgb);
    for (int a = 1; a < 256; a++)
        for (int x = 0; x < 65536; x++)
            CHECK(render_div_f32(x, a) == x / a, "div_f32(%d, %d)", x, a);
    for (int fg_s = 0; fg_s < 256; fg_s++)
        for (int bg_s = 0; bg_s < 256; bg_s++)
            for (int a = 0; a < 256; a++)
                CHECK(render_srgb_coverage_lanes(fg_s, bg_s, (uint8_t)a) ==
                      srgb_coverage(fg_s, bg_s, (uint8_t)a),
                      "srgb_coverage(%d, %d, %d)", fg_s, bg_s, a);
}

#define SWEEP_ROW 4096
static uint32_t sweep_init[65536], sweep_want[65536], sweep_dst[65536];
static uint8_t sweep_cov[65536 * 3];

static void sweep_check(const char *what, int isa, int n, uint32_t fg)
{
    for (int i = 0; i < n; i++)
        CHECK(sweep_dst[i] == sweep_want[i],
              "%s [%s] i=%d fg=%08x dst=%08x: got %08x want %08x", what,
              render_isa_name(isa), i, fg, sweep_init[i], sweep_dst[i], sweep_want[i]);
}

/* blend_any over every bg RGB value, so every lane sees every bg luminance. */
static void sweep_blend_any_bg(void)
{
    for (uint32_t base = 0; base < (1u << 24); base += SWEEP_ROW) {
        uint32_t fg = (base / SWEEP_ROW) & 1 ? 0xFFE8E4DCu : 0xFF1C2024u;
        for (int i = 0; i < SWEEP_ROW; i++) {
            uint32_t rgb = base + (uint32_t)i;
            uint32_t alpha = (i & 3) ? 255u : (uint32_t)(i * 13) & 255u;
            sweep_init[i] = alpha << 24 | rgb;
            sweep_cov[i] = (uint8_t)(1 + (i * 7 + (int)(base / SWEEP_ROW)) % 254);
            sweep_want[i] = blend_px(fg, sweep_init[i], sweep_cov[i]);
        }
        for (int l = 0; l < level_count; l++) {
            if (levels[l] == RENDER_ISA_SCALAR)
                continue;
            memcpy(sweep_dst, sweep_init, sizeof(uint32_t) * SWEEP_ROW);
            render_kernels_for(levels[l])->blend_any(sweep_dst, sweep_cov, SWEEP_ROW, fg);
            sweep_check("blend_any bg sweep", levels[l], SWEEP_ROW, fg);
        }
    }
}

/*
 * blend_any_subpx for every (fg channel, bg channel, coverage) triple. This
 * covers the whole srgb_coverage() domain in each channel.
 */
static void sweep_blend_any_subpx(void)
{
    for (int f = 0; f < 256; f++) {
        uint32_t fg = 0xFF000000u | (uint32_t)f << 16 | (uint32_t)((f + 85) & 255) << 8 |
                      (uint32_t)((f + 170) & 255);
        for (int b0 = 0; b0 < 256; b0 += 64) {
            int n = 0;
            for (int b = b0; b < b0 + 64; b++) {
                uint32_t alpha = b & 7 ? 255u : (uint32_t)(b * 31) & 255u;
                uint32_t bg = alpha << 24 | (uint32_t)b << 16 |
                              (uint32_t)((b + 85) & 255) << 8 | (uint32_t)((b + 170) & 255);
                for (int c = 0; c < 256; c++, n++) {
                    sweep_init[n] = bg;
                    sweep_cov[3 * n] = (uint8_t)c;
                    sweep_cov[3 * n + 1] = (uint8_t)(c + 85);
                    sweep_cov[3 * n + 2] = (uint8_t)(c + 170);
                    sweep_want[n] = blend_subpx(fg, bg, (uint8_t)c, (uint8_t)(c + 85),
                                                (uint8_t)(c + 170));
                }
            }
            for (int l = 0; l < level_count; l++) {
                if (levels[l] == RENDER_ISA_SCALAR)
                    continue;
                memcpy(sweep_dst, sweep_init, sizeof(uint32_t) * (size_t)n);
                render_kernels_for(levels[l])->blend_any_subpx(sweep_dst, sweep_cov, n, fg);
                sweep_check("blend_any_subpx sweep", levels[l], n, fg);
            }
        }
    }
}

/* compose_color for every (alpha, channel, bg channel, faint). */
static void sweep_compose_color(void)
{
    static uint32_t premul[65536];
    for (int a = 0; a < 256; a++)
        for (int c = 0; c < 256; c++)
            premul[a * 256 + c] = (uint32_t)a << 24 | (uint32_t)c << 16 |
                                  (uint32_t)((c + 85) & 255) << 8 | (uint32_t)((c + 170) & 255);
    for (int faint = 0; faint < 2; faint++) {
        for (int b = 0; b < 256; b++) {
            uint32_t bg = (uint32_t)(b * 7 & 255) << 24 | (uint32_t)b << 16 |
                          (uint32_t)((b + 85) & 255) << 8 | (uint32_t)((b + 170) & 255);
            for (int i = 0; i < 65536; i++) {
                sweep_init[i] = bg;
                sweep_want[i] = ref_premul(premul[i], bg, faint);
            }
            for (int l = 0; l < level_count; l++) {
                if (levels[l] == RENDER_ISA_SCALAR)
                    continue;
                memcpy(sweep_dst, sweep_init, sizeof(sweep_init));
                render_kernels_for(levels[l])->compose_color(sweep_dst, premul, 65536, faint);
                sweep_check("compose_color sweep", levels[l], 65536, 0);
            }
        }
    }
}

/* Every coverage 0..255 for a set of fg and bg pairs on kernels 6-8. */
static void exhaustive_pairs(void)
{
    enum { PAIRS = 400 };
    for (int p = 0; p < PAIRS; p++) {
        uint32_t fg, bg;
        gen_pair(&fg, &bg);
        bg = with_alpha(bg, p % 4);
        uint32_t bg2 = with_alpha(gen_color(), rnd_below(4));

        /* blend_any: one bg, then alternating bgs. */
        for (int mode = 0; mode < 2; mode++) {
            for (int c = 0; c < 256; c++) {
                sweep_init[c] = mode && (c & 1) ? bg2 : bg;
                sweep_cov[c] = (uint8_t)c;
                sweep_want[c] = blend_px(fg, sweep_init[c], (uint8_t)c);
            }
            for (int l = 0; l < level_count; l++) {
                memcpy(sweep_dst, sweep_init, sizeof(uint32_t) * 256);
                render_kernels_for(levels[l])->blend_any(sweep_dst, sweep_cov, 256, fg);
                sweep_check("blend_any pairs", levels[l], 256, fg);
            }
        }

        /* blend_any_subpx: gray, one channel, and split channel coverage. */
        for (int mode = 0; mode < 4; mode++) {
            for (int c = 0; c < 256; c++) {
                uint8_t r = (uint8_t)c, g = (uint8_t)c, b = (uint8_t)c;
                if (mode == 1) { g = 0; b = 0; }
                if (mode == 2) { r = 0; g = (uint8_t)c; b = (uint8_t)(255 - c); }
                if (mode == 3) { g = (uint8_t)(c + 85); b = (uint8_t)(c + 170); }
                sweep_init[c] = mode == 3 && (c & 1) ? bg2 : bg;
                sweep_cov[3 * c] = r;
                sweep_cov[3 * c + 1] = g;
                sweep_cov[3 * c + 2] = b;
                sweep_want[c] = blend_subpx(fg, sweep_init[c], r, g, b);
            }
            for (int l = 0; l < level_count; l++) {
                memcpy(sweep_dst, sweep_init, sizeof(uint32_t) * 256);
                render_kernels_for(levels[l])->blend_any_subpx(sweep_dst, sweep_cov, 256, fg);
                sweep_check("blend_any_subpx pairs", levels[l], 256, fg);
            }
        }

        /* blend_rect: every alpha over a uniform row and a mixed row. */
        enum { RECT_N = 41 };
        for (int a = 0; a < 256; a++) {
            for (int mode = 0; mode < 2; mode++) {
                for (int i = 0; i < RECT_N; i++) {
                    sweep_init[i] = mode && i % 3 == 1 ? bg2 : bg;
                    sweep_want[i] = blend_px(fg, sweep_init[i], (uint8_t)a);
                }
                for (int l = 0; l < level_count; l++) {
                    memcpy(sweep_dst, sweep_init, sizeof(uint32_t) * RECT_N);
                    render_kernels_for(levels[l])->blend_rect(sweep_dst, RECT_N, fg, (uint8_t)a);
                    sweep_check("blend_rect pairs", levels[l], RECT_N, fg);
                }
            }
        }
    }
}

/* ---- ISA selection --------------------------------------------------- */

static void check_resolve(void)
{
    const char *env = getenv("WSAP_RENDER_ISA");
    char *saved = env ? strdup(env) : NULL;
    int best = render_isa_best();

    CHECK(best != RENDER_ISA_AUTO, "best level is auto");
    CHECK(render_isa_resolve(RENDER_ISA_SCALAR) == RENDER_ISA_SCALAR, "scalar");
    CHECK(render_isa_resolve(best) == best, "best resolves to itself");

    setenv("WSAP_RENDER_ISA", "scalar", 1);
    CHECK(render_isa_resolve(RENDER_ISA_AUTO) == RENDER_ISA_SCALAR, "auto with env scalar");
    setenv("WSAP_RENDER_ISA", "no-such-level", 1);
    CHECK(render_isa_resolve(RENDER_ISA_AUTO) == best, "unknown env value");
    unsetenv("WSAP_RENDER_ISA");
    CHECK(render_isa_resolve(RENDER_ISA_AUTO) == best, "auto without env");

    static const int requests[] = {
        RENDER_ISA_SCALAR, RENDER_ISA_AVX2, RENDER_ISA_AVX512, RENDER_ISA_NEON, 99, -1,
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        int want = requests[i];
        int got = render_isa_resolve(want);
        CHECK(render_isa_resolve(got) == got, "%d resolves to a runnable level", want);
        for (int e = RENDER_ISA_SCALAR; e <= RENDER_ISA_NEON; e++) {
            setenv("WSAP_RENDER_ISA", render_isa_name(e), 1);
            CHECK(render_isa_resolve(want) == got, "request %d with env %s",
                  want, render_isa_name(e));
        }
        unsetenv("WSAP_RENDER_ISA");
#if defined(RENDER_KERNEL_X86)
        if (want == RENDER_ISA_AVX2 || want == RENDER_ISA_AVX512)
            CHECK(got == (want <= best ? want : best), "x86 request %s gives %s",
                  render_isa_name(want), render_isa_name(got));
        CHECK(got <= best, "request %d gives %s above best %s", want, render_isa_name(got),
              render_isa_name(best));
#elif defined(RENDER_KERNEL_NEON)
        CHECK(got == (want == RENDER_ISA_SCALAR ? RENDER_ISA_SCALAR : RENDER_ISA_NEON),
              "aarch64 request %d gives %s", want, render_isa_name(got));
#else
        CHECK(got == RENDER_ISA_SCALAR, "request %d gives %s", want, render_isa_name(got));
#endif
    }

    CHECK(!strcmp(render_isa_name(RENDER_ISA_SCALAR), "scalar"), "name");
    CHECK(!strcmp(render_isa_name(RENDER_ISA_AVX2), "avx2"), "name");
    CHECK(!strcmp(render_isa_name(RENDER_ISA_AVX512), "avx512"), "name");
    CHECK(!strcmp(render_isa_name(RENDER_ISA_NEON), "neon"), "name");
    CHECK(!strcmp(render_isa_name(77), "unknown"), "name");
    for (int i = 0; i < level_count; i++)
        CHECK(render_kernels_for(levels[i])->isa == levels[i], "table for %s",
              render_isa_name(levels[i]));
    CHECK(render_kernels_for(RENDER_ISA_AUTO)->isa == RENDER_ISA_SCALAR, "auto table");

    if (saved) {
        setenv("WSAP_RENDER_ISA", saved, 1);
        free(saved);
    }
}

/* ---- Benchmark ------------------------------------------------------- */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

enum {
    B_FILL, B_BLIT, B_ALPHA_LUT, B_SUBPX_LUT, B_COLOR, B_BLEND_ANY, B_BLEND_ANY_MIXED,
    B_BLEND_ANY_SUBPX, B_RECT, B_RECT_MIXED, B_COUNT,
};

static const char *const bench_names[B_COUNT] = {
    "fill", "blit_tile (ch=16)", "compose_alpha_lut", "compose_subpx_lut", "compose_color",
    "blend_any", "blend_any (mixed dst)", "blend_any_subpx", "blend_rect",
    "blend_rect (mixed dst)",
};

static uint32_t bench_lut[256];
static struct subpx_lut bench_subpx;

static bool bench_reads_dst(int kind)
{
    return kind >= B_COLOR;
}

/* Text-like coverage: 45% empty, 20% full, 35% edge pixels. */
static uint8_t bench_cov(void)
{
    int r = rnd_below(100);
    return r < 45 ? 0 : r < 65 ? 255 : (uint8_t)(1 + rnd_below(254));
}

/*
 * The bench rotates through a pool of different rows. Then the branch
 * predictor cannot learn one row, as it cannot in a real frame.
 */
#define BENCH_POOL_PX (64 * 1280)

static uint32_t bench_dst[1344 * 16], bench_pristine[BENCH_POOL_PX], bench_src[BENCH_POOL_PX];
static uint8_t bench_covs[BENCH_POOL_PX * 3];

static int bench_rows(int n)
{
    return n >= 1280 ? 16 : 256;
}

static void bench_prepare(int kind, int n, size_t stride)
{
    int rows = bench_rows(n);
    bool mixed = kind == B_RECT_MIXED || kind == B_BLEND_ANY_MIXED;
    bool subpx = kind == B_SUBPX_LUT || kind == B_BLEND_ANY_SUBPX;
    for (size_t i = 0; i < stride * 16; i++)
        bench_dst[i] = 0xFF1E1E2Eu;
    for (int i = 0; i < rows * n; i++) {
        bench_pristine[i] = mixed ? 0xFF000000u | rnd32() : 0xFF1E1E2Eu;
        uint8_t c = bench_cov();
        if (subpx) {
            bool edge = c && c != 255;
            bench_covs[3 * i] = c;
            bench_covs[3 * i + 1] = edge ? (uint8_t)(1 + rnd_below(254)) : c;
            bench_covs[3 * i + 2] = edge ? (uint8_t)(1 + rnd_below(254)) : c;
        } else {
            bench_covs[i] = c;
        }
        if (kind == B_COLOR) {
            int r = rnd_below(10);
            bench_src[i] = r < 3 ? 0 : r < 7 ? 0xFF000000u | rnd32() : gen_premul(0);
        } else {
            bench_src[i] = rnd32();
        }
    }
}

static void bench_once(const struct render_kernels *k, int kind, int n, size_t stride, int row)
{
    uint32_t *dst = bench_dst;
    const uint8_t *cov = bench_covs + (size_t)row * (size_t)n *
                         (kind == B_SUBPX_LUT || kind == B_BLEND_ANY_SUBPX ? 3 : 1);
    const uint32_t *src = bench_src + (kind == B_BLIT ? 0 : (size_t)row * (size_t)n);
    switch (kind) {
    case B_FILL: k->fill(dst, n, 0xFF102030u); break;
    case B_BLIT: k->blit_tile(dst, stride, src, n, 16); break;
    case B_ALPHA_LUT: k->compose_alpha_lut(dst, cov, n, bench_lut); break;
    case B_SUBPX_LUT: k->compose_subpx_lut(dst, cov, n, &bench_subpx, 0xFF1E1E2Eu); break;
    case B_COLOR: k->compose_color(dst, src, n, false); break;
    case B_BLEND_ANY: case B_BLEND_ANY_MIXED: k->blend_any(dst, cov, n, 0xFFCDD6F4u); break;
    case B_BLEND_ANY_SUBPX: k->blend_any_subpx(dst, cov, n, 0xFFCDD6F4u); break;
    default: k->blend_rect(dst, n, 0xFF89B4FAu, 128); break;
    }
}

/* ns per call for reps calls. A kernel that reads dst gets a fresh row each call. */
static double bench_time(const struct render_kernels *k, int kind, int n, size_t stride,
                         long reps, bool restore_only)
{
    bool restore = bench_reads_dst(kind);
    int rows = bench_rows(n);
    double t0 = now_ns();
    for (long r = 0; r < reps; r++) {
        int row = (int)(r % rows);
        if (restore)
            memcpy(bench_dst, bench_pristine + (size_t)row * (size_t)n, (size_t)n * 4);
        if (!restore_only)
            bench_once(k, kind, n, stride, row);
        __asm__ volatile("" ::: "memory");
    }
    return (now_ns() - t0) / (double)reps;
}

static void benchmark(const char *only, const char *level)
{
    static const int sizes[] = { 8, 16, 1280 };
    for (int a = 0; a < 256; a++)
        bench_lut[a] = blend_px(0xFFCDD6F4u, 0xFF1E1E2Eu, (uint8_t)a);
    blend_build_subpx_lut(&bench_subpx, 0xFFCDD6F4u, 0xFF1E1E2Eu);

    printf("\nns per pixel (lower is better)\n%-24s %5s", "kernel", "n");
    for (int l = 0; l < level_count; l++)
        if (!level || !strcmp(level, render_isa_name(levels[l])))
            printf(" %9s", render_isa_name(levels[l]));
    printf("\n");
    for (int kind = 0; kind < B_COUNT; kind++) {
        if (only && strcmp(only, bench_names[kind]))
            continue;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            int n = sizes[s];
            size_t stride = (size_t)n + 64;
            rng_state = 0xBE4C4ull + (uint64_t)kind * 977 + (uint64_t)n;
            bench_prepare(kind, n, stride);
            double px_per_call = kind == B_BLIT ? (double)n * 16 : (double)n;
            const struct render_kernels *k0 = render_kernels_for(RENDER_ISA_SCALAR);
            long reps = 1;
            while (bench_time(k0, kind, n, stride, reps, false) * (double)reps < 4e6)
                reps *= 2;
            double best[4] = { 1e30, 1e30, 1e30, 1e30 };
            double copy = 1e30;
            for (int pass = 0; pass < 9; pass++) {
                for (int l = 0; l < level_count; l++) {
                    if (level && strcmp(level, render_isa_name(levels[l])))
                        continue;
                    double t = bench_time(render_kernels_for(levels[l]), kind, n, stride, reps,
                                          false);
                    if (t < best[l])
                        best[l] = t;
                }
                if (bench_reads_dst(kind)) {
                    double t = bench_time(k0, kind, n, stride, reps, true);
                    if (t < copy)
                        copy = t;
                }
            }
            if (!bench_reads_dst(kind))
                copy = 0;
            printf("%-24s %5d", bench_names[kind], n);
            for (int l = 0; l < level_count; l++)
                if (!level || !strcmp(level, render_isa_name(levels[l])))
                    printf(" %9.3f", (best[l] - copy) / px_per_call);
            printf("\n");
            fflush(stdout);
        }
    }
    printf("Each value is the best of 9 interleaved passes, in thread CPU time. Calls\n"
           "rotate through a pool of rows. Kernels that read dst get a fresh row\n"
           "before each call; the copy time is subtracted.\n");
}

/* ---- Main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    srgb_ensure_luts();
    find_levels();
    printf("render-kernels-test: auto=%s best=%s levels:",
           render_isa_name(render_isa_resolve(RENDER_ISA_AUTO)),
           render_isa_name(render_isa_best()));
    for (int i = 0; i < level_count; i++)
        printf(" %s", render_isa_name(levels[i]));
    printf("\n");
    fflush(stdout);

    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        benchmark(argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL);
        return 0;
    }

    dst_mem = guarded_new(DST_WORDS * 4);
    cov_mem = guarded_new(IN_BYTES);
    src_mem = guarded_new(IN_BYTES);

    check_resolve();
    corpus_fill();
    corpus_blit_tile();
    corpus_alpha_lut();
    corpus_subpx_lut();
    corpus_color();
    corpus_blend_any(false);
    corpus_blend_any(true);
    corpus_blend_rect();
    printf("random corpus: ok\n");
    fflush(stdout);
    exhaustive_lane_forms();
    printf("lane forms: ok\n");
    fflush(stdout);
    exhaustive_pairs();
    sweep_blend_any_bg();
    sweep_blend_any_subpx();
    sweep_compose_color();
    printf("exhaustive sweeps: ok\n");
    return 0;
}
