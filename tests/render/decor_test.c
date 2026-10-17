#define _POSIX_C_SOURCE 200809L
/*
 * Tests of decorations and cursor marks, through render_draw() pixels.
 *
 * - Each underline kind has its own row shape: single and double are solid
 *   lines, curly is an antialiased wave, dotted and dashed have gaps.
 * - The pattern of each cell is the same, so it continues across cells and
 *   across both halves of a wide glyph.
 * - Faint halves the coverage of the decorations and the glyph.
 * - The bar and underline cursors mark the cell in the cursor color and
 *   leave the rest of the cell as it was.
 */
#include "helpers/render_draw.h"

#include "paint/blend.h"

static struct font_instance *font;
static struct renderer *rend;
static int cw, ch;

#define COLS 12

/* A one-row terminal, drawn fresh. px holds COLS cells. */
struct shot {
    uint32_t *px;
    struct vector2i size;
    uint32_t fg, bg, cursor;
};

static struct shot draw_row(const char *text, bool cursor)
{
    struct tterm tt;
    tterm_open(&tt, COLS, 1);
    tterm_feed(&tt, text);
    const struct terminal_frame *f = tterm_frame(&tt, cursor);
    struct shot s = {
        .size = { .x = COLS * cw, .y = ch },
        .fg = f->default_fg, .bg = f->default_bg, .cursor = f->cursor_color,
    };
    s.px = malloc((size_t)s.size.x * (size_t)ch * sizeof(*s.px));
    TCHECK(s.px, "buffer");
    struct render_frame fr = tframe(f, font, s.size);
    tdraw_fresh(rend, s.px, s.size, &fr);
    tterm_close(&tt);
    return s;
}

static uint32_t at(const struct shot *s, int col, int x, int y)
{
    return s->px[(size_t)y * s->size.x + col * cw + x];
}

/* Rows of one cell that hold pixels other than bg. */
static int ink_rows(const struct shot *s, int col, int *y0, int *y1)
{
    *y0 = -1;
    *y1 = -1;
    int n = 0;
    for (int y = 0; y < ch; y++) {
        bool inked = false;
        for (int x = 0; x < cw; x++)
            inked |= at(s, col, x, y) != s->bg;
        if (!inked)
            continue;
        if (*y0 < 0)
            *y0 = y;
        *y1 = y + 1;
        n++;
    }
    return n;
}

/* The coverage of fg over bg in a pixel, from the channel where they differ most. */
static int coverage(const struct shot *s, uint32_t p)
{
    int best = 0, shift = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int d = abs((int)((s->fg >> sh) & 255) - (int)((s->bg >> sh) & 255));
        if (d > best) {
            best = d;
            shift = sh;
        }
    }
    if (!best)
        return 0;
    int v = abs((int)((p >> shift) & 255) - (int)((s->bg >> shift) & 255));
    return v * 255 / best;
}

static bool row_solid(const struct shot *s, int col, int y, uint32_t color)
{
    for (int x = 0; x < cw; x++)
        if (at(s, col, x, y) != color)
            return false;
    return true;
}

static bool row_blank(const struct shot *s, int col, int y)
{
    return row_solid(s, col, y, s->bg);
}

/* Cells [c0, c1) have the same pixels. */
static void check_repeat(const char *what, const struct shot *s, int c0, int c1)
{
    for (int c = c0 + 1; c < c1; c++)
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                TCHECK(at(s, c, x, y) == at(s, c0, x, y),
                       "%s: cell %d differs from cell %d at (%d, %d)", what, c, c0, x, y);
}

/* Solid runs of rows in fg. Returns their count. */
static int solid_runs(const struct shot *s, int col, uint32_t color)
{
    int runs = 0;
    bool in = false;
    for (int y = 0; y < ch; y++) {
        bool solid = row_solid(s, col, y, color);
        if (solid && !in)
            runs++;
        if (!solid && !row_blank(s, col, y))
            return -1;
        in = solid;
    }
    return runs;
}

