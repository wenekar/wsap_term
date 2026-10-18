#define _POSIX_C_SOURCE 200809L
/*
 * Tests of the row raster through render_draw():
 * - cross-ISA parity: every level that the CPU has gives the SCALAR pixels;
 * - spills: the ink of a glyph that leaves its cell lands on the neighbour
 *   cell, over the neighbour's own pixels, and never outside the grid;
 * - clipped repaints: frames whose damage is a UI rect that cuts through
 *   spilling cells give the pixels of a fresh paint;
 * - window scan: a narrow overlay that appears and vanishes at every x over
 *   rows with italic, icon, and right-to-left overhang gives the pixels of
 *   a fresh paint, in gray and RGB anti-aliasing.
 */
#include "helpers/render_draw.h"

#include "paint/blend.h"
#include "render/cpu/isa.h"

static struct font_instance *font;

static int cell_w(void)
{
    return font_instance_metrics(font)->cell_w;
}

static int cell_h(void)
{
    return font_instance_metrics(font)->cell_h;
}

/* ---- Cross-ISA parity ------------------------------------------------- */

static const char *const corpus[] = {
    "plain ascii text, 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
    "\033[3mitalic WAVATAYA fjord\033[0m \033[1mbold\033[0m \033[1;3mbold italic\033[0m",
    "\033[2mfaint text\033[0m \033[2;3mfaint italic W\033[0m \033[38;5;196mred\033[48;5;22mon green\033[0m",
    "\033[38;2;10;200;90m\033[48;2;40;10;60mtrue color W\033[0m \033[7mreverse\033[0m",
    "fn main() -> Result<(), E> { a != b && c <= d || e >= f; x == y } // => ===",
    "日本語のテキスト 한국어 中文 🙂🚀👍🏽 🇺🇸 👨‍👩‍👧 e\xcc\x81 a\xcc\x88",
    "\xef\x84\x95 icon \xe2\x97\x8f\xe2\x97\x8f dot ─│┌┐└┘ \xee\x82\xb0 powerline",
    ("\033[4:1msingle\033[4:2mdouble\033[4:3mcurly\033[4:4mdotted\033[4:5mdashed\033[0m "
     "\033[9mstrike\033[0m \033[53mover\033[0m \033[2;4:3mfaint curly\033[0m"),
    "\033]8;;https://example.com\033\\a link\033]8;;\033\\ \033[48;5;94m   \033[4m   \033[0m",
};

/* Fills a terminal with the whole corpus, a selection, and a cursor. */
static void corpus_term(struct tterm *tt, int variant)
{
    tterm_open(tt, 72, 14);
    for (int i = 0; i < (int)(sizeof(corpus) / sizeof(corpus[0])); i++) {
        tterm_feed(tt, corpus[(i + variant) % (int)(sizeof(corpus) / sizeof(corpus[0]))]);
        tterm_feed(tt, "\r\n");
    }
    if (variant & 1)
        tt_select(tt->t, 3, 1, 20, 3);
    if (variant & 2)
        tterm_feed(tt, "\033[?5h");
    tterm_feed(tt, variant & 4 ? "\033[6 q" : "\033[4 q");
    tterm_feed(tt, "\033[5;9H");
}

static struct render_frame corpus_frame(const struct terminal_frame *f, struct vector2i size,
                                        struct ui_scene *scene)
{
    struct render_frame fr = tframe(f, font, size);
    fr.grid.rect = trect(3, 2, size.x - 3, size.y - 2);
    fr.ui = scene;
    fr.ui_damage = NULL;
    return fr;
}

