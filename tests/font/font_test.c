#define _GNU_SOURCE
#include "font/font.h"
#include "font/cell_draw.h"
#include "font/glyph_cache.h"
#include "font/backends/freetype/internal.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "font/glyph/intern.h"
#include <wsap_vtcore/unicode/unicode.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static struct font_instance *scaled;

static struct font_instance *cur(void)
{
    return scaled ? scaled : font_instance_default();
}

/* The golden hashes, tiles, and cell widths expect slight hinting and
 * grayscale antialiasing. Set them here, so the host fontconfig settings
 * do not change the results. */
static void use_reference_rendering(void)
{
    font_set_hint_style(FONT_HINT_SLIGHT);
    font_set_aa_mode(FONT_AA_GRAYSCALE);
}

static bool init_fonts(void)
{
    if (!font_backend_init())
        return false;
    struct font_instance *fi = cur();
    glyph_cache_rebuild(fi);
    font_bind_thread(fi);
    return true;
}

static int checks, failures;

static void check(const char *what, bool ok)
{
    checks++;
    if (!ok) {
        printf("FAIL  %s\n", what);
        failures++;
    }
}

#define ZWJ          "\xe2\x80\x8d"
#define VS15         "\xef\xb8\x8e"
#define VS16         "\xef\xb8\x8f"
#define TONE_LIGHT   "\xf0\x9f\x8f\xbb"
#define TONE_MEDIUM  "\xf0\x9f\x8f\xbd"
#define TONE_DARK    "\xf0\x9f\x8f\xbf"
#define THUMBSUP     "\xf0\x9f\x91\x8d"
#define MAN          "\xf0\x9f\x91\xa8"
#define WOMAN        "\xf0\x9f\x91\xa9"
#define GIRL         "\xf0\x9f\x91\xa7"
#define BOY          "\xf0\x9f\x91\xa6"
#define FARMER       "\xf0\x9f\x8c\xbe"
#define POLICE       "\xf0\x9f\x91\xae"
#define FEMALE_SIGN  "\xe2\x99\x80"
#define RI_T         "\xf0\x9f\x87\xb9"
#define RI_R         "\xf0\x9f\x87\xb7"
#define RI_D         "\xf0\x9f\x87\xa9"
#define RI_E         "\xf0\x9f\x87\xaa"
#define KEYCAP       "\xe2\x83\xa3"
#define BLACK_FLAG   "\xf0\x9f\x8f\xb4"
#define TAG_G        "\xf3\xa0\x81\xa7"
#define TAG_B        "\xf3\xa0\x81\xa2"
#define TAG_E        "\xf3\xa0\x81\xa5"
#define TAG_N        "\xf3\xa0\x81\xae"
#define TAG_S        "\xf3\xa0\x81\xb3"
#define TAG_C        "\xf3\xa0\x81\xa3"
#define TAG_END      "\xf3\xa0\x81\xbf"
#define TAG_T        "\xf3\xa0\x81\xb4"
#define ENGLAND      BLACK_FLAG TAG_G TAG_B TAG_E TAG_N TAG_G TAG_END
#define SCOTLAND     BLACK_FLAG TAG_G TAG_B TAG_S TAG_C TAG_T TAG_END
#define COMB_ACUTE   "\xcc\x81"
#define COMB_GRAVE   "\xcc\x80"
#define NOTEHEAD     "\xf0\x9d\x85\x98"
#define COMB_STEM    "\xf0\x9d\x85\xa5"

#define MAX_COLUMNS 24

struct column {
    uint32_t cp;
    uint16_t flags;
    uint8_t width;
    bool color;
};

struct shaped {
    int cols;
    struct column col[MAX_COLUMNS];
    uint64_t ink;
    bool color;
    unsigned luma;
};

struct font_cell {
    uint32_t cp;
    uint16_t style;
    uint8_t width;
};

static struct font_cell font_cell_of(const struct wsap_vtcore_snapshot *snap,
                                     const struct wsap_vtcore_cell *cell)
{
    struct font_cell fc = { .cp = cell->text, .width = cell->width };
    if (cell->text & WSAP_VTCORE_TEXT_CLUSTER) {
        uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
        int n = wsap_vtcore_snapshot_cluster(snap, cell->text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
        assert(n > 0);
        fc.cp = glyph_intern(cps, n < WSAP_VTCORE_GRAPHEME_MAX_LEN ? n : WSAP_VTCORE_GRAPHEME_MAX_LEN);
    }
    if (cell->attrs & WSAP_VTCORE_ATTR_BOLD) fc.style |= FONT_STYLE_BOLD;
    if (cell->attrs & WSAP_VTCORE_ATTR_ITALIC) fc.style |= FONT_STYLE_ITALIC;
    if (cell->attrs & WSAP_VTCORE_ATTR_EMOJI) fc.style |= FONT_STYLE_EMOJI;
    if (cell->attrs & WSAP_VTCORE_ATTR_TEXT) fc.style |= FONT_STYLE_TEXT;
    if (cell->width == 2) fc.style |= FONT_STYLE_WIDE;
    return fc;
}

static int term_font_cells(const char *utf8, int width, struct font_cell *out, int cap)
{
    struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = width, .y = 2 },
    });
    assert(t);
    wsap_vtcore_feed(t, utf8, strlen(utf8), 0);
    int cols = wsap_vtcore_cursor(t).pos.x;
    assert(cols <= cap);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap && wsap_vtcore_snapshot_update(snap, t));
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, 0);
    assert(row);
    for (int c = 0; c < cols; c++)
        out[c] = font_cell_of(snap, &row[c]);
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
    return cols;
}

static struct shaped shape(const char *utf8)
{
    struct shaped s = {0};
    struct font_cell cells[MAX_COLUMNS];
    s.cols = term_font_cells(utf8, MAX_COLUMNS, cells, MAX_COLUMNS);

    uint64_t h = UINT64_C(1469598103934665603);
    unsigned long luma_sum = 0, lit = 0;
    for (int c = 0; c < s.cols; c++) {
        const struct font_cell *cell = &cells[c];
        s.col[c].cp = cell->cp;
        s.col[c].flags = cell->style;
        s.col[c].width = cell->width;
        if (cell->cp <= ' ')
            continue;

        struct glyph g = {0};
        font_instance_render_glyph(cur(), cell->cp, cell->style, &g);
        s.col[c].color = g.has_color;
        s.color |= g.has_color;
        h = (h ^ g.width) * UINT64_C(1099511628211);
        h = (h ^ (uint8_t)g.x_offset) * UINT64_C(1099511628211);
        for (int y = 0; y < GLYPH_CELL_H; y++)
            for (int x = 0; x < g.width; x++) {
                uint32_t px = g.has_color ? *glyph_color_at(&g, y, x) : g.alpha[y][x];
                h = (h ^ px) * UINT64_C(1099511628211);
                if (!g.has_color || !(px >> 24))
                    continue;

                luma_sum += (((px >> 16) & 255) * 54 + ((px >> 8) & 255) * 183 +
                             (px & 255) * 19) >> 8;
                lit++;
            }
        glyph_release(&g);
    }
    s.ink = h;
    s.luma = lit ? (unsigned)(luma_sum / lit) : 0;
    return s;
}

static struct glyph unknown;

static uint64_t row_hash(uint64_t h, const struct glyph *g, int y)
{
    for (int w = 0; w < g->mask_words; w++)
        h = (h ^ glyph_mask(g, y)[w]) * UINT64_C(1099511628211);
    return h;
}

static bool is_unknown(const struct glyph *g)
{
    return g->mask_words == unknown.mask_words &&
           !memcmp(g->row_masks, unknown.row_masks,
                   (size_t)GLYPH_CELL_H * g->mask_words * sizeof(*g->row_masks));
}

static void ink_box(const struct glyph *g, int *w, int *h)
{
    int top = -1, bottom = -1, left = INT_MAX, right = -1;
    for (int y = 0; y < GLYPH_CELL_H; y++) {
        if (!glyph_row_inked(g, y))
            continue;
        if (top < 0)
            top = y;
        bottom = y;
        for (int x = 0; x < g->width; x++)
            if (glyph_inked(g, y, x)) {
                if (x < left) left = x;
                if (x > right) right = x;
            }
    }
    *w = right - left + 1;
    *h = bottom - top + 1;
}

static bool same_aspect(int w1, int h1, int w2, int h2)
{
    return w1 > 0 && h1 > 0 && w2 > 0 && h2 > 0 &&
           (w1 - 1) * (h2 - 1) * 4 <= (w2 + 1) * (h1 + 1) * 5 &&
           (w2 - 1) * (h1 - 1) * 4 <= (w1 + 1) * (h2 + 1) * 5;
}

static bool near_square(int w, int h)
{
    return same_aspect(w, h, 1, 1);
}

static void ink_boxes(uint32_t cp, int *nw, int *nh, int *ww, int *wh)
{
    struct glyph narrow = {0}, wide = {0};
    font_instance_render_glyph(cur(), cp, 0, &narrow);
    font_instance_render_glyph(cur(), cp, FONT_STYLE_WIDE, &wide);
    ink_box(&narrow, nw, nh);
    ink_box(&wide, ww, wh);
    glyph_release(&narrow);
    glyph_release(&wide);
}

static bool column_inked(const struct glyph *g, int x)
{
    for (int y = 0; y < GLYPH_CELL_H; y++)
        if (glyph_inked(g, y, x))
            return true;
    return false;
}

static void test_raster_invariants(void)
{
    static const struct { uint32_t cp; uint16_t flags; const char *name; } samples[] = {
        { 'A', 0, "A" },
        { 'A', FONT_STYLE_ITALIC, "italic A" },
        { 0x00E9, 0, "e-acute" },
        { 0x250C, 0, "box drawing" },
        { 0x2588, 0, "full block" },
        { 0x2591, 0, "light shade" },
        { 0x276F, 0, "heavy angle bracket" },
        { 0x279C, 0, "heavy round arrow" },
        { 0x4E2D, FONT_STYLE_WIDE, "CJK" },
        { 0x1F642, FONT_STYLE_WIDE, "smiley" },
        { 0x1F3FD, FONT_STYLE_WIDE, "skin tone modifier" },
        { 0x1F1F9, 0, "regional indicator" },
        { 0x20E3, 0, "enclosing keycap" },
        { 0x0301, 0, "combining acute" },
        { 0x1D165, 0, "combining musical stem" },
        { 0x00A9, FONT_STYLE_EMOJI | FONT_STYLE_WIDE, "emoji-presentation copyright" },
        { 0x00A9, FONT_STYLE_TEXT, "text-presentation copyright" },
        { 0x10FFFF, 0, "unassigned" },
    };

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        struct glyph g = {0};
        char label[96];
        snprintf(label, sizeof(label), "%s renders", samples[i].name);
        check(label, font_instance_render_glyph(cur(), samples[i].cp, samples[i].flags, &g));

        int allocated = GLYPH_CELL_W * ((samples[i].flags & FONT_STYLE_WIDE) ? 2 : 1);
        bool masks_agree = true, contained = g.mask_words == (g.width + 63) / 64;
        for (int y = 0; y < GLYPH_CELL_H; y++) {
            for (int x = 0; x < g.mask_words * 64; x++) {
                bool visible = x < g.width &&
                    (g.has_color ? (*glyph_color_at(&g, y, x) >> 24) != 0
                                 : g.alpha[y][x] != 0);
                if (glyph_inked(&g, y, x) != visible)
                    masks_agree = false;
            }
        }
        snprintf(label, sizeof(label), "%s row masks match its pixels", samples[i].name);
        check(label, masks_agree);
        snprintf(label, sizeof(label), "%s ink fits its stored raster around %d columns",
                 samples[i].name, allocated / GLYPH_CELL_W);
        check(label, contained);
        bool tight = (g.x_offset >= 0 || column_inked(&g, 0)) &&
                     (g.x_offset + g.width <= allocated || column_inked(&g, g.width - 1));
        snprintf(label, sizeof(label), "%s reaches past its cell only with ink",
                 samples[i].name);
        check(label, tight);
        glyph_release(&g);
    }
}