static void test_underline_kinds(void)
{
    int single_y0 = 0, single_y1 = 0, dotted_total = 0;
    for (int kind = 1; kind <= 5; kind++) {
        char text[64];
        snprintf(text, sizeof(text), "\033[4:%dm%*s", kind, COLS, "");
        struct shot s = draw_row(text, false);
        char what[32];
        snprintf(what, sizeof(what), "underline %d", kind);
        int y0, y1;
        int rows = ink_rows(&s, 0, &y0, &y1);
        TCHECK(rows > 0, "%s: no ink", what);
        TCHECK(y0 >= ch / 2, "%s: ink starts at row %d of %d", what, y0, ch);
        /* The same pixels in every cell: the pattern continues. */
        check_repeat(what, &s, 0, COLS);
        switch (kind) {
        case 1:
            TCHECK(solid_runs(&s, 0, s.fg) == 1, "%s: not one solid line", what);
            single_y0 = y0;
            single_y1 = y1;
            break;
        case 2:
            TCHECK(solid_runs(&s, 0, s.fg) == 2, "%s: not two solid lines", what);
            break;
        case 3: {
            /* An antialiased wave: partial pixels, and taller than a line. */
            int partial = 0, top = ch, bottom = -1, ytop[64], ybot[64];
            for (int x = 0; x < cw && x < 64; x++) {
                ytop[x] = ch;
                ybot[x] = -1;
                for (int y = 0; y < ch; y++) {
                    uint32_t p = at(&s, 0, x, y);
                    if (p == s.bg)
                        continue;
                    partial += p != s.fg;
                    ytop[x] = y < ytop[x] ? y : ytop[x];
                    ybot[x] = y;
                }
                TCHECK(ybot[x] >= 0, "%s: column %d has no ink", what, x);
                top = ytop[x] < top ? ytop[x] : top;
                bottom = ybot[x] > bottom ? ybot[x] : bottom;
            }
            TCHECK(partial > 0, "%s: no antialiased pixel", what);
            TCHECK(bottom - top + 1 > single_y1 - single_y0, "%s: no wave", what);
            /* The wave goes up and down inside one cell. */
            bool moves = false;
            for (int x = 1; x < cw && x < 64; x++)
                moves |= ytop[x] != ytop[0];
            TCHECK(moves, "%s: the wave is flat", what);
            /* Seam: the last column of a cell meets the first column of
             * the next one. */
            int last = (cw < 64 ? cw : 64) - 1;
            TCHECK(ytop[last] <= ybot[0] + 1 && ytop[0] <= ybot[last] + 1,
                   "%s: the wave breaks at the cell edge", what);
            break;
        }
        case 4:
        case 5: {
            /* Gaps inside the ink rows. A gap can be thinner than a pixel,
             * so a gap is a column below half coverage. A dash is longer
             * than a dot: the dashes cover more of the line. */
            int gaps = 0, total = 0;
            for (int x = 0; x < cw; x++) {
                int best = 0;
                for (int yy = y0; yy < y1; yy++) {
                    int cov = coverage(&s, at(&s, 0, x, yy));
                    best = cov > best ? cov : best;
                }
                gaps += best < 128;
                total += best;
            }
            TCHECK(gaps > 0, "%s: no gap", what);
            if (kind == 4)
                dotted_total = total;
            else
                TCHECK(total > dotted_total, "%s: dashes %d cover no more than dots %d", what,
                       total, dotted_total);
            TCHECK(y1 - y0 <= (single_y1 - single_y0) + 1, "%s: too tall", what);
            break;
        }
        }
        free(s.px);
    }
}