static void test_isa_parity(void)
{
    static const int levels[] = {
        RENDER_ISA_SCALAR, RENDER_ISA_AVX2, RENDER_ISA_AVX512, RENDER_ISA_NEON,
    };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    TCHECK(ui_scene_rect(&scene, trect(40, 30, 90, 40), 0x80FF8040u), "rect");
    TCHECK(ui_scene_rounded_rect(&scene, trect(200, 10, 60, 120), 6, 0xC0204080u), "rrect");

    int checked = 0;
    for (int aa = 0; aa < 2; aa++) {
        font_set_aa_mode(aa ? FONT_AA_RGB : FONT_AA_GRAYSCALE);
        for (int variant = 0; variant < 8; variant++) {
            struct tterm tt;
            corpus_term(&tt, variant);
            const struct terminal_frame *f = tterm_frame(&tt, true);
            struct vector2i size = { .x = 72 * cell_w() + 9, .y = 14 * cell_h() + 7 };
            size_t n = (size_t)size.x * (size_t)size.y;
            uint32_t *want = malloc(n * sizeof(*want)), *got = malloc(n * sizeof(*got));
            TCHECK(want && got, "buffers");
            struct render_frame fr = corpus_frame(f, size, &scene);

            for (unsigned li = 0; li < sizeof(levels) / sizeof(levels[0]); li++) {
                struct renderer *r = renderer_create(&(struct render_options){
                    .isa = levels[li], .threads = variant & 1 ? 3 : RENDER_NONE,
                    .cache_bytes = variant & 2 ? 64 << 10 : 0 });
                TCHECK(r, "renderer");
                if (renderer_isa(r) != levels[li]) {
                    renderer_destroy(r);
                    continue;
                }
                /* A cold paint and two warm paints, so that tiles and the
                 * direct compose both run at this level. */
                for (int pass = 0; pass < 3; pass++) {
                    tdraw_fresh(r, li ? got : want, size, &fr);
                    if (li) {
                        char what[96];
                        snprintf(what, sizeof(what), "%s aa %d variant %d pass %d",
                                 render_isa_name(levels[li]), aa, variant, pass);
                        tcheck_same(what, got, want, size);
                        checked++;
                    }
                }
                renderer_destroy(r);
            }
            free(want);
            free(got);
            tterm_close(&tt);
        }
    }
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    ui_scene_destroy(&scene);
    printf("isa parity: %d paints compared with scalar (best level %s)\n", checked,
           render_isa_name(render_isa_best()));
}

/* The incremental path at each level gives the scalar pixels too. */
static void test_isa_incremental(void)
{
    struct renderer *ref = renderer_create(&(struct render_options){ .isa = RENDER_ISA_SCALAR });
    struct renderer *best = renderer_create(&(struct render_options){ .isa = render_isa_best() });
    TCHECK(ref && best, "renderers");
    struct tterm tt;
    tterm_open(&tt, 60, 12);
    struct vector2i size = { .x = 60 * cell_w(), .y = 12 * cell_h() };
    struct tcanvas a, b;
    tcanvas_init(&a, size.x, size.y);
    tcanvas_init(&b, size.x, size.y);
    for (int step = 0; step < 120; step++) {
        tterm_feed(&tt, corpus[step % (int)(sizeof(corpus) / sizeof(corpus[0]))]);
        tterm_feed(&tt, step % 3 ? " " : "\r\n");
        const struct terminal_frame *f = tterm_frame(&tt, true);
        struct render_frame fr = tframe(f, font, size);
        tdraw(ref, &a, &fr);
        tdraw(best, &b, &fr);
        tcheck_same("incremental at the best level", b.px, a.px, size);
    }
    tcanvas_free(&a);
    tcanvas_free(&b);
    tterm_close(&tt);
    renderer_destroy(ref);
    renderer_destroy(best);
}

/* ---- Spills ----------------------------------------------------------- */

struct spiller {
    const char *seq;
    uint32_t cp;
    uint16_t style;
};

/* The ink columns of a glyph, in cell-local x. */
static bool glyph_span(const struct glyph *g, int *lo, int *hi)
{
    *lo = INT32_MAX;
    *hi = INT32_MIN;
    if (!g || !g->row_masks)
        return false;
    for (int y = 0; y < g->height && y < cell_h(); y++)
        for (int x = 0; x < g->width; x++)
            if (glyph_inked(g, y, x)) {
                if (g->x_offset + x < *lo)
                    *lo = g->x_offset + x;
                if (g->x_offset + x > *hi)
                    *hi = g->x_offset + x;
            }
    return *lo <= *hi;
}