static bool font_covers(uint32_t cp)
{
    FcPattern *pat = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    bool covered = false;
    if (pat && cs) {
        FcCharSetAddChar(cs, cp);
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_FAMILY, NULL);
        FcFontSet *fs = os ? FcFontList(NULL, pat, os) : NULL;
        for (int i = 0; fs && i < fs->nfont && !covered; i++) {
            FcChar8 *family = NULL;
            FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &family);
            covered = !family || strcmp((const char *)family, ".LastResort");
        }
        if (fs)
            FcFontSetDestroy(fs);
        if (os)
            FcObjectSetDestroy(os);
    }
    if (cs)
        FcCharSetDestroy(cs);
    if (pat)
        FcPatternDestroy(pat);
    return covered;
}

static uint64_t glyph_ink_hash(const struct glyph *g);

static void test_coverage(void)
{
    struct glyph regular = {0}, italic = {0}, cjk = {0}, emoji = {0},
                keycap = {0}, tone = {0}, stem = {0};

    check("italic styling alters the outline",
          font_instance_render_glyph(cur(), 'A', 0, &regular) &&
          font_instance_render_glyph(cur(), 'A', FONT_STYLE_ITALIC, &italic) &&
          memcmp(regular.row_masks, italic.row_masks,
                 (size_t)GLYPH_CELL_H * regular.mask_words *
                 sizeof(*regular.row_masks)) != 0);

    static const uint16_t variants[] = {
        FONT_STYLE_BOLD, FONT_STYLE_ITALIC, FONT_STYLE_BOLD | FONT_STYLE_ITALIC,
    };
    uint64_t variant_ink[3] = {0};
    bool variants_render = true;
    for (int i = 0; i < 3; i++) {
        struct glyph g = {0};
        variants_render &= font_instance_render_glyph(font_instance_default(), 'A',
                                                      variants[i], &g);
        variant_ink[i] = glyph_ink_hash(&g);
        glyph_release(&g);
    }
    check("bold, italic, and bold italic A each look different",
          variants_render && variant_ink[0] != variant_ink[1] &&
          variant_ink[0] != variant_ink[2] && variant_ink[1] != variant_ink[2]);

    check("CJK resolves to a real glyph",
          font_instance_render_glyph(cur(), 0x4E2D, FONT_STYLE_WIDE, &cjk) && !is_unknown(&cjk));
    check("color emoji resolves to a color strike",
          font_instance_render_glyph(cur(), 0x1F600, FONT_STYLE_WIDE, &emoji) && emoji.has_color);

    check("skin-tone modifier has a glyph of its own",
          font_instance_render_glyph(cur(), 0x1F3FD, FONT_STYLE_WIDE, &tone) && !is_unknown(&tone));
    check("combining enclosing keycap has a glyph of its own",
          font_instance_render_glyph(cur(), 0x20E3, 0, &keycap) && !is_unknown(&keycap));
    font_instance_render_glyph(cur(), 0x1D165, 0, &stem);
    if (font_covers(0x1D165))
        check("combining musical stem has a glyph of its own",
              !is_unknown(&stem));
    else
        printf("SKIP  no font covering U+1D165 installed\n");

    glyph_release(&regular);
    glyph_release(&italic);
    glyph_release(&cjk);
    glyph_release(&emoji);
    glyph_release(&keycap);
    glyph_release(&tone);
    glyph_release(&stem);
}

static void test_presentation(void)
{
    struct shaped emoji_default = shape(THUMBSUP);
    check("a default-emoji codepoint takes two columns", emoji_default.cols == 2);
    check("a default-emoji codepoint renders in color", emoji_default.color);

    struct shaped vs16 = shape("\xc2\xa9" VS16);
    check("VS16 widens a text-default character to two columns", vs16.cols == 2);
    check("VS16 selects the color strike", vs16.color);

    struct shaped vs15 = shape("\xe2\x98\xba" VS15);
    check("VS15 keeps a single column", vs15.cols == 1);
    check("VS15 suppresses the color strike", !vs15.color);

    check("ASCII stays monochrome in one column",
          shape("A").cols == 1 && !shape("A").color);

    check("ZWJ consumes no column", shape("A" ZWJ "B").cols == 2);
    check("a skin-tone modifier consumes no column",
          shape(THUMBSUP TONE_DARK).cols == 2);
    check("tag characters consume no columns", shape(ENGLAND).cols == 2);
    check("combining marks consume no column", shape("e" COMB_ACUTE).cols == 1);
}

static void test_color_aspect_ratio(void)
{
    static const uint32_t emoji[] = { 0x1F600, 0x1F44D, 0x1F308 };
    for (size_t i = 0; i < sizeof(emoji) / sizeof(emoji[0]); i++) {
        int nw, nh, ww, wh;
        ink_boxes(emoji[i], &nw, &nh, &ww, &wh);

        char label[96];
        snprintf(label, sizeof(label), "U+%04X keeps its aspect in either allocation",
                 emoji[i]);
        check(label, same_aspect(nw, nh, ww, wh));

        struct glyph wide = {0};
        font_instance_render_glyph(cur(), emoji[i], FONT_STYLE_WIDE, &wide);
        snprintf(label, sizeof(label), "U+%04X fills its two columns", emoji[i]);
        check(label, wide.width > GLYPH_CELL_W);
        glyph_release(&wide);
    }
}

static void test_music(void)
{

    static const uint32_t music[] = {
        0x1d13a, 0x1d13b, 0x1d13c, 0x1d13d, 0x1d13e, 0x1d13f,
        0x1d140, 0x1d141, 0x1d142, 0x1d15c, 0x1d15d, 0x1d157,
        0x1d158, 0x1d165, 0x1d16e, 0x1d16f, 0x1d170, 0x1d171, 0x1d172,
    };
    size_t covered = 0;
    for (size_t i = 0; i < sizeof(music) / sizeof(music[0]); i++)
        covered += font_covers(music[i]);
    if (covered != sizeof(music) / sizeof(music[0])) {
        printf("SKIP  musical symbols lack font coverage (%zu/%zu)\n",
               covered, sizeof(music) / sizeof(music[0]));
        return;
    }
    bool all_visible = true, all_covered = true;
    for (size_t i = 0; i < sizeof(music) / sizeof(music[0]); i++) {
        struct glyph note = {0};
        font_instance_render_glyph(cur(), music[i], 0, &note);
        if (is_unknown(&note))
            all_covered = false;
        bool visible = false;
        for (int y = 0; y < GLYPH_CELL_H; y++)
            visible |= glyph_row_inked(&note, y);
        if (!visible)
            all_visible = false;
        glyph_release(&note);
    }
    check("musical symbols are found by coverage", all_covered);
    check("musical symbols draw ink", all_visible);
}

static void test_skin_tone(void)
{
    struct shaped plain = shape(THUMBSUP);
    struct shaped light = shape(THUMBSUP TONE_LIGHT);
    struct shaped medium = shape(THUMBSUP TONE_MEDIUM);
    struct shaped dark = shape(THUMBSUP TONE_DARK);

    check("a toned emoji still occupies exactly two columns",
          light.cols == 2 && medium.cols == 2 && dark.cols == 2);

    check("a skin-tone modifier changes the pixels drawn", plain.ink != medium.ink);
    check("different skin tones render differently", light.ink != dark.ink);
    check("a lighter tone renders lighter than a darker one", light.luma > dark.luma);
    check("a toned person emoji differs from the untoned one",
        shape(MAN).ink != shape(MAN TONE_DARK).ink);
}

static void test_zwj_sequences(void)
{
    check("man + ZWJ + ear of rice is one two-column glyph",
        shape(MAN ZWJ FARMER).cols == 2);
    check("a four-person family is one two-column glyph",
        shape(MAN ZWJ WOMAN ZWJ GIRL ZWJ BOY).cols == 2);
    check("a gendered ZWJ sequence is one two-column glyph",
        shape(POLICE ZWJ FEMALE_SIGN VS16).cols == 2);
    check("a joined sequence differs from its first component",
        shape(MAN ZWJ WOMAN ZWJ GIRL ZWJ BOY).ink != shape(MAN).ink &&
        shape(MAN ZWJ WOMAN ZWJ GIRL ZWJ BOY).cols == shape(MAN).cols);
}

static void test_regional_flags(void)
{
    struct shaped flag = shape(RI_T RI_R);
    check("a regional-indicator pair spans two columns", flag.cols == 2);
    check("a regional-indicator pair forms one wide cluster",
        flag.col[0].width == 2 && flag.col[1].width == 0);
    check("two flags side by side stay one cluster each",
        shape(RI_T RI_R RI_D RI_E).col[2].width == 2);

    check("an unpaired regional indicator takes one column", shape(RI_T).cols == 1);
}

static void test_keycaps(void)
{
    struct shaped keycap = shape("1" VS16 KEYCAP);
    check("a keycap sequence occupies two columns", keycap.cols == 2);
    check("the enclosing keycap is drawn around the digit",
        keycap.ink != shape("1" VS16).ink);
    check("different keycap digits render differently",
        shape("1" VS16 KEYCAP).ink != shape("2" VS16 KEYCAP).ink &&
        shape("1" VS16 KEYCAP).ink != shape("1" VS16).ink);
}

static void test_tag_sequences(void)
{
    check("a tag sequence occupies two columns", shape(ENGLAND).cols == 2);
    check("a tag sequence differs from the bare black flag",
        shape(ENGLAND).ink != shape(BLACK_FLAG).ink);
    check("different subdivision flags render differently",
        shape(ENGLAND).ink != shape(SCOTLAND).ink);
}

