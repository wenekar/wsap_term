#include "wsap_vtcore/wsap_vtcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct spec {
    const char *name, *input;
    int cols, rows;
    const char *line[4];
    int x, y;
    int attr_x, attr_y;
    uint16_t attrs;
    wsap_vtcore_color fg, bg;
    int history;
    const char *reply;
};

#define ROWS3(a, b, c) .line = { a, b, c }
#define AT(x_, y_) .x = x_, .y = y_
#define NO_EXTRA .attr_x = -1, .history = -1
#define CASE(name_, input_, a, b, c, x_, y_) \
    { .name = name_, .input = input_, .cols = 8, .rows = 3, ROWS3(a, b, c), AT(x_, y_), NO_EXTRA }
#define RED (WSAP_VTCORE_COLOR_SLOT | 1)
#define BLUE (WSAP_VTCORE_COLOR_SLOT | 4)

static const struct spec cases[] = {
    CASE("print", "abc", "abc", "", "", 3, 0),
    CASE("delayed wrap", "abcdefghZ", "abcdefgh", "Z", "", 1, 1),
    CASE("carriage return", "abc\rZ", "Zbc", "", "", 1, 0),
    CASE("line feed keeps column", "ab\nZ", "ab", "  Z", "", 3, 1),
    CASE("vertical tab is a line feed", "ab\x0bZ", "ab", "  Z", "", 3, 1),
    CASE("form feed is a line feed", "ab\x0cZ", "ab", "  Z", "", 3, 1),
    CASE("backspace", "abc\bZ", "abZ", "", "", 3, 0),
    CASE("backspace at column 0", "\bZ", "Z", "", "", 1, 0),
    CASE("backspace clears pending wrap", "abcdefgh\bZ", "abcdefZh", "", "", 7, 0),
    CASE("tab", "a\tZ", "a      Z", "", "", 7, 0),
    CASE("tab stops at the last column", "\t\tZ", "       Z", "", "", 7, 0),
    CASE("tab clears pending wrap", "abcdefgh\tZ", "abcdefgZ", "", "", 7, 0),
    { .name = "tab does not erase", .input = "abcdefghijk\r\tZ", .cols = 12, .rows = 2,
      .line = { "abcdefghZjk" }, AT(9, 0), NO_EXTRA },
    CASE("tab clear all", "\033[3g\tZ", "       Z", "", "", 7, 0),
    { .name = "tab clear at cursor", .input = "\033[9G\033[0g\r\tZ", .cols = 12, .rows = 2,
      .line = { "           Z" }, AT(11, 0), NO_EXTRA },
    { .name = "tab set", .input = "\033[4G\033H\r\tZ", .cols = 12, .rows = 2,
      .line = { "   Z" }, AT(4, 0), NO_EXTRA },
    CASE("bell", "a\aZ", "aZ", "", "", 2, 0),
    CASE("delete is ignored", "ab\x7f" "c", "abc", "", "", 3, 0),
    CASE("NUL, CAN, SUB are ignored", "a\x18" "b\x1a" "c", "abc", "", "", 3, 0),
    CASE("scroll", "a\r\nb\r\nc\r\nd", "b", "c", "d", 1, 2),
    CASE("shift out to DEC graphics", "\033)0a\x0eq\x0fq", "a?q", "", "", 3, 0),
    CASE("DEC graphics in G0", "\033(0x\033(Bx", "?x", "", "", 2, 0),

    CASE("UTF-8 truncated before ASCII", "a\xe3\x80z", "a#z", "", "", 3, 0),
    CASE("UTF-8 lone continuation", "a\x80z", "a#z", "", "", 3, 0),
    CASE("UTF-8 surrogate", "\xed\xa0\x80z", "###z", "", "", 4, 0),
    CASE("UTF-8 overlong", "\xc0\xafz", "##z", "", "", 3, 0),
    CASE("UTF-8 bad second byte", "\xf0\x80z", "##z", "", "", 3, 0),
    CASE("UTF-8 ended by a control", "\xc3\rZ", "Z", "", "", 1, 0),
    CASE("UTF-8 ended by ESC", "\xc3\033[CZ", "# Z", "", "", 3, 0),
    CASE("UTF-8 ended by DEL", "\xc3\x7fZ", "#Z", "", "", 2, 0),
    CASE("UTF-8 C1 NEL", "ab\xc2\x85Z", "ab", "Z", "", 1, 1),
    CASE("UTF-8 C1 CSI", "\xc2\x9b" "3CZ", "   Z", "", "", 4, 0),
    CASE("wide glyph", "a中b", "a?_b", "", "", 4, 0),
    CASE("wide glyph wraps with a pad", "abcdefg中", "abcdefg", "?_", "", 2, 1),
    CASE("combining mark at column 0 has no base", "abcdefgh\r\n\xcc\x81Z",
         "abcdefgh", "Z", "", 1, 1),
    CASE("combining mark at right margin", "\033[?69h\033[2;4s\033[1;2Habc\xcc\x81Z",
         " ab?", " Z", "", 2, 1),
    CASE("combining mark at left margin has no base", "\033[?69h\033[3;6s\033[1;1Habcd\033[1;3H\xcc\x81Z",
         "abZd", "", "", 3, 0),
    { .name = "wide glyph on one column", .input = "中", .cols = 1, .rows = 1,
      .line = { "?" }, AT(0, 0), .attr_x = -1, .history = 0 },

    CASE("CUP", "abc\033[2;3HZ", "abc", "  Z", "", 3, 1),
    CASE("CUP clamps", "\033[9;9HZ", "", "", "       Z", 7, 2),
    CASE("CUU", "\033[3;4H\033[2AZ", "   Z", "", "", 4, 0),
    CASE("CUD", "\033[1;4H\033[2BZ", "", "", "   Z", 4, 2),
    CASE("CUF", "\033[3CZ", "   Z", "", "", 4, 0),
    CASE("CUB", "abc\033[2DZ", "aZc", "", "", 2, 0),
    CASE("CNL", "\033[2EZ", "", "", "Z", 1, 2),
    CASE("CPL", "\033[3;4H\033[2FZ", "Z", "", "", 1, 0),
    CASE("CHA", "abc\033[2GZ", "aZc", "", "", 2, 0),
    CASE("HPA", "abc\033[2`Z", "aZc", "", "", 2, 0),
    CASE("HPR", "a\033[2aZ", "a  Z", "", "", 4, 0),
    CASE("VPA", "a\033[3dZ", "a", "", " Z", 2, 2),
    CASE("VPR", "a\033[2eZ", "a", "", " Z", 2, 2),
    CASE("CUU stops at the top margin", "\033[2;3r\033[3;1H\033[5AZ", "", "Z", "", 1, 1),
    CASE("CUD stops at the bottom margin", "\033[1;2r\033[5BZ", "", "Z", "", 1, 1),
    { .name = "CUU from below the region", .input = "\033[2;3r\033[4;1H\033[9AZ",
      .cols = 8, .rows = 4, .line = { "", "Z", "", "" }, AT(1, 1), NO_EXTRA },
    { .name = "CUD from above the region", .input = "\033[2;3r\033[1;1H\033[9BZ",
      .cols = 8, .rows = 4, .line = { "", "", "Z", "" }, AT(1, 2), NO_EXTRA },

    CASE("EL 0", "abcdef\033[1;3H\033[K", "ab", "", "", 2, 0),
    CASE("EL 1", "abcdef\033[1;3H\033[1K", "   def", "", "", 2, 0),
    CASE("EL 2", "abcdef\033[2K", "", "", "", 6, 0),
    CASE("ED 0", "abcd\r\nefgh\033[1;3H\033[J", "ab", "", "", 2, 0),
    CASE("ED 1", "abcd\r\nefgh\033[2;3H\033[1J", "", "   h", "", 2, 1),
    CASE("ECH", "abcdef\033[1;3H\033[2X", "ab  ef", "", "", 2, 0),
    CASE("ECH erases a whole wide glyph", "a中b\033[1;3H\033[X", "a  b", "", "", 2, 0),
    CASE("DCH", "abcdef\033[1;3H\033[2P", "abef", "", "", 2, 0),
    CASE("ICH", "abcdef\033[1;3H\033[2@", "ab  cdef", "", "", 2, 0),
    CASE("REP", "a\033[3b", "aaaa", "", "", 4, 0),
    CASE("insert mode", "abcd\r\033[4hZ", "Zabcd", "", "", 1, 0),
    CASE("insert mode at the row end", "abc\033[4hZ", "abcZ", "", "", 4, 0),
    CASE("IL", "a\r\nb\r\nc\033[2;3H\033[L", "a", "", "b", 0, 1),
    CASE("DL", "a\r\nb\r\nc\033[2;3H\033[M", "a", "c", "", 0, 1),
    CASE("SU", "a\r\nb\r\nc\033[S", "b", "c", "", 1, 2),
    CASE("SD", "a\r\nb\r\nc\033[T", "", "a", "b", 1, 2),
    CASE("CHT", "\033[1IZ", "       Z", "", "", 7, 0),
    CASE("CBT", "\033[2;3H\033[1ZZ", "", "Z", "", 1, 1),
    { .name = "ED 2 keeps text in history", .input = "x\r\nabc\r\033[3P\033[2J",
      .cols = 8, .rows = 3, ROWS3("", "", ""), AT(0, 1), .attr_x = -1, .history = 1 },
    { .name = "ED 2 after ECH reads cells", .input = "x\r\nabc\r\033[3X\033[2J",
      .cols = 8, .rows = 3, ROWS3("", "", ""), AT(0, 1), .attr_x = -1, .history = 1 },
    { .name = "ED 3", .input = "a\r\nb\r\nc\r\nd\033[3J", .cols = 8, .rows = 3,
      ROWS3("b", "c", "d"), AT(1, 2), .attr_x = -1, .history = 0 },

    CASE("IND", "a\033D Z", "a", "  Z", "", 3, 1),
    CASE("NEL", "abc\033EZ", "abc", "Z", "", 1, 1),
    CASE("RI", "\033[2;1HZ\033M", "", "Z", "", 1, 0),
    CASE("RI scrolls at the top", "a\033MZ", " Z", "a", "", 2, 0),
    CASE("save restore", "a\0337bc\0338Z", "aZc", "", "", 2, 0),
    CASE("restore without save goes home", "abc\0338Z", "Zbc", "", "", 1, 0),
    CASE("restore keeps the pending wrap", "abcdefgh\0337\r\0338Z", "abcdefgh", "Z", "", 1, 1),
    CASE("restore restores origin mode", "\033[2;3r\033[?6h\0337\033[?6l\0338\033[HZ",
         "", "Z", "", 1, 1),
    CASE("restore restores the character set", "\033(0\0337\033(B\0338x", "?", "", "", 1, 0),
    CASE("reset", "abc\033cZ", "Z", "", "", 1, 0),
    CASE("screen alignment", "\033#8", "EEEEEEEE", "EEEEEEEE", "EEEEEEEE", 0, 0),

    CASE("autowrap off", "\033[?7labcdefghi", "abcdefgi", "", "", 7, 0),
    CASE("alternate", "abc\033[?1049hZ", "Z", "", "", 1, 0),
    CASE("alternate restores", "abc\033[?1049hZ\033[?1049lQ", "abcQ", "", "", 4, 0),
    CASE("origin", "\033[2;3r\033[?6h\033[HZ", "", "Z", "", 1, 1),
    CASE("margin", "\033[2;3r\033[3;1Ha\033D", "", "a", "", 1, 2),
    CASE("line feed mode", "ab\033[20h\nZ", "ab", "Z", "", 1, 1),

    CASE("C0 inside CSI", "\033[2\n;3HZ", "", "  Z", "", 3, 1),
    CASE("CAN cancels CSI", "\033[31\x18Z", "Z", "", "", 1, 0),
    CASE("high byte cancels CSI", "\033[3\xffZ", "#Z", "", "", 2, 0),
    CASE("DEL inside CSI is ignored", "\033[\1772CZ", "  Z", "", "", 3, 0),
    CASE("OSC is swallowed", "a\033]0;title\007b", "ab", "", "", 2, 0),
    CASE("DCS is swallowed", "a\033Pxyz\033\\b", "ab", "", "", 2, 0),
    CASE("APC is swallowed", "a\033_x\033\\b", "ab", "", "", 2, 0),

    {
        .name = "SGR", .input = "\033[1;3;4;9;53;38;5;123;48;2;1;2;3mX",
        .cols = 8, .rows = 3, ROWS3("X", "", ""), AT(1, 0),
        .attr_x = 0, .attr_y = 0,
        .attrs = WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_ITALIC | WSAP_VTCORE_ATTR_STRIKE |
                 WSAP_VTCORE_ATTR_OVERLINE | WSAP_VTCORE_ATTR_UNDERLINE,
        .fg = WSAP_VTCORE_COLOR_SLOT | 123, .bg = 0xff010203u, .history = -1,
    },
    {
        .name = "SGR colon RGB with a color space", .input = "\033[38:2::1:2:3mX",
        .cols = 8, .rows = 3, ROWS3("X", "", ""), AT(1, 0),
        .attr_x = 0, .attr_y = 0, .fg = 0xff010203u, .bg = WSAP_VTCORE_COLOR_BG, .history = -1,
    },
    {
        .name = "SGR colon form is not two semicolon colors",
        .input = "\033[48:2::::38;2;;;mX",
        .cols = 8, .rows = 3, ROWS3("X", "", ""), AT(1, 0),
        .attr_x = 0, .attr_y = 0, .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG, .history = -1,
    },
    {
        .name = "restore restores the pen", .input = "\033[31m\0337\033[m\0338X",
        .cols = 8, .rows = 3, ROWS3("X", "", ""), AT(1, 0),
        .attr_x = 0, .attr_y = 0, .fg = RED, .bg = WSAP_VTCORE_COLOR_BG, .history = -1,
    },
    {
        .name = "SCORC restores the pen", .input = "\033[1m\033[s\033[m\033[uX",
        .cols = 8, .rows = 3, ROWS3("X", "", ""), AT(1, 0),
        .attr_x = 0, .attr_y = 0, .attrs = WSAP_VTCORE_ATTR_BOLD,
        .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG, .history = -1,
    },
    {
        .name = "restore without save resets the pen", .input = "\033[31m\0338X",
        .cols = 8, .rows = 3, ROWS3("X", "", ""), AT(1, 0),
        .attr_x = 0, .attr_y = 0, .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG, .history = -1,
    },
    {
        .name = "DECCARA on half a wide glyph", .input = "a中b\033[1;3;1;3;1$r",
        .cols = 8, .rows = 3, ROWS3("a?_b", "", ""), AT(4, 0),
        .attr_x = 1, .attr_y = 0, .attrs = WSAP_VTCORE_ATTR_BOLD,
        .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG, .history = -1,
    },
    {
        .name = "erase uses the pen background", .input = "ab\033[41m\033[1;1H\033[X",
        .cols = 8, .rows = 3, ROWS3(" b", "", ""), AT(0, 0),
        .attr_x = 0, .attr_y = 0, .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_SLOT | 1, .history = -1,
    },
    {
        .name = "full-screen scroll BCE", .input = "abc\033[44m\r\n\r\n\r\n",
        .cols = 8, .rows = 3, ROWS3("", "", ""), AT(0, 2),
        .attr_x = 0, .attr_y = 2,
        .fg = WSAP_VTCORE_COLOR_FG, .bg = BLUE, .history = -1,
    },

    { .name = "DSR 5", .input = "\033[5n", .cols = 8, .rows = 3, ROWS3("", "", ""),
      AT(0, 0), NO_EXTRA, .reply = "\033[0n" },
    { .name = "DSR 6", .input = "ab\033[6n", .cols = 8, .rows = 3, ROWS3("ab", "", ""),
      AT(2, 0), NO_EXTRA, .reply = "\033[1;3R" },
    { .name = "DSR 6 with a pending wrap", .input = "abcdefgh\033[6n", .cols = 8, .rows = 3,
      ROWS3("abcdefgh", "", ""), AT(7, 0), NO_EXTRA, .reply = "\033[1;8R" },
    { .name = "DSR 6 in origin mode", .input = "\033[2;3r\033[?6h\033[6n", .cols = 8, .rows = 3,
      ROWS3("", "", ""), AT(0, 1), NO_EXTRA, .reply = "\033[1;1R" },
    { .name = "DA1", .input = "\033[c\033[0c", .cols = 8, .rows = 3, ROWS3("", "", ""),
      AT(0, 0), NO_EXTRA, .reply = "\033[?62;22c\033[?62;22c" },
    { .name = "DA1 with a nonzero parameter", .input = "\033[1c", .cols = 8, .rows = 3,
      ROWS3("", "", ""), AT(0, 0), NO_EXTRA, .reply = "" },
    { .name = "DA2", .input = "\033[>c", .cols = 8, .rows = 3, ROWS3("", "", ""),
      AT(0, 0), NO_EXTRA, .reply = "\033[>0;10;0c" },
    { .name = "DECRQSS SGR", .input = "\033[1;31m\033P$qm\033\\", .cols = 8, .rows = 3,
      ROWS3("", "", ""), AT(0, 0), NO_EXTRA, .reply = "\033P1$r0;1;31m\033\\" },
    { .name = "DECRQSS underline color", .input = "\033[58;5;3m\033P$qm\033\\",
      .cols = 8, .rows = 3, ROWS3("", "", ""), AT(0, 0), NO_EXTRA,
      .reply = "\033P1$r0;58:5:3m\033\\" },
    { .name = "DECRQSS margins", .input = "\033[2;3r\033P$qr\033\\", .cols = 8, .rows = 3,
      ROWS3("", "", ""), AT(0, 0), NO_EXTRA, .reply = "\033P1$r2;3r\033\\" },
    { .name = "DECRQM", .input = "\033[4h\033[4$p\033[?7$p", .cols = 8, .rows = 3,
      ROWS3("", "", ""), AT(0, 0), NO_EXTRA, .reply = "\033[4;1$y\033[?7;1$y" },
};