static void test_strike_overline_link(void)
{
    struct shot s = draw_row("\033[9m    \033[29;53m    \033[55m\033]8;;x\033\\    \033]8;;\033\\", false);
    int y0, y1;
    TCHECK(ink_rows(&s, 0, &y0, &y1) > 0 && solid_runs(&s, 0, s.fg) == 1, "strike: one line");
    TCHECK(y0 >= ch / 4 && y1 <= ch - ch / 4, "strike: rows %d..%d of %d", y0, y1, ch);
    check_repeat("strike", &s, 0, 4);
    TCHECK(ink_rows(&s, 4, &y0, &y1) > 0 && solid_runs(&s, 4, s.fg) == 1, "overline: one line");
    TCHECK(y1 <= ch / 4 + 1, "overline: rows %d..%d of %d", y0, y1, ch);
    check_repeat("overline", &s, 4, 8);
    /* A link without an underline style gets the single underline. */
    struct shot u = draw_row("\033[4m    ", false);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            TCHECK(at(&s, 8, x, y) == at(&u, 0, x, y), "link: not the single underline");
    free(s.px);
    free(u.px);
}

/* Colored decorations take the cell fg. */
static void test_colors(void)
{
    struct shot s = draw_row("\033[38;2;250;40;90;4m    \033[24;9m    ", false);
    uint32_t red = 0xFFFA285Au;
    TCHECK(solid_runs(&s, 0, red) == 1 && solid_runs(&s, 4, red) == 1, "colored lines");
    free(s.px);
}

static void test_faint(void)
{
    /* A faint solid underline blends at half coverage. */
    struct shot s = draw_row("\033[4m    \033[2m    ", false);
    uint32_t half = blend_px(s.fg, s.bg, 128);
    int y0, y1;
    TCHECK(ink_rows(&s, 0, &y0, &y1) > 0, "underline");
    for (int y = y0; y < y1; y++) {
        TCHECK(row_solid(&s, 0, y, s.fg), "normal underline row %d", y);
        TCHECK(row_solid(&s, 4, y, half), "faint underline row %d is not at half coverage", y);
    }
    free(s.px);

    /* A faint glyph: every pixel is the glyph at half coverage. */
    s = draw_row("A \033[2mA", false);
    glyph_cache_refresh(font);
    const struct glyph *g = glyph_cache_fetch(font, 'A', 0);
    TCHECK(g && g->row_masks && !g->has_subpx_ink, "glyph");
    int inked = 0;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            int gx = x - g->x_offset;
            uint8_t a = gx >= 0 && gx < g->width && y < g->height ? g->alpha[y][gx] : 0;
            TCHECK(at(&s, 0, x, y) == blend_px(s.fg, s.bg, a), "plain A at (%d, %d)", x, y);
            TCHECK(at(&s, 2, x, y) == blend_px(s.fg, s.bg, (uint8_t)((a + 1) / 2)),
                   "faint A at (%d, %d)", x, y);
            inked += a != 0;
        }
    TCHECK(inked, "A has ink");
    free(s.px);
}

/* Wide glyph halves: the underline runs on through both halves. */
static void test_wide(void)
{
    struct shot s = draw_row("\033[4:3m日本  ", false);
    struct shot b = draw_row("\033[4:3m    ", false);
    int y0, y1;
    TCHECK(ink_rows(&b, 0, &y0, &y1) > 0, "curly");
    /* Below the glyph the pixels are the bare curly rows. */
    for (int col = 0; col < 6; col++)
        for (int y = 0; y < ch; y++) {
            bool glyph_row = false;
            for (int x = 0; x < cw; x++)
                glyph_row |= at(&s, col, x, y) != at(&b, col, x, y);
            if (glyph_row)
                continue;
            for (int x = 0; x < cw; x++)
                TCHECK(at(&s, col, x, y) == at(&b, 0, x, y), "wide: cell %d row %d", col, y);
        }
    free(s.px);
    free(b.px);
}