static void test_combining_marks(void)
{
    check("a combining mark shares its base character's column",
          shape("e" COMB_ACUTE).cols == 1);
    check("a combining acute draws an accent", shape("e" COMB_ACUTE).ink != shape("e").ink);
    check("stacked combining marks each draw",
        shape("a" COMB_GRAVE COMB_ACUTE).ink != shape("a" COMB_GRAVE).ink);

    check("a notehead and its combining stem share one column",
        shape(NOTEHEAD COMB_STEM).cols == 1);
}

static void test_single_column_color(void)
{
    static const struct { const char *utf8; const char *name; } narrow[] = {
        { "\xe2\x98\xba", "U+263A white smiling face" },
        { "\xe2\x9d\xa4", "U+2764 heavy black heart" },
        { "\xe2\x9c\x94", "U+2714 heavy check mark" },
        { RI_T, "U+1F1F9 regional indicator T" },
    };
    for (size_t i = 0; i < sizeof(narrow) / sizeof(narrow[0]); i++) {
        struct shaped s = shape(narrow[i].utf8);
        char label[128];
        snprintf(label, sizeof(label), "%s is not a color strike in one column",
                 narrow[i].name);
        check(label, !(s.cols == 1 && s.col[0].color));
    }
}

static void test_emoji_text_height(void)
{
    const char info[] = "(\xe2\x84\xb9" VS16 ")";
    struct font_cell cells[MAX_COLUMNS];
    int cols = term_font_cells(info, MAX_COLUMNS, cells, MAX_COLUMNS);
    assert(cols == 4);

    struct glyph left = {0}, emoji = {0}, right = {0};
    font_instance_render_glyph(cur(), cells[0].cp, cells[0].style, &left);
    font_instance_render_glyph(cur(), cells[1].cp, cells[1].style, &emoji);
    font_instance_render_glyph(cur(), cells[3].cp, cells[3].style, &right);
    int lw, lh, ew, eh, rw, rh;
    ink_box(&left, &lw, &lh);
    ink_box(&emoji, &ew, &eh);
    ink_box(&right, &rw, &rh);
    check("information emoji keeps the parentheses' optical height",
          emoji.has_color && abs(eh - lh) <= 1 && abs(eh - rh) <= 1);
    glyph_release(&left);
    glyph_release(&emoji);
    glyph_release(&right);
}

static void ink_margins(const struct glyph *g, int allocated,
                        int *left, int *right, int *top, int *bottom)
{
    int t = -1, b = -1, l = INT_MAX, r = -1;
    for (int y = 0; y < GLYPH_CELL_H; y++) {
        if (!glyph_row_inked(g, y))
            continue;
        if (t < 0)
            t = y;
        b = y;
        for (int x = 0; x < g->width; x++)
            if (glyph_inked(g, y, x)) {
                if (x < l) l = x;
                if (x > r) r = x;
            }
    }
    *left = g->x_offset + l;
    *right = allocated - 1 - (g->x_offset + r);
    *top = t;
    *bottom = GLYPH_CELL_H - 1 - b;
}

static void test_ink_centering(void)
{
    static const uint32_t emoji[] = { 0x1F600, 0x1F44D, 0x1F308 };
    for (size_t i = 0; i < sizeof(emoji) / sizeof(emoji[0]); i++) {
        struct glyph g = {0};
        font_instance_render_glyph(cur(), emoji[i], FONT_STYLE_WIDE, &g);
        int left, right, top, bottom;
        ink_margins(&g, 2 * GLYPH_CELL_W, &left, &right, &top, &bottom);
        glyph_release(&g);

        char label[96];
        snprintf(label, sizeof(label), "U+%04X is centred across its two columns",
                 emoji[i]);
        check(label, left - right <= 1 && right - left <= 1);
        snprintf(label, sizeof(label), "U+%04X is centred vertically in its cell",
                 emoji[i]);
        check(label, top - bottom <= 2 && bottom - top <= 2);
    }

    static const uint32_t text[] = { 'M', 'H', '0' };
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        struct glyph g = {0};
        font_instance_render_glyph(cur(), text[i], 0, &g);
        int left, right, top, bottom;
        ink_margins(&g, GLYPH_CELL_W, &left, &right, &top, &bottom);

        char label[96];
        snprintf(label, sizeof(label), "'%c' is centred horizontally in its cell",
                 (char)text[i]);
        check(label, left - right <= 1 && right - left <= 1);
        snprintf(label, sizeof(label), "'%c' is centred vertically in its cell",
                 (char)text[i]);
        check(label, top - bottom <= 1 && bottom - top <= 1);
        glyph_release(&g);
    }
}

static void test_outline_aspect_ratio(void)
{

    for (uint32_t cp = 0x1FA60; cp <= 0x1FA6D; cp++) {
        int nw, nh, ww, wh;
        ink_boxes(cp, &nw, &nh, &ww, &wh);
        char label[96];

        snprintf(label, sizeof(label), "U+%04X is a square glyph given the room", cp);
        check(label, near_square(ww, wh));

        snprintf(label, sizeof(label), "U+%04X stays square in one column", cp);
        check(label, near_square(nw, nh));
    }

    int dw, dh, dww, dwh;
    ink_boxes(0x1F030, &dw, &dh, &dww, &dwh);
    check("U+1F030 keeps its aspect in either allocation",
        same_aspect(dw, dh, dww, dwh));

    int cw, ch, cww, cwh;
    ink_boxes(0x25CF, &cw, &ch, &cww, &cwh);
    check("U+25CF black circle stays round in one column",
        abs(cw - ch) <= 1 && same_aspect(cw, ch, cww, cwh));
    struct glyph circle = {0};
    font_instance_render_glyph(cur(), 0x25CF, 0, &circle);
    int cl, cr, ct, cb;
    ink_margins(&circle, GLYPH_CELL_W, &cl, &cr, &ct, &cb);
    check("U+25CF black circle keeps its horizontal overhang", cl < 0 || cr < 0);
    glyph_release(&circle);

    int nw, nh, ww, wh;
    ink_boxes('M', &nw, &nh, &ww, &wh);
    check("ASCII is not rescaled by its allocation", nw == ww && nh == wh);
}

static int utf8_of(uint32_t cp, char out[5])
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 63)); out[2] = 0; return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63)); out[3] = 0; return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63));
    out[3] = (char)(0x80 | (cp & 63)); out[4] = 0; return 4;
}

static uint64_t render_after(uint32_t target, uint32_t lo, uint32_t hi)
{
    font_backend_shutdown();
    init_fonts();
    struct glyph g = {0};
    for (uint32_t cp = lo; cp <= hi; cp++)
        font_instance_render_glyph(cur(), cp, 0, &g);
    font_instance_render_glyph(cur(), target, 0, &g);

    uint64_t h = UINT64_C(1469598103934665603);
    for (int y = 0; y < GLYPH_CELL_H; y++)
        h = row_hash(h, &g, y);
    glyph_release(&g);
    return h;
}

static void test_fallback_order_independence(void)
{
    const uint32_t target = 0x1F0A1;
    uint64_t alone = render_after(target, 1, 0);
    uint64_t after_dominoes = render_after(target, 0x1F030, 0x1F093);
    uint64_t after_xiangqi = render_after(target, 0x1FA60, 0x1FA6D);

    check("U+1F0A1 renders the same however it was reached",
          alone == after_dominoes && alone == after_xiangqi);

    font_backend_shutdown();
    init_fonts();
}

static struct glyph drawn(uint32_t cp)
{
    struct glyph g = {0};
    g.width = GLYPH_CELL_W;
    check("aligned-symbol drawing supports the tested code point",
          draw_cell_aligned_symbol(cp, &g));
    return g;
}

static void count_shaped_cells(void *ctx, const struct glyph_span *span)
{
    *(int *)ctx += span->count;
}

static int shaped_cells(const uint32_t *cps, int n, uint16_t style)
{
    int shaped = 0;
    glyph_cache_shape_segment(cur(), cps, n, style, count_shaped_cells, &shaped);
    return shaped;
}

static void test_programming_ligatures(void)
{
    const uint32_t arrow[] = { '-', '>' };
    check("the configured font shapes -> as a two-cell ligature",
          shaped_cells(arrow, 2, 0) == 2);
    const uint32_t letters[] = { 'a', 'b' };
    check("ordinary adjacent letters are not treated as a ligature",
          shaped_cells(letters, 2, 0) == 0);
    const uint32_t equals[] = { '=', '=', '=' };
    check("a three-cell run can be shaped for ===", shaped_cells(equals, 3, 0) == 3);
    uint32_t separator[128];
    for (int i = 0; i < 128; i++)
        separator[i] = '=';
    const int lengths[] = { 4, 5, 6, 7, 8, 34, 66, 128 };
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
        check("equals separators keep separate glyphs",
              shaped_cells(separator, lengths[i], 0) == 0);
    const uint32_t arabic[] = { 0x0633, 0x0644, 0x0627, 0x0645 };
    check("an Arabic word is shaped as a contextual cell run",
          shaped_cells(arabic, 4, 0) == 4);
}