/* The pixel that the glyph column at cell-local x blends over `under`. */
static uint32_t glyph_over(const struct glyph *g, int y, int x, uint32_t fg, uint32_t under)
{
    int gx = x - g->x_offset;
    if (y >= g->height || gx < 0 || gx >= g->width || !glyph_inked(g, y, gx))
        return under;
    if (g->has_subpx_ink && g->subpx) {
        const uint8_t *s = glyph_subpx_at(g, y, gx);
        return blend_subpx(fg, under, s[0], s[1], s[2]);
    }
    return blend_px(fg, under, g->alpha[y][gx]);
}

/*
 * Draws "<sp>X<sp><sp>" with the spilling glyph X in column 1 and checks
 * every pixel of the neighbour cells: the glyph ink over the plain bg.
 * Then draws "XX" and checks that the spill of the left X lies over the
 * pixels of the right X. Returns false when this font does not spill X.
 */
static bool check_spill(struct renderer *r, const struct spiller *sp, bool rgb)
{
    font_set_aa_mode(rgb ? FONT_AA_RGB : FONT_AA_GRAYSCALE);
    glyph_cache_refresh(font);
    const struct glyph *g = glyph_cache_fetch(font, sp->cp, sp->style);
    int lo, hi, cw = cell_w(), ch = cell_h();
    if (!glyph_span(g, &lo, &hi) || (lo >= 0 && hi < cw))
        return false;

    struct tterm tt;
    tterm_open(&tt, 6, 1);
    char text[64];
    snprintf(text, sizeof(text), " %s  ", sp->seq);
    tterm_feed(&tt, text);
    const struct terminal_frame *f = tterm_frame(&tt, false);
    struct vector2i size = { .x = 6 * cw, .y = ch };
    uint32_t *px = malloc((size_t)size.x * (size_t)ch * sizeof(*px));
    uint32_t *alone = malloc((size_t)size.x * (size_t)ch * sizeof(*px));
    TCHECK(px && alone, "buffers");
    struct render_frame fr = tframe(f, font, size);
    tdraw_fresh(r, px, size, &fr);
    uint32_t fg = f->default_fg, bg = f->default_bg;
    /* The glyph must be fetched again: the draw may have refilled the slot. */
    g = glyph_cache_fetch(font, sp->cp, sp->style);
    int inked = 0;
    for (int y = 0; y < ch; y++) {
        for (int x = -cw; x < 2 * cw; x++) {
            if (x >= 0 && x < cw)
                continue;
            uint32_t want = glyph_over(g, y, x, fg, bg);
            uint32_t got = px[(size_t)y * size.x + cw + x];
            TCHECK(got == want, "U+%04X %s: spill pixel (%d, %d) is %08x, want %08x", sp->cp,
                   rgb ? "rgb" : "gray", x, y, got, want);
            inked += want != bg;
        }
    }
    TCHECK(inked > 0, "U+%04X: the spill reaches no neighbour pixel", sp->cp);

    /* The same glyph to the right: the left spill lies on top of it. */
    if (hi >= cw) {
        tterm_feed(&tt, "\033[2J\033[H ");
        tterm_feed(&tt, sp->seq);
        tterm_feed(&tt, "\033[0m");
        f = tterm_frame(&tt, false);
        fr = tframe(f, font, size);
        tdraw_fresh(r, alone, size, &fr);
        tterm_feed(&tt, "\033[H");
        tterm_feed(&tt, sp->seq);
        tterm_feed(&tt, sp->seq);
        f = tterm_frame(&tt, false);
        fr = tframe(f, font, size);
        tdraw_fresh(r, px, size, &fr);
        g = glyph_cache_fetch(font, sp->cp, sp->style);
        for (int y = 0; y < ch; y++)
            for (int x = cw; x < 2 * cw; x++) {
                uint32_t want = glyph_over(g, y, x, fg, alone[(size_t)y * size.x + x]);
                uint32_t got = px[(size_t)y * size.x + x];
                TCHECK(got == want, "U+%04X: spill over the neighbour glyph at (%d, %d) is "
                                    "%08x, want %08x", sp->cp, x, y, got, want);
            }
    }
    free(px);
    free(alone);
    tterm_close(&tt);
    return true;
}