struct replies {
    char s[512];
    size_t len;
};

static void on_reply(void *ud, const char *bytes, size_t len)
{
    struct replies *r = ud;
    if (r->len + len < sizeof(r->s)) {
        memcpy(r->s + r->len, bytes, len);
        r->len += len;
    }
}

static char cell_char(const struct wsap_vtcore_snapshot *s, const struct wsap_vtcore_cell *c)
{
    if (c->width == 0) return '_';
    uint32_t cps[4] = {0};
    int n = wsap_vtcore_snapshot_cluster(s, c->text, cps, 4);
    if (n != 1) return '?';
    if (cps[0] == 0xFFFD) return '#';
    return cps[0] >= 0x20 && cps[0] < 0x7f ? (char)cps[0] : '?';
}

static int run(const struct spec *c, struct wsap_vtcore_snapshot *s, bool bytes)
{
    struct replies got = {0};
    struct wsap_vtcore_options opt = {
        .size = { .x = c->cols, .y = c->rows }, .scrollback = 100,
        .callbacks = { .ud = &got, .reply = on_reply },
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    if (!t) return 1;
    size_t len = strlen(c->input);
    if (bytes)
        for (size_t i = 0; i < len; i++) wsap_vtcore_feed(t, c->input + i, 1, 0);
    else
        wsap_vtcore_feed(t, c->input, len, 0);
    const char *mode = bytes ? "byte by byte" : "whole";
    int bad = 0;
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(t);
    if (cursor.pos.x != c->x || cursor.pos.y != c->y) {
        fprintf(stderr, "%s (%s): cursor %d,%d; expected %d,%d\n", c->name, mode,
                cursor.pos.x, cursor.pos.y, c->x, c->y);
        bad = 1;
    }
    if (!wsap_vtcore_snapshot_update(s, t)) return 1;
    for (int y = 0; y < c->rows && y < 4; y++) {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, y);
        char text[64];
        int n = 0;
        for (int x = 0; x < c->cols && n < 63; x++) text[n++] = cell_char(s, &row[x]);
        while (n > 0 && text[n - 1] == ' ') n--;
        text[n] = 0;
        const char *want = c->line[y] ? c->line[y] : "";
        if (strcmp(text, want)) {
            fprintf(stderr, "%s (%s): row %d \"%s\"; expected \"%s\"\n", c->name, mode,
                    y, text, want);
            bad = 1;
        }
    }
    if (c->attr_x >= 0) {
        const struct wsap_vtcore_cell *cell = &wsap_vtcore_snapshot_row(s, c->attr_y)[c->attr_x];
        if (cell->attrs != c->attrs || cell->fg != c->fg || cell->bg != c->bg) {
            fprintf(stderr, "%s (%s): attributes %x fg %x bg %x; expected %x %x %x\n",
                    c->name, mode, cell->attrs, cell->fg, cell->bg, c->attrs, c->fg, c->bg);
            bad = 1;
        }
    }
    if (c->history >= 0 && wsap_vtcore_scrollback(t).total != c->history) {
        fprintf(stderr, "%s (%s): history %d; expected %d\n", c->name, mode,
                wsap_vtcore_scrollback(t).total, c->history);
        bad = 1;
    }
    if (c->reply && (got.len != strlen(c->reply) || memcmp(got.s, c->reply, got.len))) {
        fprintf(stderr, "%s (%s): reply differs\n", c->name, mode);
        bad = 1;
    }
    wsap_vtcore_free(t);
    return bad;
}