static void test_run_with_space_shapes(void)
{
    static const struct {
        const char *text;
        uint32_t cps[24];
        int n;
    } cases[] = {
        { "// comment here", { '/', '/', ' ', 'c', 'o', 'm', 'm', 'e', 'n', 't',
                              ' ', 'h', 'e', 'r', 'e' }, 15 },
        { "-> x", { '-', '>', ' ', 'x' }, 4 },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
        check("a run with a space shapes through the run cache",
              shaped_cells(cases[c].cps, cases[c].n, 0) > 0);
}

struct bump_in_emit {
    struct font_instance *fi;
    int cells;
    bool intact;
};

static size_t span_alpha_bytes(const struct glyph_span *span)
{
    size_t n = 0;
    for (int i = 0; i < span->count; i++)
        if (span->cells[i].alpha)
            n += (size_t)glyph_rows(&span->cells[i]) * span->cells[i].width;
    return n;
}

static void copy_span_alpha(const struct glyph_span *span, uint8_t *out)
{
    for (int i = 0; i < span->count; i++) {
        const struct glyph *g = &span->cells[i];
        if (!g->alpha)
            continue;
        for (int y = 0; y < glyph_rows(g); y++, out += g->width)
            memcpy(out, g->alpha[y], g->width);
    }
}

static void bump_and_fetch(void *ctx, const struct glyph_span *span)
{
    struct bump_in_emit *b = ctx;
    size_t n = span_alpha_bytes(span);
    uint8_t *before = malloc(n + 1), *after = malloc(n + 1);
    assert(before && after);
    copy_span_alpha(span, before);
    font_bump_generation();
    (void)glyph_cache_fetch(b->fi, 0x9F98 + (uint32_t)b->cells, 0);
    b->intact &= span_alpha_bytes(span) == n;
    if (b->intact) {
        copy_span_alpha(span, after);
        b->intact = memcmp(before, after, n) == 0;
    }
    b->cells += span->count;
    free(before);
    free(after);
}

static void test_font_change_in_emit(void)
{
    const uint32_t cps[] = { '-', '>', ' ', '-', '>', ' ', '-', '>' };
    int n = (int)(sizeof(cps) / sizeof(cps[0]));
    int warm = shaped_cells(cps, n, 0);
    struct bump_in_emit b = { .fi = cur(), .intact = true };
    glyph_cache_shape_segment(cur(), cps, n, 0, bump_and_fetch, &b);
    check("a segment with ligatures emits spans", warm > 0);
    check("a font change in emit keeps the span pixels", b.intact);
    check("a font change in emit lets the segment finish", b.cells == warm);
    glyph_cache_rebuild(cur());
}

#define STRESS_THREADS 4
#define STRESS_ITERS 400

static pthread_barrier_t stress_frame;

static void stress_end_frame(int id)
{
    pthread_barrier_wait(&stress_frame);
    if (id == 0)
        glyph_cache_flush_frame();
    pthread_barrier_wait(&stress_frame);
}

static uint64_t stress_touch(const struct glyph *cells, const uint32_t *tiles,
                             int n)
{
    uint64_t acc = 0;
    for (int i = 0; i < n; i++) {
        acc += cells[i].row_masks ? 1u : 0u;
        if (cells[i].row_masks && cells[i].mask_words > 0)
            acc += cells[i].row_masks[0];
    }
    if (tiles && n > 0)
        acc += tiles[0];
    return acc;
}

struct stress_job {
    struct font_instance *fi;
    int id, iter;
    uint64_t acc;
};

static void stress_hold_span(void *ctx, const struct glyph_span *span)
{
    struct stress_job *job = ctx;
    for (int k = 0; k < 64; k++) {
        for (int i = 0; i < 6; i++) {
            int mix = job->iter * 13 + k * 29 + job->id * 5 + i * 3;
            const struct glyph *g = glyph_cache_fetch(job->fi,
                0x4e00u + (uint32_t)(mix % 0x200), 0);
            const uint32_t *tile = glyph_cache_fetch_tile(job->fi,
                0x400u + (uint32_t)(mix % 0x60), (k & 1) ? FONT_STYLE_BOLD : 0);
            job->acc += (g ? g->width : 0u) + (tile ? tile[0] : 0u);
        }
    }
    job->acc += stress_touch(span->cells, span->tiles, span->count);
}

static void *stress_worker(void *arg)
{
    struct stress_job job = { .fi = font_instance_default(), .id = (int)(uintptr_t)arg };
    for (; job.iter < STRESS_ITERS; job.iter++) {
        int iter = job.iter, id = job.id;
        glyph_cache_flush_deferred();

        uint32_t cps[FONT_RUN_MAX];
        int n = 6 + (iter + id * 7) % 8;
        for (int i = 0; i < n; i++) {
            uint32_t base = 0x400u + (uint32_t)((iter * 31 + id * 17 + i * 13) % 0x60);
            cps[i] = (iter % 3 == 0) ? base : (uint32_t)('a' + ((iter + i) % 26));
        }
        int arrow = (iter + id) % (n - 1);
        cps[arrow] = '-';
        cps[arrow + 1] = '>';
        glyph_cache_shape_segment(job.fi, cps, n, 0, stress_hold_span, &job);
        stress_end_frame(id);
    }
    glyph_cache_thread_detach();
    font_thread_detach();
    return (void *)(uintptr_t)(job.acc & 1u);
}

static void run_cache_survives_concurrent_eviction(void)
{
    check("fonts load for the run cache stress", init_fonts());
    assert(pthread_barrier_init(&stress_frame, NULL, STRESS_THREADS) == 0);
    glyph_cache_set_parallel(true);
    pthread_t th[STRESS_THREADS];
    for (uintptr_t i = 0; i < STRESS_THREADS; i++)
        assert(pthread_create(&th[i], NULL, stress_worker, (void *)i) == 0);
    for (int i = 0; i < STRESS_THREADS; i++)
        assert(pthread_join(th[i], NULL) == 0);
    pthread_barrier_destroy(&stress_frame);
    glyph_cache_set_parallel(false);
    glyph_cache_flush_frame();
    glyph_cache_thread_detach();
    check("the run cache survives concurrent eviction",
          font_instance_default() != NULL);
}

static bool solid(const struct glyph *g, int y, int x)
{
    return g->alpha[y][x] == 255;
}

static void release_glyphs(struct glyph *g, int n)
{
    for (int i = 0; i < n; i++)
        glyph_release(&g[i]);
}

static bool partitions_cell(const uint32_t *cps, int n)
{
    struct glyph g[8] = {0};
    for (int i = 0; i < n; i++)
        g[i] = drawn(cps[i]);

    for (int y = 0; y < GLYPH_CELL_H; y++)
        for (int x = 0; x < GLYPH_CELL_W; x++) {
            int times = 0;
            for (int i = 0; i < n; i++)
                if (solid(&g[i], y, x))
                    times++;
            if (times != 1) {
                release_glyphs(g, n);
                return false;
            }
        }
    release_glyphs(g, n);
    return true;
}

static void test_cell_tiling(void)
{

    struct glyph full = drawn(0x2588);
    bool every_pixel = true;
    for (int y = 0; y < GLYPH_CELL_H; y++)
        for (int x = 0; x < GLYPH_CELL_W; x++)
            if (!solid(&full, y, x))
                every_pixel = false;
    check("a full block covers every pixel of its cell", every_pixel);

    static const struct { const char *what; uint32_t cps[8]; int n; } sets[] = {
        { "halves",          { 0x2580, 0x2584 }, 2 },
        { "left and right",  { 0x258C, 0x2590 }, 2 },
        { "quadrants",       { 0x2598, 0x259D, 0x2596, 0x2597 }, 4 },

        { "eighth columns",  { 0x258F, 0x1FB70, 0x1FB71, 0x1FB72,
                               0x1FB73, 0x1FB74, 0x1FB75, 0x2595 }, 8 },
        { "eighth rows",     { 0x2594, 0x1FB76, 0x1FB77, 0x1FB78,
                               0x1FB79, 0x1FB7A, 0x1FB7B, 0x2581 }, 8 },
        { "sextants",        { 0x1FB00, 0x1FB01, 0x1FB03,
                               0x1FB07, 0x1FB0F, 0x1FB1E }, 6 },

        { "a sextant and its complement", { 0x1FB00, 0x1FB3B }, 2 },
        { "an octant and its complement", { 0x1CD00, 0x1CDE3 }, 2 },
    };
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        char label[96];
        snprintf(label, sizeof(label), "%s cover the cell exactly once", sets[i].what);
        check(label, partitions_cell(sets[i].cps, sets[i].n));
    }

    glyph_release(&full);

    struct glyph lower = drawn(0x2581), upper = drawn(0x2594);
    bool meet = true;
    for (int x = 0; x < GLYPH_CELL_W; x++)
        if (!solid(&lower, GLYPH_CELL_H - 1, x) || !solid(&upper, 0, x))
            meet = false;
    check("bars stacked in neighbouring rows meet at the boundary", meet);

    glyph_release(&lower);
    glyph_release(&upper);

    struct glyph line = drawn(0x2500), cross = drawn(0x253C), bar = drawn(0x2502);
    bool aligned = true;
    for (int y = 0; y < GLYPH_CELL_H; y++)
        if (solid(&line, y, 0) != solid(&cross, y, 0) ||
            solid(&line, y, GLYPH_CELL_W - 1) != solid(&cross, y, GLYPH_CELL_W - 1))
            aligned = false;
    for (int x = 0; x < GLYPH_CELL_W; x++)
        if (solid(&bar, 0, x) != solid(&cross, 0, x) ||
            solid(&bar, GLYPH_CELL_H - 1, x) != solid(&cross, GLYPH_CELL_H - 1, x))
            aligned = false;
    check("a line, a bar and a cross meet on the same pixels", aligned);

    bool spans = false;
    for (int y = 0; y < GLYPH_CELL_H; y++)
        if (solid(&line, y, 0) && solid(&line, y, GLYPH_CELL_W - 1))
            spans = true;
    check("a light horizontal line reaches both edges of its cell", spans);
    glyph_release(&line);
    glyph_release(&cross);
    glyph_release(&bar);

}

static void test_tile_colors(void)
{
    struct font_instance *fi = font_instance_default();
    const struct font_metrics *m = font_instance_metrics(fi);
    size_t bytes = (size_t)m->cell_w * (size_t)m->cell_h * sizeof(uint32_t);
    const uint32_t a_fg = 0xFFFFFFFFu, a_bg = 0xFF1E1E2Eu;
    const uint32_t b_fg = 0xFF203040u, b_bg = 0xFFE8D8C8u;
    static const uint32_t cps[] = { 'A', 0xE9, 0x416, 0x3B1 };
    uint32_t *at_a = malloc(bytes), *at_b = malloc(bytes), *fresh = malloc(bytes);
    if (!at_a || !at_b || !fresh) {
        check("tile color buffers allocate", false);
        free(at_a); free(at_b); free(fresh);
        return;
    }
    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); i++) {
        glyph_cache_begin_frame(fi, a_fg, a_bg);
        const uint32_t *tile = glyph_cache_fetch_tile(fi, cps[i], 0);
        if (!tile) {
            check("an ASCII letter has a default-color tile", cps[i] != 'A');
            continue;
        }
        memcpy(at_a, tile, bytes);

        glyph_cache_begin_frame(fi, b_fg, b_bg);
        tile = glyph_cache_fetch_tile(fi, cps[i], 0);
        check("a tile stays available after a color change", tile != NULL);
        if (!tile)
            continue;
        memcpy(at_b, tile, bytes);

        glyph_cache_rebuild(fi);
        tile = glyph_cache_fetch_tile(fi, cps[i], 0);
        check("a rebuilt cache has the tile", tile != NULL);
        if (!tile)
            continue;
        memcpy(fresh, tile, bytes);

        char what[96];
        snprintf(what, sizeof(what), "U+%04X recolors to the new frame colors", cps[i]);
        check(what, !memcmp(at_b, fresh, bytes));
        snprintf(what, sizeof(what), "U+%04X differs between color pairs", cps[i]);
        check(what, memcmp(at_a, fresh, bytes) != 0);
    }
    glyph_cache_begin_frame(fi, a_fg, a_bg);
    free(at_a); free(at_b); free(fresh);
}

static void set_scale(uint32_t scale_120)
{
    struct font_instance *fi =
        font_instance_acquire(font_instance_size_pt(font_instance_default()), scale_120);
    font_instance_unref(scaled);
    scaled = fi;
    font_bind_thread(fi);
    font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
}