static void test_spills(void)
{
    static const struct spiller cases[] = {
        { "\033[3mW\033[23m", 'W', FONT_STYLE_ITALIC },
        { "\033[3mf\033[23m", 'f', FONT_STYLE_ITALIC },
        { "\xef\x84\x95", 0xF115, 0 },
        { "\xe2\x97\x8f", 0x25CF, 0 },
        { "W", 'W', 0 },
        { "\033[1mW\033[22m", 'W', FONT_STYLE_BOLD },
    };
    struct renderer *r = renderer_create(NULL);
    struct renderer *direct = renderer_create(&(struct render_options){
        .threads = RENDER_NONE, .cache_bytes = RENDER_NONE });
    TCHECK(r && direct, "renderers");
    int alpha = 0, subpx = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int rgb = 0; rgb < 2; rgb++) {
            /* Twice with the cache: the second draw blits a tile with the spill flag. */
            bool spilled = check_spill(r, &cases[i], rgb);
            if (spilled) {
                TCHECK(check_spill(r, &cases[i], rgb), "repeat");
                TCHECK(check_spill(direct, &cases[i], rgb), "direct");
            }
            alpha += spilled && !rgb;
            subpx += spilled && rgb;
        }
    }
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    printf("spills: %d grayscale and %d subpixel glyphs spill\n", alpha, subpx);
    /* The bundled font has spilling glyphs in both modes. */
    TCHECK(alpha >= 2 && subpx >= 1, "too few spilling glyphs: %d gray, %d rgb", alpha, subpx);
    renderer_destroy(r);
    renderer_destroy(direct);
}

/* Spills clip to the cell area: the base left of the grid and the right
 * margin keep their colors. */
static void test_spill_clip(void)
{
    struct renderer *r = renderer_create(NULL);
    TCHECK(r, "renderer");
    int cw = cell_w(), ch = cell_h();
    struct tterm tt;
    tterm_open(&tt, 8, 3);
    /* A left spill in column 0, a right spill in the last column, and a
     * wide glyph whose right half is past the last column. */
    tterm_feed(&tt, "\xe2\x97\x8f    \xef\x84\x95\r\n\033[3m     W\033[0m\r\n     🙂");
    const struct terminal_frame *f = tterm_frame(&tt, false);
    /* Six visible columns: the grid rect is narrower than the terminal. */
    struct vector2i size = { .x = 7 * cw + 20, .y = 3 * ch + 8 };
    uint32_t *px = calloc((size_t)size.x * (size_t)size.y, sizeof(*px));
    TCHECK(px, "buffer");
    struct render_frame fr = tframe(f, font, size);
    fr.grid.rect = trect(10, 4, 6 * cw + cw - 1, 3 * ch);
    fr.base = 0xFF102030u;
    tdraw_fresh(r, px, size, &fr);
    for (int y = 0; y < size.y; y++)
        for (int x = 0; x < size.x; x++) {
            uint32_t p = px[(size_t)y * size.x + x];
            bool in_grid = rect2i_contains(fr.grid.rect, (struct vector2i){ .x = x, .y = y });
            bool margin = in_grid && x >= 10 + 6 * cw;
            if (!in_grid)
                TCHECK(p == fr.base, "base pixel (%d, %d) is %08x", x, y, p);
            else if (margin)
                TCHECK(p == f->default_bg, "margin pixel (%d, %d) is %08x", x, y, p);
        }
    free(px);
    tterm_close(&tt);
    renderer_destroy(r);
}

/* ---- Clipped repaints --------------------------------------------------- */

/*
 * A UI rect moves over rows full of spilling glyphs. Its edges cut through
 * cells. The damage of each frame is the UI rect only, so the grid repaints
 * in clipped windows. Each frame must equal a fresh paint.
 */