int main(void)
{
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    if (!s) return 1;
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failed += run(&cases[i], s, false) | run(&cases[i], s, true);

    struct wsap_vtcore_options opt = { .size = { .x = 8, .y = 2 } };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    if (!t) return 1;
    wsap_vtcore_feed(t, "ab\nZ", 4, WSAP_VTCORE_FEED_ONLCR);
    if (wsap_vtcore_cursor(t).pos.x != 1 || wsap_vtcore_cursor(t).pos.y != 1) {
        fprintf(stderr, "ONLCR: line feed did not return to column zero\n");
        failed++;
    }
    wsap_vtcore_free(t);

    opt.size = (struct wsap_vtcore_vector2i){ .x = 8, .y = 3 };
    opt.scrollback = 5;
    t = wsap_vtcore_new(&opt);
    if (!t) return 1;
    char *stream = malloc(13000u * 9u + 8u);
    if (!stream) return 1;
    for (int i = 0; i < 13000; i++) memcpy(stream + (size_t)i * 9, "1234567\r\n", 9);
    memcpy(stream + 13000u * 9u, "broken\rY", 8);
    wsap_vtcore_feed(t, stream, 13000u * 9u + 8u, 0);
    free(stream);
    if (wsap_vtcore_scrollback(t).total != 5) {
        fprintf(stderr, "long stream: history count differs\n");
        failed++;
    }
    for (int offset = 5;; offset = offset > 3 ? offset - 3 : 0) {
        wsap_vtcore_scroll_to(t, offset);
        if (!wsap_vtcore_snapshot_update(s, t)) return 1;
        for (int y = 0; y < 3; y++) {
            const char *want = offset == 0 && y == 2 ? "Yroken" : "1234567";
            const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, y);
            for (int x = 0; x < 8; x++) {
                uint32_t cp = x < (int)strlen(want) ? (unsigned char)want[x] : ' ';
                if (row[x].text != cp) {
                    fprintf(stderr, "long stream: offset %d cell %d,%d = %x, expected %x\n",
                            offset, x, y, row[x].text, cp);
                    failed++;
                    break;
                }
            }
        }
        if (!offset) break;
    }
    wsap_vtcore_free(t);
    wsap_vtcore_snapshot_free(s);
    if (failed) fprintf(stderr, "%d spec failures\n", failed);
    return failed != 0;
}