static void test_scaled_cells(void)
{
    int base_w = GLYPH_CELL_W, base_h = GLYPH_CELL_H;
    uint32_t ceiling = 480;

    int last_w = base_w, last_h = base_h;
    for (uint32_t scale = 120; scale <= ceiling; scale += 15) {
        set_scale(scale);
        char what[80];
        snprintf(what, sizeof(what), "cell does not shrink as the scale rises (%u/120)", scale);
        check(what, GLYPH_CELL_W >= last_w && GLYPH_CELL_H >= last_h);

        last_w = GLYPH_CELL_W;
        last_h = GLYPH_CELL_H;
    }

    if (ceiling >= 240) {
        set_scale(240);
        check("doubling the scale approximately doubles the cell width",
              abs(GLYPH_CELL_W - base_w * 2) <= 1);
        check("doubling the scale doubles the cell height",
              abs(GLYPH_CELL_H - base_h * 2) <= 1);
    }

    set_scale(ceiling);
    int ceiling_w = GLYPH_CELL_W, ceiling_h = GLYPH_CELL_H;
    set_scale(ceiling * 3);
    check("a scale past the old 200% ceiling keeps growing the cell",
          GLYPH_CELL_W > ceiling_w && GLYPH_CELL_H > ceiling_h);
    struct glyph big = {0};
    check("a wide glyph past 64 pixels renders with ink",
          GLYPH_CELL_W * 2 > 64 &&
          font_instance_render_glyph(cur(), 0x4E2D, FONT_STYLE_WIDE, &big) &&
          big.mask_words > 1 && glyph_row_inked(&big, GLYPH_CELL_H / 2));
    bool right_half = false;
    for (int y = 0; y < GLYPH_CELL_H; y++)
        for (int x = 64; x < big.width; x++)
            right_half |= glyph_inked(&big, y, x);
    check("ink past column 64 is marked", right_half);
    glyph_release(&big);

    set_scale(120);
    check("returning to an unscaled cell restores it",
          GLYPH_CELL_W == base_w && GLYPH_CELL_H == base_h);
}

static void test_font_sizes(void)
{
    struct font_instance *base = font_instance_default();
    const struct font_metrics *bm = font_instance_metrics(base);
    check("font starts at the configured default size",
          font_instance_size_pt(base) == FONT_DEFAULT_SIZE_PT);
    struct font_instance *bigger = font_instance_acquire(font_instance_size_pt(base) + 4, 120);
    check("a larger size is another instance", bigger && bigger != base);
    const struct font_metrics *gm = font_instance_metrics(bigger);
    check("a larger font does not shrink its cell",
          gm->cell_w >= bm->cell_w && gm->cell_h > bm->cell_h);
    check("the same size and scale give the same instance",
          font_instance_acquire(font_instance_size_pt(base) + 4, 120) == bigger);
    font_instance_unref(bigger);

    const struct glyph *small_a = glyph_cache_fetch(base, 'A', 0);
    const struct glyph *big_a = glyph_cache_fetch(bigger, 'A', 0);
    check("each size caches its own glyphs", small_a && big_a && small_a != big_a &&
          big_a->width == gm->cell_w && small_a->width == bm->cell_w);
    font_instance_unref(bigger);

    struct font_instance *huge = font_instance_acquire(FONT_MAX_SIZE_PT + 100, 120);
    check("font size is clamped to the supported maximum",
          huge && font_instance_size_pt(huge) == FONT_MAX_SIZE_PT);
    font_instance_unref(huge);
    font_bind_thread(base);
}

static void test_render_binds_own_instance(void)
{
    struct font_instance *small = font_instance_acquire(8, 120);
    struct font_instance *large = font_instance_acquire(20, 120);
    assert(small && large);
    struct font_instance *pairs[2][2] = { { small, large }, { large, small } };
    struct glyph g = {0};
    for (int i = 0; i < 2; i++) {
        struct font_instance *before = pairs[i][0], *fi = pairs[i][1];
        const struct font_metrics *m = font_instance_metrics(fi);
        char what[96];

        font_bind_thread(before);
        bool ok = font_instance_render_glyph(fi, 0x2588, 0, &g);
        bool filled = ok;
        for (int y = 0; filled && y < g.height; y++)
            filled = glyph_row_inked(&g, y);
        snprintf(what, sizeof(what), "a full block at %dpt fills its own cell",
                 font_instance_size_pt(fi));
        check(what, ok && g.width == m->cell_w && g.height == m->cell_h && filled);

        font_bind_thread(before);
        ok = font_instance_render_glyph(fi, 'M', 0, &g);
        snprintf(what, sizeof(what), "a letter at %dpt has the rows of its own cell",
                 font_instance_size_pt(fi));
        check(what, ok && g.height == m->cell_h);
    }
    glyph_release(&g);
    font_instance_unref(small);
    font_instance_unref(large);
    font_bind_thread(cur());
}

static void test_hinting(void)
{
    enum font_hint_style h;
    enum font_aa_mode aa;
    check("\"auto\" is a valid hinting mode",
          font_parse_hint_style("auto", &h) && h == FONT_HINT_AUTO);
    check("hinting mode parsing is case-insensitive",
          font_parse_hint_style("AUTO", &h) && h == FONT_HINT_AUTO);
    check("\"system\" selects auto hinting",
          font_parse_hint_style("system", &h) && h == FONT_HINT_AUTO);
    check("\"auto\" is a valid antialias mode",
          font_parse_aa_mode("auto", &aa) && aa == FONT_AA_AUTO);
    check("an unknown hinting mode is rejected",
          !font_parse_hint_style("bogus", &h));
    check("a missing hinting mode is rejected",
          !font_parse_hint_style(NULL, &h));
    check("an antialias name is not a hinting style",
          !font_parse_hint_style("grayscale", &h));
    check("a hinting style is not an antialias name",
          !font_parse_aa_mode("full", &aa));

    struct glyph hinted = {0}, mono = {0};
    font_set_hint_style(FONT_HINT_SLIGHT);
    bool ok = font_instance_render_glyph(cur(), 'A', 0, &hinted);
    font_set_aa_mode(FONT_AA_MONO);
    ok &= font_instance_render_glyph(cur(), 'A', 0, &mono);
    check("glyphs rasterize under both antialiased and monochrome modes", ok);
    check("hinting modes produce distinct rasters",
          memcmp(hinted.row_masks, mono.row_masks,
                 (size_t)GLYPH_CELL_H * hinted.mask_words *
                 sizeof(*hinted.row_masks)) != 0 ||
          memcmp(hinted.alpha[0], mono.alpha[0],
                 (size_t)GLYPH_CELL_H * hinted.width) != 0);
    bool binary = true;
    for (int y = 0; y < GLYPH_CELL_H; y++)
        for (int x = 0; x < mono.width; x++)
            if (mono.alpha[y][x] != 0 && mono.alpha[y][x] != 255)
                binary = false;
    check("monochrome mode emits only solid pixels", binary);
    glyph_release(&hinted);
    glyph_release(&mono);

    font_set_hint_style(FONT_HINT_FULL);
    struct glyph still_mono = {0};
    font_instance_render_glyph(cur(), 'A', 0, &still_mono);
    binary = still_mono.width > 0;
    for (int y = 0; y < GLYPH_CELL_H && binary; y++)
        for (int x = 0; x < still_mono.width; x++)
            if (still_mono.alpha[y][x] != 0 && still_mono.alpha[y][x] != 255)
                binary = false;
    check("changing hinting strength keeps monochrome antialiasing", binary);
    glyph_release(&still_mono);

    font_set_hint_style(FONT_HINT_NONE);
    struct glyph unhinted_mono = {0};
    font_instance_render_glyph(cur(), 'A', 0, &unhinted_mono);
    binary = unhinted_mono.width > 0;
    for (int y = 0; y < GLYPH_CELL_H && binary; y++)
        for (int x = 0; x < unhinted_mono.width; x++)
            if (unhinted_mono.alpha[y][x] != 0 && unhinted_mono.alpha[y][x] != 255)
                binary = false;
    check("disabling hinting keeps monochrome antialiasing", binary);
    glyph_release(&unhinted_mono);

    font_set_aa_mode(FONT_AA_GRAYSCALE);
    struct glyph gray = {0};
    font_instance_render_glyph(cur(), 'A', 0, &gray);
    bool smooth = false;
    for (int y = 0; y < GLYPH_CELL_H; y++)
        for (int x = 0; x < gray.width; x++)
            if (gray.alpha[y][x] != 0 && gray.alpha[y][x] != 255)
                smooth = true;
    check("switching antialiasing mode keeps the hinting strength", smooth);
    glyph_release(&gray);

    font_set_aa_mode(FONT_AA_RGB);
    struct glyph subpx_g = {0};
    font_instance_render_glyph(cur(), 'A', 0, &subpx_g);
    check("subpixel RGB mode sets has_subpx_ink", subpx_g.has_subpx_ink && subpx_g.subpx != NULL);
    bool has_distinct_rgb = false;
    for (int y = 0; y < GLYPH_CELL_H; y++) {
        for (int x = 0; x < subpx_g.width; x++) {
            const uint8_t *rgb = glyph_subpx_at(&subpx_g, y, x);
            if (rgb[0] != rgb[1] || rgb[1] != rgb[2])
                has_distinct_rgb = true;
        }
    }
    check("subpixel RGB mode produces distinct per-channel coverages", has_distinct_rgb);
    glyph_release(&subpx_g);

    use_reference_rendering();
}

static uint64_t glyph_ink_hash(const struct glyph *g)
{
    uint64_t h = UINT64_C(1469598103934665603);
    h = (h ^ (unsigned)g->width) * UINT64_C(1099511628211);
    h = (h ^ (unsigned)g->x_offset) * UINT64_C(1099511628211);
    h = (h ^ g->has_color) * UINT64_C(1099511628211);
    for (int y = 0; y < GLYPH_CELL_H; y++) {
        h = row_hash(h, g, y);
        for (int x = 0; x < g->width; x++)
            h = (h ^ (g->has_color ? *glyph_color_at(g, y, x) : g->alpha[y][x])) *
                UINT64_C(1099511628211);
    }
    return h;
}

struct live_font_worker {
    struct font_instance *font;
    int ready[2], resume[2];
    uint64_t ink;
};

static void *render_live_font(void *arg)
{
    struct live_font_worker *w = arg;
    struct glyph g = {0};
    font_thread_attach();
    font_instance_render_glyph(w->font, 'M', 0, &g);
    char token = 1;
    assert(write(w->ready[1], &token, 1) == 1);
    assert(read(w->resume[0], &token, 1) == 1);
    font_thread_attach();
    if (font_instance_render_glyph(w->font, 'M', 0, &g))
        w->ink = glyph_ink_hash(&g);
    glyph_release(&g);
    font_thread_detach();
    return NULL;
}