static void test_window_repaint(void)
{
    struct renderer *r = renderer_create(NULL);
    struct renderer *oracle = renderer_create(&(struct render_options){
        .threads = RENDER_NONE, .cache_bytes = RENDER_NONE });
    TCHECK(r && oracle, "renderers");
    int cw = cell_w(), ch = cell_h();
    for (int rgb = 0; rgb < 2; rgb++) {
        font_set_aa_mode(rgb ? FONT_AA_RGB : FONT_AA_GRAYSCALE);
        struct tterm tt;
        tterm_open(&tt, 40, 6);
        tterm_feed(&tt, "\033[3mWWWWfWWWWW AVAVAVA WfWfWf\033[0m \xef\x84\x95\xef\x84\x95 "
                        "\xe2\x97\x8f\xe2\x97\x8f\xe2\x97\x8f\r\n");
        tterm_feed(&tt, "\xef\x84\x95 \xef\x84\x95\xef\x84\x95 \033[38;5;214m\xef\x84\x95\033[0m "
                        "🙂🚀 日本語 \033[3;4:3mWWWW\033[0m\r\n");
        tterm_feed(&tt, "\033[2;3mfaint WWWW\033[0m \033[7m\033[3mWWW\033[0m e\xcc\x81\xe2\x97\x8f\r\n");
        tterm_feed(&tt, "\033[48;5;52m\033[3mWW  WW\033[0m \xe2\x97\x8f\033[48;5;17m\xe2\x97\x8f\033[0m");
        tt_select(tt.t, 5, 2, 12, 2);
        const struct terminal_frame *f = tterm_frame(&tt, true);
        struct vector2i size = { .x = 40 * cw + 4, .y = 6 * ch + 3 };
        struct tcanvas c;
        tcanvas_init(&c, size.x, size.y);
        uint32_t *want = malloc(tcanvas_px(&c) * sizeof(*want));
        uint32_t *before = malloc(tcanvas_px(&c) * sizeof(*before));
        TCHECK(want && before, "buffers");

        struct rect2i old = {0};
        int windowed = 0;
        for (int step = 0; step < 4 * 40; step++) {
            /* Odd offsets put the rect edges inside cells. */
            struct rect2i now = trect((step * 7) % (size.x - 20) + 1, ((step / 3) % 5) * ch + 3,
                                      13 + step % 11, ch + 5);
            bool shown = step % 9 != 8;
            struct ui_scene scene = {0};
            ui_scene_init(&scene);
            if (shown)
                TCHECK(ui_scene_blend_rect(&scene, now, 0xFF30A0F0u, 120), "rect");
            struct ui_damage ud = {0};
            if (!rect2i_is_empty(old))
                ui_damage_add(&ud, old);
            if (shown)
                ui_damage_add(&ud, now);
            struct render_frame fr = tframe(f, font, size);
            fr.ui = &scene;
            fr.ui_damage = step ? &ud : NULL;
            memcpy(before, c.px, tcanvas_px(&c) * sizeof(*before));
            struct render_result res = tdraw(r, &c, &fr);
            tdraw_fresh(oracle, want, size, &fr);
            char what[64];
            snprintf(what, sizeof(what), "window step %d rgb %d", step, rgb);
            tcheck_same(what, c.px, want, size);
            if (step) {
                tcheck_damage(what, before, c.px, size, &res.damage);
                /* No row repainted in full: the damage is the UI rects. */
                struct rect2i box = rect2i_union(old, shown ? now : (struct rect2i){0});
                TCHECK(!res.damage.all &&
                           tdamage_area(&res.damage, size) <= (long)box.width * box.height,
                       "%s: the damage is larger than the UI rects", what);
                windowed++;
            }
            old = shown ? now : (struct rect2i){0};
            ui_scene_destroy(&scene);
        }
        printf("windows: %d clipped frames (rgb %d)\n", windowed, rgb);
        free(want);
        free(before);
        tcanvas_free(&c);
        tterm_close(&tt);
    }
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    renderer_destroy(r);
    renderer_destroy(oracle);
}

/* ---- Window scan -------------------------------------------------------- */

/*
 * An overlay of 1 to 3 px appears and then vanishes at every x. Each step
 * repaints only the damaged window of rows that the planner keeps. The
 * window must grow by the overhang of the cells next to it. If it does not,
 * the ink that spills into the window stays wrong at some x.
 */