/* The cursor marks and the cell around them. */
static void test_cursor(void)
{
    static const struct {
        const char *style;
        bool bar;
    } marks[] = { { "\033[6 q", true }, { "\033[4 q", false } };
    const char *const texts[] = { "\033[HM", "\033[H\033[2mM", "\033[H\033[4:3mM", "\033[H " };
    for (int m = 0; m < 2; m++) {
        for (unsigned t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            char text[64];
            snprintf(text, sizeof(text), "%s%s", marks[m].style, texts[t]);
            /* The cursor sits after the text: move it back onto the cell. */
            char with[80];
            snprintf(with, sizeof(with), "%s\033[1;1H", text);
            struct shot plain = draw_row(with, false);
            struct shot cur = draw_row(with, true);
            int marked = 0, kept = 0;
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) {
                    uint32_t p = at(&cur, 0, x, y), q = at(&plain, 0, x, y);
                    if (p == q) {
                        kept++;
                        continue;
                    }
                    /* A changed pixel is the mark, at full strength. */
                    TCHECK(p == cur.cursor, "cursor %d text %u: (%d, %d) is %08x", m, t, x, y, p);
                    marked++;
                }
            TCHECK(marked > 0 && kept > 0, "cursor %d text %u: %d marked", m, t, marked);
            /* The mark is a band of whole columns at the left (bar) or of
             * whole rows at the bottom (underline). */
            int n = marks[m].bar ? cw : ch, band = 0;
            for (int i = 0; i < n; i++) {
                int full = 0, changed = 0, len = marks[m].bar ? ch : cw;
                for (int j = 0; j < len; j++) {
                    int x = marks[m].bar ? i : j, y = marks[m].bar ? j : i;
                    full += at(&cur, 0, x, y) == cur.cursor;
                    changed += at(&cur, 0, x, y) != at(&plain, 0, x, y);
                }
                bool marked_line = full == len;
                if (marked_line)
                    band++;
                else
                    TCHECK(!changed, "cursor %d text %u: line %d is cut", m, t, i);
            }
            TCHECK(band > 0 && band <= n / 2, "cursor %d: band of %d lines", m, band);
            if (marks[m].bar)
                for (int x = 0; x < band; x++)
                    TCHECK(at(&cur, 0, x, ch / 2) == cur.cursor, "bar: not at the left");
            else
                for (int y = ch - band; y < ch; y++)
                    TCHECK(at(&cur, 0, 0, y) == cur.cursor, "underline: not at the bottom");
            /* The other cells do not change. */
            for (int c = 1; c < COLS; c++)
                for (int y = 0; y < ch; y++)
                    for (int x = 0; x < cw; x++)
                        TCHECK(at(&cur, c, x, y) == at(&plain, c, x, y), "cursor leaks into cell %d", c);
            free(plain.px);
            free(cur.px);
        }
    }
    /* A block cursor: the cell takes the cursor color as its bg. */
    struct shot b = draw_row("\033[2 q\033[1;1H", true);
    TCHECK(at(&b, 0, 0, 0) == b.cursor && at(&b, 1, 0, 0) == b.bg, "block cursor");
    free(b.px);
}

int main(void)
{
    TCHECK(render_init(), "render_init");
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    rend = renderer_create(NULL);
    TCHECK(rend, "renderer");
    struct font_instance *big = font_instance_acquire(
        font_instance_size_pt(font_instance_default()), 180);
    struct font_instance *fonts[] = { font_instance_default(), big };
    for (int i = 0; i < 2; i++) {
        font = fonts[i];
        TCHECK(font, "font %d", i);
        cw = font_instance_metrics(font)->cell_w;
        ch = font_instance_metrics(font)->cell_h;
        test_underline_kinds();
        test_strike_overline_link();
        test_colors();
        test_faint();
        test_wide();
        test_cursor();
        printf("decorations: cell %dx%d ok\n", cw, ch);
    }
    font_instance_unref(big);
    renderer_destroy(rend);
    render_shutdown();
    puts("PASS: decorations and cursor marks");
    return 0;
}