static void test_live_font_family(void)
{
    size_t count = 0;
    char **families = font_list_families(&count);
    bool available = false;
    for (size_t i = 0; i < count; i++) {
        available |= strcmp(families[i], "DejaVu Sans Mono") == 0;
        free(families[i]);
    }
    free(families);
    if (!available) {
        printf("SKIP  DejaVu Sans Mono is not installed\n");
        return;
    }
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", font_get_family());
    struct glyph g = {0};
    font_instance_render_glyph(cur(), 'M', 0, &g);
    uint64_t original = glyph_ink_hash(&g);

    struct live_font_worker worker = { .font = font_instance_default() };
    assert(pipe(worker.ready) == 0 && pipe(worker.resume) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, render_live_font, &worker) == 0);
    char token;
    assert(read(worker.ready[0], &token, 1) == 1);

    font_set_family("DejaVu Sans Mono");
    struct font_instance *fi = font_instance_default();
    check("a font family change renders without font reinitialization",
          font_instance_render_glyph(fi, 'M', 0, &g) && glyph_ink_hash(&g) != original);
    uint64_t changed = glyph_ink_hash(&g);
    assert(write(worker.resume[1], &token, 1) == 1);
    pthread_join(thread, NULL);
    check("an existing render worker uses the new family", worker.ink == changed);
    for (int i = 0; i < 2; i++) {
        close(worker.ready[i]);
        close(worker.resume[i]);
    }

    font_set_family(saved);
    fi = font_instance_default();
    check("restoring a font family restores its glyphs",
          font_instance_render_glyph(fi, 'M', 0, &g) && glyph_ink_hash(&g) == original);
    glyph_release(&g);
}

static void test_fallback_reuse(void)
{
    static const uint32_t samples[] = {
        'A', 0x0633, 0x4E2D, 0x1F0A1, 0x1D11E, 0x1F642, 0x10FFFF,
    };
    static const uint16_t styles[] = {
        0, FONT_STYLE_ITALIC, FONT_STYLE_TEXT, FONT_STYLE_EMOJI | FONT_STYLE_WIDE,
    };
    uint64_t expected[sizeof(samples) / sizeof(samples[0])]
                     [sizeof(styles) / sizeof(styles[0])];
    struct glyph g = {0};
    set_scale(180);
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        for (unsigned j = 0; j < sizeof(styles) / sizeof(styles[0]); j++) {
            font_instance_render_glyph(cur(), samples[i], styles[j], &g);
            expected[i][j] = glyph_ink_hash(&g);
        }
    }

    for (unsigned i = 0; i < 4096; i++) {
        uint32_t cp = 0x800 + ((i * 7919u) % 0x108000);
        if (cp < 0xd800 || cp > 0xdfff)
            font_instance_render_glyph(cur(), cp, 0, &g);
    }
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            font_backend_shutdown();
            check("fonts reinitialize after varied Unicode rendering", init_fonts());
        }
        bool same = true;
        for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            for (unsigned j = 0; j < sizeof(styles) / sizeof(styles[0]); j++) {
                font_instance_render_glyph(cur(), samples[i], styles[j], &g);
                same &= expected[i][j] == glyph_ink_hash(&g);
            }
        }
        check(pass ? "styled glyphs survive font reinitialization at scale"
                   : "styled glyphs remain identical after varied Unicode rendering", same);
    }
    glyph_release(&g);
    set_scale(120);
}

static void test_playing_cards(void)
{

    static const uint32_t cards[] = {
        0x1F0A0, 0x1F0A1, 0x1F0AE,
        0x1F0B1, 0x1F0B2, 0x1F0B3, 0x1F0B4, 0x1F0B5, 0x1F0B6, 0x1F0B7,
        0x1F0B8, 0x1F0B9, 0x1F0BA, 0x1F0BB, 0x1F0BC, 0x1F0BD, 0x1F0BE,
        0x1F0C1, 0x1F0CE, 0x1F0D1, 0x1F0DE,
        0x1F0E0, 0x1F0E1, 0x1F0F5,
    };
    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); i++) {
        uint32_t cp = cards[i];
        char label[96];
        snprintf(label, sizeof(label), "U+%05X renders", cp);
        struct glyph g = {0};
        bool ok = font_instance_render_glyph(cur(), cp, 0, &g) && !is_unknown(&g);
        check(label, ok);
        if (ok) {
            bool ink = false;
            for (int y = 0; y < GLYPH_CELL_H; y++)
                ink |= glyph_row_inked(&g, y);
            snprintf(label, sizeof(label),
                     "U+%05X draws monochrome ink", cp);
            check(label, ink && !g.has_color);
        }
        glyph_release(&g);
    }

    uint64_t seen[14];
    for (uint32_t i = 0; i < 14; i++) {
        struct glyph g = {0};
        font_instance_render_glyph(cur(), 0x1F0B1 + i, 0, &g);
        seen[i] = glyph_ink_hash(&g);
        glyph_release(&g);
    }
    bool distinct = true;
    for (int i = 0; i < 14; i++)
        for (int j = i + 1; j < 14; j++)
            distinct &= seen[i] != seen[j];
    check("each heart rank renders differently", distinct);

    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); i++) {
        char utf8[5], label[96];
        utf8_of(cards[i], utf8);
        snprintf(label, sizeof(label), "U+%05X takes one column", cards[i]);
        check(label, shape(utf8).cols == 1);
    }

    int cw, ch;
    struct glyph ace = {0};
    font_instance_render_glyph(cur(), 0x1F0B1, 0, &ace);
    ink_box(&ace, &cw, &ch);
    check("a card's ink is taller than wide", ch > cw);
    glyph_release(&ace);
}

static const char *find_cardless_font(void)
{
    static char path[PATH_MAX];
    path[0] = 0;
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_CHARSET, NULL);
    FcFontSet *fs = pat && os ? FcFontList(NULL, pat, os) : NULL;
    for (int i = 0; fs && i < fs->nfont && !path[0]; i++) {
        FcChar8 *file;
        FcCharSet *cs;
        if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) != FcResultMatch ||
            FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch)
            continue;
        if (FcCharSetHasChar(cs, 'A') && !FcCharSetHasChar(cs, 0x1F0B1))
            snprintf(path, sizeof(path), "%s", (const char *)file);
    }
    if (fs) FcFontSetDestroy(fs);
    if (os) FcObjectSetDestroy(os);
    if (pat) FcPatternDestroy(pat);
    return path[0] ? path : NULL;
}

static void test_playing_cards_embedded(void)
{

    const char *plain = find_cardless_font();
    char dir[] = "/tmp/wsap-fonttest-XXXXXX";
    if (!plain || !mkdtemp(dir)) {
        printf("SKIP  no card-less font available for isolation test\n");
        return;
    }
    char cachedir[PATH_MAX], conffile[PATH_MAX], link[PATH_MAX];
    snprintf(cachedir, sizeof(cachedir), "%s/cache", dir);
    snprintf(conffile, sizeof(conffile), "%s/fonts.conf", dir);
    snprintf(link, sizeof(link), "%s/font.ttf", dir);
    bool setup = !mkdir(cachedir, 0700) && !symlink(plain, link);
    if (setup) {
        char conf[1024];
        int n = snprintf(conf, sizeof(conf),
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
            "<fontconfig>\n\t<dir>%s</dir>\n\t<cachedir>%s</cachedir>\n"
            "</fontconfig>\n", dir, cachedir);
        FILE *f = fopen(conffile, "w");
        setup = f && n > 0 && n < (int)sizeof(conf) &&
                fwrite(conf, 1, (size_t)n, f) == (size_t)n;
        if (f) fclose(f);
    }

    if (!setup) {
        printf("SKIP  cannot build an isolated fontconfig sandbox\n");
        goto out;
    }
    {
        bool ok = false, text_ok = false;
        const char *saved_env = getenv("FONTCONFIG_FILE");
        char *saved = saved_env ? strdup(saved_env) : NULL;
        setenv("FONTCONFIG_FILE", conffile, 1);
        font_backend_shutdown();
        if (init_fonts()) {
            struct glyph card = {0}, letter = {0};
            font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
            ok = font_instance_render_glyph(cur(), 0x1F0B1, 0, &card) && !is_unknown(&card);
            text_ok = font_instance_render_glyph(cur(), 'A', 0, &letter) &&
                    !is_unknown(&letter);
            glyph_release(&card);
            glyph_release(&letter);
        }
        if (saved) {
            setenv("FONTCONFIG_FILE", saved, 1);
            free(saved);
        } else {
            unsetenv("FONTCONFIG_FILE");
        }
        font_backend_shutdown();
        init_fonts();
        font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);

        check("the isolation fontconfig still renders text", text_ok);
        check("cards render without any installed card font", ok);
    }

out:
    unlink(link);
    unlink(conffile);
    rmdir(cachedir);
    rmdir(dir);
}

static bool each_regular_font(bool (*fn)(const char *file, const char *family,
                                         FcCharSet *cs, void *ctx),
                              void *ctx)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib))
        return false;
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_FAMILY, FC_WEIGHT,
                                       FC_SLANT, FC_CHARSET, NULL);
    FcFontSet *fs = pat && os ? FcFontList(NULL, pat, os) : NULL;
    bool found = false;
    for (int i = 0; fs && i < fs->nfont && !found; i++) {
        FcPattern *m = fs->fonts[i];
        FcChar8 *file = NULL, *family = NULL;
        FcCharSet *cs = NULL;
        int weight = -1, slant = -1;
        if (FcPatternGetString(m, FC_FILE, 0, &file) != FcResultMatch ||
            FcPatternGetString(m, FC_FAMILY, 0, &family) != FcResultMatch ||
            FcPatternGetCharSet(m, FC_CHARSET, 0, &cs) != FcResultMatch ||
            strstr((const char *)file, ".ttc"))
            continue;
        FcPatternGetInteger(m, FC_WEIGHT, 0, &weight);
        FcPatternGetInteger(m, FC_SLANT, 0, &slant);
        if (slant != FC_SLANT_ROMAN || weight < 0 || weight > FC_WEIGHT_REGULAR)
            continue;
        FT_Face probe = NULL;
        if (FT_New_Face(lib, (const char *)file, 0, &probe) ||
            !FT_IS_SCALABLE(probe) || !probe->family_name) {
            if (probe)
                FT_Done_Face(probe);
            continue;
        }
        found = fn((const char *)file, probe->family_name, cs, ctx);
        FT_Done_Face(probe);
    }
    if (fs)
        FcFontSetDestroy(fs);
    if (os)
        FcObjectSetDestroy(os);
    if (pat)
        FcPatternDestroy(pat);
    FT_Done_FreeType(lib);
    return found;
}