static void scan_windows(struct font_instance *f, struct renderer *oracle)
{
    const struct font_metrics *m = font_instance_metrics(f);
    int cols = 24, rows = 4;
    struct vector2i size = { .x = cols * m->cell_w + 5, .y = rows * m->cell_h };
    struct renderer *r = renderer_create(&(struct render_options){ .threads = RENDER_NONE });
    TCHECK(r, "renderer");
    struct tcanvas c;
    tcanvas_init(&c, size.x, size.y);
    struct tterm t;
    tterm_open(&t, cols, rows);
    tterm_feed(&t,
        "\033[?25l\033[3mfjfWAVfjyg\033[0m\xef\x84\x95\xef\x84\x95\xee\x82\xb0x\xee\x82\xb2\033[3m//ff\r\n"
        "\033[1;3mWWfjf\033[0m\033[48;5;52m\033[3mfjf\033[0m\xef\x84\x95 \xef\x84\x95\033[3;2mjjj\r\n"
        "\033[3m\033[38;5;196mf\033[38;5;46mf\033[38;5;21mf\033[48;5;100mj\033[0m"
        "\xe2\x97\x8f\xe2\x97\x8f\xe2\x80\x94\xe2\x80\x94\r\n"
        "\033[3m\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 fjfjfj\033[0m\xef\x84\x95\xef\x84\x95");
    size_t n = tcanvas_px(&c);
    uint32_t *want = malloc(n * sizeof(*want)), *before = malloc(n * sizeof(*before));
    TCHECK(want && before, "buffers");
    struct ui_scene sc = {0};
    ui_scene_init(&sc);
    const struct terminal_frame *tf = tterm_frame(&t, false);
    struct render_frame fr = {
        .base = 0xFF000000u,
        .grid = { .frame = tf, .font = f, .rect = { .size = size } },
        .ui = &sc,
        .ui_font = font_instance_default(),
    };
    struct ui_damage ud = { .all = true };
    fr.ui_damage = &ud;
    (void)tdraw(r, &c, &fr);
    for (int w = 1; w <= 3; w++) {
        for (int x = -2; x < size.x; x++) {
            for (int on = 1; on >= 0; on--) {
                ui_scene_destroy(&sc);
                ui_scene_init(&sc);
                struct rect2i ov = trect(x, 0, w, size.y);
                if (on)
                    TCHECK(ui_scene_blend_rect(&sc, ov, 0xFFFF00FFu, 128), "overlay");
                ud = (struct ui_damage){0};
                ui_damage_add(&ud, ov);
                memcpy(before, c.px, n * sizeof(*before));
                struct render_result res = tdraw(r, &c, &fr);
                tdraw_fresh(oracle, want, size, &fr);
                char what[96];
                snprintf(what, sizeof(what), "window w %d x %d %s", w, x, on ? "on" : "off");
                tcheck_same(what, c.px, want, size);
                tcheck_damage(what, before, c.px, size, &res.damage);
            }
        }
    }
    ui_scene_destroy(&sc);
    free(want);
    free(before);
    tcanvas_free(&c);
    tterm_close(&t);
    renderer_destroy(r);
}

static void test_window_scan(void)
{
    struct renderer *oracle = renderer_create(&(struct render_options){
        .threads = RENDER_NONE, .cache_bytes = RENDER_NONE });
    TCHECK(oracle, "oracle");
    struct font_instance *big = font_instance_acquire(font_instance_size_pt(font), 180);
    TCHECK(big, "1.5x font");
    for (int aa = 0; aa < 2; aa++) {
        font_set_aa_mode(aa ? FONT_AA_RGB : FONT_AA_GRAYSCALE);
        scan_windows(font, oracle);
        scan_windows(big, oracle);
    }
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    font_instance_unref(big);
    renderer_destroy(oracle);
}

int main(void)
{
    TCHECK(render_init(), "render_init");
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    font = font_instance_default();
    TCHECK(font, "font");
    test_isa_parity();
    test_isa_incremental();
    test_spills();
    test_spill_clip();
    test_window_repaint();
    test_window_scan();
    render_shutdown();
    puts("PASS: row raster");
    return 0;
}
