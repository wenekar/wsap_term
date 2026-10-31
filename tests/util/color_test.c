#include <wsap_vtcore/wsap_vtcore.h>
#include "term_api.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char response[256];
static size_t response_len;
static void respond(void *ud, const char *data, size_t len)
{
    (void)ud;
    assert(len < sizeof(response));
    memcpy(response, data, len);
    response[len] = 0;
    response_len = len;
}

static struct wsap_vtcore *new_term(void)
{
    struct wsap_vtcore_callbacks cb = { .reply = respond };
    struct wsap_vtcore *t = tt_new_opts(40, 4, &cb);
    assert(t);
    return t;
}

static struct wsap_vtcore *new_term_with_theme(const struct wsap_vtcore_theme *theme)
{
    struct wsap_vtcore_options opt = {
        .size = { .x = 40, .y = 4 },
        .scrollback = TEST_SCROLLBACK,
        .theme = *theme,
        .callbacks = { .reply = respond },
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    return t;
}

static void write_seq(struct wsap_vtcore *t, const char *s)
{
    for (; *s; s++)
        wsap_vtcore_feed(t, s, 1, 0);
}

static uint32_t view_palette(struct wsap_vtcore *t, int entry)
{
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(s && wsap_vtcore_snapshot_update(s, t));
    uint32_t argb = wsap_vtcore_snapshot_view(s)->palette[entry];
    wsap_vtcore_snapshot_free(s);
    return argb;
}

static uint32_t color_of(struct wsap_vtcore *t, wsap_vtcore_color c)
{
    return wsap_vtcore_color_is_slot(c) ? view_palette(t, (int)(c & 0x1FFu)) : c;
}

static struct wsap_vtcore_cell cell_at(struct wsap_vtcore *t, int col, int row)
{
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(s && wsap_vtcore_snapshot_update(s, t));
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(s, row);
    assert(cells && col < wsap_vtcore_snapshot_view(s)->size.x);
    struct wsap_vtcore_cell cell = cells[col];
    wsap_vtcore_snapshot_free(s);
    return cell;
}

static uint32_t argb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static bool channel_equal(uint32_t a, uint32_t b, int tol)
{
    for (int shift = 0; shift <= 16; shift += 8) {
        int da = (int)((a >> shift) & 0xFF) - (int)((b >> shift) & 0xFF);
        if (da < -tol || da > tol)
            return false;
    }
    return true;
}

static void expect_reply(const char *expected)
{
    assert(response_len == strlen(expected));
    assert(!strcmp(response, expected));
}

static struct wsap_vtcore_theme light_theme(void)
{
    struct wsap_vtcore_theme theme = {
        .fg = argb(0x20, 0x20, 0x20),
        .bg = argb(0xF0, 0xF0, 0xE8),
        .cursor = argb(0x10, 0x60, 0xC0),
        .interpolate_lab = true,
    };
    for (int i = 0; i < 16; i++)
        theme.ansi[i] = argb((uint8_t)(i * 13), (uint8_t)(200 - i * 9), (uint8_t)(i * 7 + 30));
    return theme;
}

static void test_parse_hex6(void)
{
    uint32_t c;
    assert(wsap_vtcore_color_parse("#ff0010", &c));
    assert(c == argb(0xFF, 0x00, 0x10));

    assert(wsap_vtcore_color_parse("#AABBCC", &c));
    assert(c == argb(0xAA, 0xBB, 0xCC));
}

static void test_parse_hex3(void)
{
    uint32_t c;
    assert(wsap_vtcore_color_parse("#fff", &c));
    assert(c == argb(0xFF, 0xFF, 0xFF));

    assert(wsap_vtcore_color_parse("#345", &c));

    assert(c == argb(0x33, 0x44, 0x55));
}

static void test_parse_bare_hex(void)
{
    uint32_t c;
    assert(wsap_vtcore_color_parse("FF0010", &c));
    assert(c == argb(0xFF, 0x00, 0x10));

    assert(wsap_vtcore_color_parse("FFF", &c));
    assert(c == argb(0xFF, 0xFF, 0xFF));
}

static void test_parse_rgb_colon(void)
{
    uint32_t c;
    assert(wsap_vtcore_color_parse("rgb:ff/00/10", &c));
    assert(c == argb(0xFF, 0x00, 0x10));

    assert(wsap_vtcore_color_parse("rgb:f/ff/fff", &c));
    assert(c == argb(0xFF, 0xFF, 0xFF));
}

static void test_parse_rgbi(void)
{
    uint32_t c;
    assert(wsap_vtcore_color_parse("rgbi:1.0/0/0", &c));
    assert(c == argb(0xFF, 0x00, 0x00));

    assert(wsap_vtcore_color_parse("rgbi:0.5/0.5/0.5", &c));
    assert(channel_equal(c, argb(0x80, 0x80, 0x80), 2));
}

static void test_parse_whitespace(void)
{
    uint32_t c;
    assert(wsap_vtcore_color_parse("  #AABBCC  ", &c));
    assert(c == argb(0xAA, 0xBB, 0xCC));
}

static void test_parse_invalid(void)
{
    uint32_t c;
    assert(!wsap_vtcore_color_parse("", &c));
    assert(!wsap_vtcore_color_parse("  ", &c));
    assert(!wsap_vtcore_color_parse("#", &c));
    assert(!wsap_vtcore_color_parse("#ff", &c));
    assert(!wsap_vtcore_color_parse("#ffff", &c));
    assert(!wsap_vtcore_color_parse("#gggggg", &c));
    assert(!wsap_vtcore_color_parse("rgb:", &c));
    assert(!wsap_vtcore_color_parse("rgb:zz/zz/zz", &c));
    assert(!wsap_vtcore_color_parse("rgbi:a/a/a", &c));
    assert(!wsap_vtcore_color_parse("nosuchcolor", &c));
    assert(!wsap_vtcore_color_parse("12345", &c));
}

static void test_zero_theme_selects_builtin_theme(void)
{
    struct wsap_vtcore_theme def;
    wsap_vtcore_theme_default(&def);
    assert(def.fg && def.bg && def.cursor);

    struct wsap_vtcore *t = new_term();
    assert(color_of(t, WSAP_VTCORE_COLOR_FG) == def.fg);
    assert(color_of(t, WSAP_VTCORE_COLOR_BG) == def.bg);
    assert(view_palette(t, WSAP_VTCORE_PALETTE_CURSOR) == def.cursor);
    for (int i = 0; i < 16; i++)
        assert(view_palette(t, i) == def.ansi[i]);
    wsap_vtcore_free(t);
}

static void test_theme_option_sets_colors(void)
{
    struct wsap_vtcore_theme theme = light_theme();
    struct wsap_vtcore *t = new_term_with_theme(&theme);
    assert(color_of(t, WSAP_VTCORE_COLOR_FG) == theme.fg);
    assert(color_of(t, WSAP_VTCORE_COLOR_BG) == theme.bg);
    assert(view_palette(t, WSAP_VTCORE_PALETTE_CURSOR) == theme.cursor);
    for (int i = 0; i < 16; i++)
        assert(view_palette(t, i) == theme.ansi[i]);
    wsap_vtcore_free(t);
}

static void test_palette_corners_match_theme(void)
{
    struct wsap_vtcore *t = new_term();
    uint32_t fg = color_of(t, WSAP_VTCORE_COLOR_FG), bg = color_of(t, WSAP_VTCORE_COLOR_BG);

    assert(channel_equal(view_palette(t, 16), bg, 3));
    assert(channel_equal(view_palette(t, 231), fg, 3));
    wsap_vtcore_free(t);
}

static void test_palette_grayscale_interpolates_bg_to_fg(void)
{
    struct wsap_vtcore *t = new_term();
    uint32_t fg = color_of(t, WSAP_VTCORE_COLOR_FG), bg = color_of(t, WSAP_VTCORE_COLOR_BG);

    assert(channel_equal(view_palette(t, 232), bg, 12));
    assert(channel_equal(view_palette(t, 255), fg, 12));
    wsap_vtcore_free(t);
}

static void test_derived_palette_follows_theme(void)
{
    struct wsap_vtcore *t = new_term();
    uint32_t cube_before = view_palette(t, 100);
    uint32_t gray_before = view_palette(t, 240);

    struct wsap_vtcore_theme theme = light_theme();
    wsap_vtcore_set_theme(t, &theme);
    assert(color_of(t, WSAP_VTCORE_COLOR_FG) == theme.fg);
    assert(color_of(t, WSAP_VTCORE_COLOR_BG) == theme.bg);
    assert(view_palette(t, 1) == theme.ansi[1]);
    assert(view_palette(t, 100) != cube_before);
    assert(view_palette(t, 240) != gray_before);

    assert(channel_equal(view_palette(t, 232), theme.fg, 12));
    assert(channel_equal(view_palette(t, 255), theme.bg, 12));
    wsap_vtcore_free(t);
}

static void test_theme_change_recolors_existing_text(void)
{
    struct wsap_vtcore *t = new_term();
    write_seq(t, "\033[31mA\033[0mB");
    struct wsap_vtcore_cell a = cell_at(t, 0, 0);
    assert(a.text == 'A');
    wsap_vtcore_color red = a.fg;
    struct wsap_vtcore_cell b = cell_at(t, 1, 0);
    assert(b.text == 'B');
    wsap_vtcore_color plain = b.fg;

    struct wsap_vtcore_theme theme = light_theme();
    wsap_vtcore_set_theme(t, &theme);
    assert(color_of(t, red) == theme.ansi[1]);
    assert(color_of(t, plain) == theme.fg);
    wsap_vtcore_free(t);
}

static void test_osc4_override_lifetime(void)
{
    struct wsap_vtcore_theme def;
    wsap_vtcore_theme_default(&def);
    struct wsap_vtcore *t = new_term_with_theme(&def);
    uint32_t derived = view_palette(t, 100);

    uint32_t custom = argb(0x12, 0x34, 0x56);
    write_seq(t, "\033]4;100;#123456\033\\");
    assert(view_palette(t, 100) == custom);

    wsap_vtcore_set_theme(t, &def);
    assert(view_palette(t, 100) == custom);

    write_seq(t, "\033]104;100\033\\");
    assert(view_palette(t, 100) == derived);

    write_seq(t, "\033]4;100;#123456\033\\");
    struct wsap_vtcore_theme theme = light_theme();
    wsap_vtcore_set_theme(t, &theme);
    assert(view_palette(t, 100) != custom);
    wsap_vtcore_free(t);
}

static void test_osc104_resets_all(void)
{
    struct wsap_vtcore *t = new_term();
    uint32_t c0 = view_palette(t, 0), c200 = view_palette(t, 200);
    write_seq(t, "\033]4;0;rgb:01/02/03;200;rgb:04/05/06\033\\");
    assert(view_palette(t, 0) == argb(1, 2, 3));
    assert(view_palette(t, 200) == argb(4, 5, 6));

    write_seq(t, "\033]104\033\\");
    assert(view_palette(t, 0) == c0);
    assert(view_palette(t, 200) == c200);
    wsap_vtcore_free(t);
}

static void test_osc4_set_and_query(void)
{
    struct wsap_vtcore *t = new_term();

    write_seq(t, "\033]4;1;#ff0000\033\\");
    assert(view_palette(t, 1) == argb(0xFF, 0x00, 0x00));

    response_len = 0;
    write_seq(t, "\033]4;1;?\033\\");
    expect_reply("\033]4;1;rgb:ffff/0000/0000\033\\");

    wsap_vtcore_free(t);
}

static void test_osc4_multiple_set(void)
{
    struct wsap_vtcore *t = new_term();
    write_seq(t, "\033]4;1;#ff0000;2;#00ff00\033\\");
    assert(view_palette(t, 1) == argb(0xFF, 0x00, 0x00));
    assert(view_palette(t, 2) == argb(0x00, 0xFF, 0x00));
    wsap_vtcore_free(t);
}

static void test_osc10_set_and_query(void)
{
    struct wsap_vtcore *t = new_term();

    write_seq(t, "\033]10;#123456\033\\");
    assert(color_of(t, WSAP_VTCORE_COLOR_FG) == argb(0x12, 0x34, 0x56));

    response_len = 0;
    write_seq(t, "\033]10;?\033\\");
    expect_reply("\033]10;rgb:1212/3434/5656\033\\");

    write_seq(t, "\033]110\033\\");
    struct wsap_vtcore_theme def;
    wsap_vtcore_theme_default(&def);
    assert(color_of(t, WSAP_VTCORE_COLOR_FG) == def.fg);
    wsap_vtcore_free(t);
}

static void test_osc11_set_and_query(void)
{
    struct wsap_vtcore *t = new_term();

    write_seq(t, "\033]11;#aabbcc\033\\");
    assert(color_of(t, WSAP_VTCORE_COLOR_BG) == argb(0xAA, 0xBB, 0xCC));

    response_len = 0;
    write_seq(t, "\033]11;?\033\\");
    expect_reply("\033]11;rgb:aaaa/bbbb/cccc\033\\");

    wsap_vtcore_free(t);
}

static void test_osc12_set_and_query(void)
{
    struct wsap_vtcore *t = new_term();

    write_seq(t, "\033]12;#ff00ff\033\\");
    assert(view_palette(t, WSAP_VTCORE_PALETTE_CURSOR) == argb(0xFF, 0x00, 0xFF));

    response_len = 0;
    write_seq(t, "\033]12;?\033\\");
    expect_reply("\033]12;rgb:ffff/0000/ffff\033\\");

    wsap_vtcore_free(t);
}

static void test_osc12_query_default(void)
{
    struct wsap_vtcore_theme theme = light_theme();
    struct wsap_vtcore *t = new_term_with_theme(&theme);
    response_len = 0;
    write_seq(t, "\033]12;?\033\\");

    char expected[64];
    uint32_t def = theme.cursor;
    snprintf(expected, sizeof(expected),
             "\033]12;rgb:%02x%02x/%02x%02x/%02x%02x\033\\",
             (def >> 16) & 0xFF, (def >> 16) & 0xFF,
             (def >> 8) & 0xFF, (def >> 8) & 0xFF,
             def & 0xFF, def & 0xFF);
    expect_reply(expected);

    wsap_vtcore_free(t);
}

static void test_osc_is_per_terminal(void)
{
    struct wsap_vtcore *a = new_term();
    struct wsap_vtcore *b = new_term();
    uint32_t fg = color_of(b, WSAP_VTCORE_COLOR_FG);
    uint32_t bg = color_of(b, WSAP_VTCORE_COLOR_BG);
    uint32_t c1 = view_palette(b, 1);

    write_seq(a, "\033]4;1;#010203\033\\\033]10;#040506\033\\\033]11;#070809\033\\");
    assert(view_palette(a, 1) == argb(1, 2, 3));
    assert(color_of(a, WSAP_VTCORE_COLOR_FG) == argb(4, 5, 6));
    assert(color_of(a, WSAP_VTCORE_COLOR_BG) == argb(7, 8, 9));

    assert(view_palette(b, 1) == c1);
    assert(color_of(b, WSAP_VTCORE_COLOR_FG) == fg);
    assert(color_of(b, WSAP_VTCORE_COLOR_BG) == bg);

    struct wsap_vtcore *c = new_term();
    assert(view_palette(c, 1) == c1);
    assert(color_of(c, WSAP_VTCORE_COLOR_FG) == fg);

    wsap_vtcore_free(c);
    wsap_vtcore_free(b);
    wsap_vtcore_free(a);
}

int main(void)
{
    test_parse_hex6();
    test_parse_hex3();
    test_parse_bare_hex();
    test_parse_rgb_colon();
    test_parse_rgbi();
    test_parse_whitespace();
    test_parse_invalid();

    test_zero_theme_selects_builtin_theme();
    test_theme_option_sets_colors();
    test_palette_corners_match_theme();
    test_palette_grayscale_interpolates_bg_to_fg();
    test_derived_palette_follows_theme();
    test_theme_change_recolors_existing_text();
    test_osc4_override_lifetime();
    test_osc104_resets_all();

    test_osc4_set_and_query();
    test_osc4_multiple_set();
    test_osc10_set_and_query();
    test_osc11_set_and_query();
    test_osc12_set_and_query();
    test_osc12_query_default();
    test_osc_is_per_terminal();

    printf("PASS: color tests\n");
    return 0;
}