static bool match_sibling(const char *regular, const char *family, int weight,
                          int slant, char out[PATH_MAX], FcCharSet **cs)
{
    FcPattern *want = FcPatternCreate();
    if (!want)
        return false;
    FcPatternAddString(want, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(want, FC_SLANT, slant);
    FcPatternAddInteger(want, FC_WEIGHT, weight);
    FcConfigSubstitute(NULL, want, FcMatchPattern);
    FcDefaultSubstitute(want);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, want, &res);
    FcPatternDestroy(want);
    if (!m)
        return false;
    FcChar8 *file = NULL;
    FcCharSet *mcs = NULL;
    int mweight = 0, mslant = 0;
    FcPatternGetInteger(m, FC_WEIGHT, 0, &mweight);
    FcPatternGetInteger(m, FC_SLANT, 0, &mslant);
    bool slant_ok = slant == FC_SLANT_ROMAN
        ? mslant == FC_SLANT_ROMAN
        : (mslant == FC_SLANT_ITALIC || mslant == FC_SLANT_OBLIQUE);
    bool weight_ok = weight == FC_WEIGHT_REGULAR ? mweight < FC_WEIGHT_DEMIBOLD
                                                 : mweight >= FC_WEIGHT_DEMIBOLD;
    bool ok = slant_ok && weight_ok &&
              FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch &&
              file && strcmp((const char *)file, regular);
    if (ok)
        snprintf(out, PATH_MAX, "%s", (const char *)file);
    if (ok && cs)
        *cs = FcPatternGetCharSet(m, FC_CHARSET, 0, &mcs) == FcResultMatch
            ? FcCharSetCopy(mcs) : NULL;
    FcPatternDestroy(m);
    return ok;
}

static bool styled_family_fn(const char *file, const char *family,
                             FcCharSet *cs, void *ctx)
{
    char (*paths)[PATH_MAX] = ctx;
    if (!FcCharSetHasChar(cs, 'e') ||
        !match_sibling(file, family, FC_WEIGHT_BOLD, FC_SLANT_ROMAN, paths[1], NULL))
        return false;
    snprintf(paths[0], PATH_MAX, "%s", file);
    return true;
}

static bool find_styled_family(char paths[2][PATH_MAX])
{
    paths[0][0] = paths[1][0] = 0;
    return each_regular_font(styled_family_fn, paths);
}

static char sandbox_dir[PATH_MAX], sandbox_conf[PATH_MAX];
static char sandbox_links[4][PATH_MAX];
static int sandbox_nlinks;

static bool make_font_sandbox(const char *const *files, int nfiles)
{
    char dir[] = "/tmp/wsap-boldtest-XXXXXX";
    char cachedir[PATH_MAX];
    if (!mkdtemp(dir))
        return false;
    snprintf(sandbox_dir, sizeof(sandbox_dir), "%s", dir);
    snprintf(sandbox_conf, sizeof(sandbox_conf), "%s/fonts.conf", dir);
    snprintf(cachedir, sizeof(cachedir), "%s/cache", dir);
    bool ok = !mkdir(cachedir, 0700);
    for (int i = 0; ok && i < nfiles; i++) {
        snprintf(sandbox_links[i], PATH_MAX, "%s/font%d.ttf", dir, i);
        ok = !symlink(files[i], sandbox_links[i]);
    }
    sandbox_nlinks = ok ? nfiles : 0;
    if (ok) {
        char conf[1024];
        int n = snprintf(conf, sizeof(conf),
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
            "<fontconfig>\n\t<dir>%s</dir>\n\t<cachedir>%s</cachedir>\n"
            "</fontconfig>\n", dir, cachedir);
        FILE *f = fopen(sandbox_conf, "w");
        ok = f && n > 0 && n < (int)sizeof(conf) &&
             fwrite(conf, 1, (size_t)n, f) == (size_t)n;
        if (f)
            fclose(f);
    }
    return ok;
}

static void remove_font_sandbox(void)
{
    for (int i = 0; i < sandbox_nlinks; i++)
        unlink(sandbox_links[i]);
    char cachedir[PATH_MAX + 16];
    snprintf(cachedir, sizeof(cachedir), "%s/cache", sandbox_dir);
    unlink(sandbox_conf);
    rmdir(cachedir);
    rmdir(sandbox_dir);
    sandbox_nlinks = 0;
}

static char *dupenv(const char *name)
{
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}

static void restore_env(const char *name, char *saved)
{
    if (saved) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

struct bold_worker_result {
    uint64_t ink;
    bool ok;
};

static void *render_bold_on_worker(void *arg)
{
    struct bold_worker_result *r = arg;
    struct glyph g = {0};
    r->ok = font_thread_attach();
    r->ok = r->ok && font_instance_render_glyph(font_instance_default(), 'e', FONT_STYLE_BOLD,
                                                &g);
    if (r->ok)
        r->ink = glyph_ink_hash(&g);
    glyph_release(&g);
    font_thread_detach();
    return NULL;
}

static bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = in ? fopen(dst, "wb") : NULL;
    char buf[16384];
    bool ok = out != NULL;
    while (ok) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n && fwrite(buf, 1, n, out) != n)
            ok = false;
        if (n < sizeof(buf)) {
            if (ferror(in)) ok = false;
            break;
        }
    }
    if (out && fclose(out)) ok = false;
    if (in) fclose(in);
    if (!ok) unlink(dst);
    return ok;
}

static const char *find_color_emoji_font(void)
{
    static char path[PATH_MAX];
    FcPattern *pat = FcNameParse((const FcChar8 *)"Noto Color Emoji");
    if (!pat)
        return NULL;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);
    FcChar8 *file = NULL;
    FcBool color = FcFalse;
    if (match) {
        FcPatternGetBool(match, FC_COLOR, 0, &color);
        if (color && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            snprintf(path, sizeof(path), "%s", (const char *)file);
        FcPatternDestroy(match);
    }
    return path[0] ? path : NULL;
}

static void test_deferred_face_retry(void)
{
    const char *plain = find_cardless_font();
    const char *emoji = find_color_emoji_font();
    if (!plain || !emoji) {
        printf("SKIP  no color emoji font available for deferred-load retry\n");
        return;
    }

    char *saved_fc = dupenv("FONTCONFIG_FILE");
    const char *files[] = { plain, emoji };
    if (!make_font_sandbox(files, 2)) {
        printf("SKIP  cannot build deferred-load font sandbox\n");
        restore_env("FONTCONFIG_FILE", saved_fc);
        return;
    }

    unlink(sandbox_links[1]);
    if (!copy_file(emoji, sandbox_links[1])) {
        printf("SKIP  cannot copy the deferred emoji font\n");
        remove_font_sandbox();
        restore_env("FONTCONFIG_FILE", saved_fc);
        return;
    }
    setenv("FONTCONFIG_FILE", sandbox_conf, 1);
    font_set_family(plain);
    font_backend_shutdown();
    bool initialized = init_fonts();
    if (initialized)
        font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
    unlink(sandbox_links[1]);

    struct glyph first = {0}, retry = {0};
    bool first_missing = initialized &&
        font_instance_render_glyph(cur(), 0x1F600, FONT_STYLE_WIDE | FONT_STYLE_EMOJI, &first) &&
        is_unknown(&first);
    bool restored = copy_file(emoji, sandbox_links[1]);
    bool retry_ok = restored &&
        font_instance_render_glyph(cur(), 0x1F44D, FONT_STYLE_WIDE | FONT_STYLE_EMOJI, &retry) &&
        !is_unknown(&retry) && retry.has_color;
    check("a deferred face can fail while its file is unavailable", first_missing);
    check("a deferred face retries after its file becomes available", retry_ok);
    glyph_release(&first);
    glyph_release(&retry);

    remove_font_sandbox();
    restore_env("FONTCONFIG_FILE", saved_fc);
    font_set_family(NULL);
    font_backend_shutdown();
    init_fonts();
    font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
}

struct segment_ink {
    bool shaped[FONT_RUN_MAX];
    uint64_t ink[FONT_RUN_MAX];
};

static void collect_span_ink(void *ctx, const struct glyph_span *span)
{
    struct segment_ink *s = ctx;
    for (int i = 0; i < span->count; i++) {
        s->shaped[span->start + i] = true;
        s->ink[span->start + i] = glyph_ink_hash(&span->cells[i]);
    }
}

static struct segment_ink segment_ink(const uint32_t *cps, int n, uint16_t style)
{
    struct segment_ink s = {0};
    glyph_cache_shape_segment(cur(), cps, n, style, collect_span_ink, &s);
    for (int i = 0; i < n; i++) {
        const struct glyph *g = s.shaped[i] ? NULL : glyph_cache_fetch(cur(), cps[i], style);
        if (g)
            s.ink[i] = glyph_ink_hash(g);
    }
    return s;
}

static void test_bold_face(void)
{
    char styled[2][PATH_MAX];
    if (!find_styled_family(styled)) {
        printf("SKIP  no font family with a bold sibling installed\n");
        return;
    }
    char *saved_fc = dupenv("FONTCONFIG_FILE");

    uint64_t plain_ink = 0, fake_ink = 0;
    const char *one[] = { styled[0] };
    if (make_font_sandbox(one, 1)) {
        setenv("FONTCONFIG_FILE", sandbox_conf, 1);
        font_set_family(styled[0]);
        font_backend_shutdown();
        if (init_fonts()) {
            font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
            struct glyph r = {0}, b = {0};
            bool ok = font_instance_render_glyph(cur(), 'e', 0, &r) &&
                      !is_unknown(&r) &&
                      font_instance_render_glyph(cur(), 'e', FONT_STYLE_BOLD, &b) &&
                      !is_unknown(&b);
            plain_ink = glyph_ink_hash(&r);
            fake_ink = glyph_ink_hash(&b);
            check("bold synthesis still renders without a bold sibling",
                  ok && fake_ink != plain_ink);
            glyph_release(&r);
            glyph_release(&b);
        }
        remove_font_sandbox();
    }

    struct bold_worker_result wr = {0};
    const char *two[] = { styled[0], styled[1] };
    if (make_font_sandbox(two, 2)) {
        setenv("FONTCONFIG_FILE", sandbox_conf, 1);
        font_set_family(styled[0]);
        font_backend_shutdown();
        if (init_fonts()) {
            font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
            struct glyph b = {0};
            check("bold text renders with a real bold face",
                  font_instance_render_glyph(cur(), 'e', FONT_STYLE_BOLD, &b) &&
                  !is_unknown(&b));
            uint64_t real_ink = glyph_ink_hash(&b);
            glyph_release(&b);
            check("the real bold face outranks synthesis",
                  real_ink != plain_ink && real_ink != fake_ink);

            const uint32_t run[] = { 'e', 'e' };
            struct segment_ink shown = segment_ink(run, 2, FONT_STYLE_BOLD);
            check("a bold cell run shows the real bold face",
                  shown.ink[0] == real_ink && shown.ink[1] == real_ink);

            pthread_t th;
            if (!pthread_create(&th, NULL, render_bold_on_worker, &wr))
                pthread_join(th, NULL);
            check("a render worker resolves the same bold face",
                  wr.ok && wr.ink == real_ink);
        }
        remove_font_sandbox();
    }

    restore_env("FONTCONFIG_FILE", saved_fc);
    font_set_family(NULL);
    font_backend_shutdown();
    init_fonts();
    font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
}

static uint64_t golden_ink_hash(const struct glyph *g)
{
    uint64_t h = UINT64_C(14695981039346656037);
    const uint8_t *b;
    int width = g->width, color = g->has_color;
    b = (const uint8_t *)&width;
    for (size_t i = 0; i < sizeof(width); i++)
        h = (h ^ b[i]) * UINT64_C(0x100000001b3);
    b = (const uint8_t *)&color;
    for (size_t i = 0; i < sizeof(color); i++)
        h = (h ^ b[i]) * UINT64_C(0x100000001b3);
    for (int y = 0; y < font_instance_metrics(cur())->cell_h; y++) {
        if (!g->has_color && g->alpha && g->alpha[y])
            for (int x = 0; x < width; x++)
                h = (h ^ g->alpha[y][x]) * UINT64_C(0x100000001b3);
        b = (const uint8_t *)glyph_mask(g, y);
        for (size_t i = 0; i < g->mask_words * sizeof(uint64_t); i++)
            h = (h ^ b[i]) * UINT64_C(0x100000001b3);
    }
    return h;
}

static void test_variant_golden(void)
{
    static const struct { uint32_t cp; uint16_t style; uint64_t hash; } golden[] = {
        { 'A', 0, UINT64_C(0xd2f4d6bb3c859393) },
        { 'A', FONT_STYLE_BOLD, UINT64_C(0x1491aa51bda13865) },
        { 'A', FONT_STYLE_ITALIC, UINT64_C(0x5da368bd8a138efe) },
        { 'A', FONT_STYLE_BOLD | FONT_STYLE_ITALIC, UINT64_C(0x8dba048f3714d23f) },
        { 'g', 0, UINT64_C(0xb6acfc125d472eb2) },
        { 'g', FONT_STYLE_BOLD, UINT64_C(0x745788716ad8c940) },
        { 'g', FONT_STYLE_ITALIC, UINT64_C(0xacb7092e93158a78) },
        { 'g', FONT_STYLE_BOLD | FONT_STYLE_ITALIC, UINT64_C(0x2a94c628ee375d2d) },
    };
    enum { N = sizeof(golden) / sizeof(golden[0]) };
    uint64_t ink[N];
    bool rendered = true;
    for (int i = 0; i < N; i++) {
        struct glyph g = {0};
        rendered &= font_instance_render_glyph(cur(), golden[i].cp, golden[i].style, &g);
        ink[i] = golden_ink_hash(&g);
        glyph_release(&g);
    }
    if (!rendered || ink[0] != golden[0].hash) {
        printf("SKIP  the default family differs from the reference machine\n");
        return;
    }
    bool same = true;
    for (int i = 0; i < N; i++)
        same &= ink[i] == golden[i].hash;
    check("bold, italic, and bold italic A and g draw the reference ink", same);
}

static bool sandbox_variant_ink(const char *const *files, int n, uint64_t ink[4])
{
    static const uint16_t styles[4] = {
        0, FONT_STYLE_BOLD, FONT_STYLE_ITALIC, FONT_STYLE_BOLD | FONT_STYLE_ITALIC,
    };
    if (!make_font_sandbox(files, n))
        return false;
    setenv("FONTCONFIG_FILE", sandbox_conf, 1);
    font_set_family(files[0]);
    font_backend_shutdown();
    bool ok = init_fonts();
    if (ok)
        font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
    for (int i = 0; ok && i < 4; i++) {
        struct glyph g = {0};
        ok = font_instance_render_glyph(cur(), 'e', styles[i], &g) && !is_unknown(&g);
        ink[i] = glyph_ink_hash(&g);
        glyph_release(&g);
    }
    remove_font_sandbox();
    return ok;
}

struct family_files {
    char paths[4][PATH_MAX];
};

static bool four_faces_fn(const char *file, const char *family, FcCharSet *cs,
                          void *ctx)
{
    struct family_files *f = ctx;
    if (!FcCharSetHasChar(cs, 'e') ||
        !match_sibling(file, family, FC_WEIGHT_BOLD, FC_SLANT_ROMAN, f->paths[1], NULL) ||
        !match_sibling(file, family, FC_WEIGHT_REGULAR, FC_SLANT_ITALIC, f->paths[2], NULL) ||
        !match_sibling(file, family, FC_WEIGHT_BOLD, FC_SLANT_ITALIC, f->paths[3], NULL) ||
        !strcmp(f->paths[1], f->paths[2]) || !strcmp(f->paths[1], f->paths[3]) ||
        !strcmp(f->paths[2], f->paths[3]))
        return false;
    snprintf(f->paths[0], PATH_MAX, "%s", file);
    return true;
}

static void test_variant_preference(void)
{
    struct family_files f = {0};
    if (!each_regular_font(four_faces_fn, &f)) {
        printf("SKIP  no font family with bold, italic, and bold italic files installed\n");
        return;
    }
    char *saved_fc = dupenv("FONTCONFIG_FILE");
    enum { PLAIN, BOLD, ITALIC, BOLD_ITALIC };
    const char *all[] = { f.paths[0], f.paths[1], f.paths[2], f.paths[3] };
    const char *no_bi[] = { f.paths[0], f.paths[1], f.paths[2] };
    const char *bold[] = { f.paths[0], f.paths[1] };
    const char *italic[] = { f.paths[0], f.paths[2] };
    uint64_t ink_all[4], ink_no_bi[4], ink_bold[4], ink_italic[4];
    bool ok = sandbox_variant_ink(all, 4, ink_all) &&
              sandbox_variant_ink(no_bi, 3, ink_no_bi) &&
              sandbox_variant_ink(bold, 2, ink_bold) &&
              sandbox_variant_ink(italic, 2, ink_italic);
    check("bold text draws the bold face",
          ok && ink_all[BOLD] == ink_bold[BOLD] && ink_all[BOLD] != ink_italic[BOLD]);
    check("italic text draws the italic face",
          ok && ink_all[ITALIC] == ink_italic[ITALIC] &&
          ink_all[ITALIC] != ink_bold[ITALIC]);
    check("bold italic text draws the bold italic face",
          ok && ink_all[BOLD_ITALIC] != ink_no_bi[BOLD_ITALIC] &&
          ink_all[PLAIN] == ink_no_bi[PLAIN]);
    check("bold italic text prefers the bold face over the italic face",
          ok && ink_no_bi[BOLD_ITALIC] == ink_bold[BOLD_ITALIC] &&
          ink_no_bi[BOLD_ITALIC] != ink_italic[BOLD_ITALIC]);

    restore_env("FONTCONFIG_FILE", saved_fc);
    font_set_family(NULL);
    font_backend_shutdown();
    init_fonts();
    font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
}

struct bold_only {
    char paths[2][PATH_MAX];
    uint32_t cp;
};

static bool bold_only_fn(const char *file, const char *family, FcCharSet *cs,
                         void *ctx)
{
    struct bold_only *b = ctx;
    FcCharSet *bold_cs = NULL;
    if (!match_sibling(file, family, FC_WEIGHT_BOLD, FC_SLANT_ROMAN, b->paths[1], &bold_cs))
        return false;
    FcCharSet *only = bold_cs ? FcCharSetSubtract(bold_cs, cs) : NULL;
    FcChar32 map[FC_CHARSET_MAP_SIZE], next;
    b->cp = 0;
    for (FcChar32 base = only ? FcCharSetFirstPage(only, map, &next) : FC_CHARSET_DONE;
         base != FC_CHARSET_DONE && !b->cp; base = FcCharSetNextPage(only, map, &next))
        for (FcChar32 k = 0; k < FC_CHARSET_MAP_SIZE * 32 && !b->cp; k++)
            if ((map[k / 32] >> (k % 32) & 1) && wsap_vtcore_unicode_width(base + k) == 1)
                b->cp = base + k;
    if (only)
        FcCharSetDestroy(only);
    if (bold_cs)
        FcCharSetDestroy(bold_cs);
    if (b->cp)
        snprintf(b->paths[0], PATH_MAX, "%s", file);
    return b->cp != 0;
}

static void test_bold_only_coverage(void)
{
    struct bold_only b = {0};
    if (!each_regular_font(bold_only_fn, &b)) {
        printf("SKIP  no font family whose bold face covers more than its regular face\n");
        return;
    }
    char *saved_fc = dupenv("FONTCONFIG_FILE");
    const char *two[] = { b.paths[0], b.paths[1] };
    if (make_font_sandbox(two, 2)) {
        setenv("FONTCONFIG_FILE", sandbox_conf, 1);
        font_set_family(b.paths[0]);
        font_backend_shutdown();
        if (init_fonts()) {
            font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
            struct glyph g = {0};
            check("bold text draws a code point only the bold face covers",
                  font_instance_render_glyph(cur(), b.cp, FONT_STYLE_BOLD, &g) &&
                  !is_unknown(&g));
            glyph_release(&g);
        }
        remove_font_sandbox();
    }

    restore_env("FONTCONFIG_FILE", saved_fc);
    font_set_family(NULL);
    font_backend_shutdown();
    init_fonts();
    font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!font_system_init()) {
        fprintf(stderr, "FAIL: font initialization\n");
        return 1;
    }
    use_reference_rendering();
    font_instance_render_glyph(cur(), 0x10FFFF, 0, &unknown);

    test_variant_golden();
    test_cell_tiling();
    test_tile_colors();
    test_programming_ligatures();
    test_live_font_family();
    test_run_with_space_shapes();
    test_font_change_in_emit();
    run_cache_survives_concurrent_eviction();
    test_raster_invariants();
    test_coverage();
    test_presentation();
    test_color_aspect_ratio();
    test_ink_centering();
    test_music();

    test_skin_tone();
    test_zwj_sequences();
    test_regional_flags();
    test_keycaps();
    test_tag_sequences();
    test_combining_marks();
    test_single_column_color();
    test_emoji_text_height();
    test_outline_aspect_ratio();
    test_fallback_order_independence();
    test_fallback_reuse();
    test_playing_cards();
    test_playing_cards_embedded();
    test_deferred_face_retry();
    test_bold_face();
    test_variant_preference();
    test_bold_only_coverage();

    test_scaled_cells();
    test_font_sizes();
    test_render_binds_own_instance();
    test_hinting();

    font_set_aa_mode(FONT_AA_RGB);
    test_raster_invariants();
    use_reference_rendering();

    set_scale(180);
    test_cell_tiling();
    test_raster_invariants();
    test_ink_centering();
    set_scale(120);
    font_instance_unref(scaled);

    font_system_shutdown();
    printf("PASS: %d font checks\n", checks - failures);
    if (failures)
        printf("FAIL: %d font checks\n", failures);
    return failures != 0;
}
