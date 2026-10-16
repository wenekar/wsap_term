#include "term_api.h"
#include "wsap_vtcore/unicode/unicode.h"
#include "wsap_vtcore/unicode/utf8.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char response[64];
static size_t response_len;
static void respond(void *ud, const char *data, size_t len)
{
    (void)ud;
    assert(len < sizeof(response));
    memcpy(response, data, len);
    response[len] = 0;
    response_len = len;
}
static char title[128];
static void capture_title(void *ud, const char *s)
{
    (void)ud;
    snprintf(title, sizeof(title), "%s", s);
}
static const struct wsap_vtcore_callbacks reply_cb = { .reply = respond };
static const struct wsap_vtcore_callbacks title_cb = { .reply = respond, .title = capture_title };

static void write_seq(struct wsap_vtcore *t, const char *s)
{

    for (; *s; s++) tt_write(t, s, 1);
}
static void feed_seq(struct wsap_vtcore *t, const char *s, unsigned flags)
{
    for (; *s; s++) wsap_vtcore_feed(t, s, 1, flags);
}
static const char *row_text(struct wsap_vtcore *t, int r, char *buf)
{
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(tt_snapshot_for(t), r);
    int n = 0;
    for (int c = 0; c < tt_cols(t); c++) {
        uint32_t cp = tt_cell_cp(t, &cells[c]);
        buf[n++] = !cp ? ' ' : cp < 0x80 ? (char)cp : '?';
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = 0;
    return buf;
}
static bool row_eq(struct wsap_vtcore *t, int r, const char *want)
{
    char *got = malloc((size_t)tt_cols(t) + 1);
    assert(got);
    bool equal = !strcmp(row_text(t, r, got), want);
    free(got);
    return equal;
}
static bool row_is(struct wsap_vtcore *t, int r, const char *want)
{
    char *got = malloc((size_t)tt_cols(t) + 1);
    assert(got);
    bool equal = !strcmp(row_text(t, r, got), want);
    if (!equal)
        fprintf(stderr, "row %d: got \"%s\" want \"%s\"\n", r, got, want);
    free(got);
    return equal;
}

static void write_both(struct wsap_vtcore *whole, struct wsap_vtcore *bytes, const char *s)
{
    tt_write(whole, s, strlen(s));
    write_seq(bytes, s);
}

static uint64_t view_version(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t)
{
    wsap_vtcore_snapshot_update(s, t);
    return wsap_vtcore_snapshot_view(s)->version;
}

static void test_skipped_cells_read_blank(void)
{
    static const char *const lines[] = {
        "ab\033[1Ccd\033[3Cef",
        "a…b\033[1Cc",
        "\033[1m\033[38;2;1;2;3mab\033[0m\033[2Ccd",
    };
    static const char *const want[] = { "ab cd   ef", "a?b c", "ab  cd" };
    for (size_t k = 0; k < sizeof(lines) / sizeof(lines[0]); k++) {
        struct wsap_vtcore *whole = tt_new(20, 4);
        struct wsap_vtcore *bytes = tt_new(20, 4);
        for (int i = 0; i < 12; i++)
            write_both(whole, bytes, "XXXXXXXXXXXXXXXXXXXX\r\n");
        write_both(whole, bytes, lines[k]);
        for (struct wsap_vtcore *t = whole; t; t = t == whole ? bytes : NULL) {
            char *got = malloc((size_t)tt_cols(t) + 1);
            assert(got);
            row_text(t, 3, got);
            if (strcmp(got, want[k])) {
                fprintf(stderr, "line %zu (%s): got \"%s\" want \"%s\"\n", k,
                        t == whole ? "whole" : "bytes", got, want[k]);
                assert(0);
            }
            free(got);
        }
        wsap_vtcore_free(whole);
        wsap_vtcore_free(bytes);
    }

    struct wsap_vtcore *whole = tt_new(20, 4);
    struct wsap_vtcore *bytes = tt_new(20, 4);
    write_both(whole, bytes, "abcdefgh\r\033[2C\033[K\033[2CZ");
    assert(row_is(whole, 0, "ab  Z"));
    assert(row_is(bytes, 0, "ab  Z"));
    wsap_vtcore_free(whole);
    wsap_vtcore_free(bytes);
}

static void assert_same_rows(struct wsap_vtcore *a, struct wsap_vtcore *b, int rows)
{
    struct wsap_vtcore_snapshot *sa = wsap_vtcore_snapshot_new();
    struct wsap_vtcore_snapshot *sb = wsap_vtcore_snapshot_new();
    bool copied = sa && sb && wsap_vtcore_snapshot_update(sa, a) && wsap_vtcore_snapshot_update(sb, b);
    assert(copied);
    int cols = wsap_vtcore_snapshot_view(sa)->size.x;
    assert(cols == wsap_vtcore_snapshot_view(sb)->size.x);
    for (int y = 0; y < rows; y++) {
        const struct wsap_vtcore_cell *ra = wsap_vtcore_snapshot_row(sa, y);
        const struct wsap_vtcore_cell *rb = wsap_vtcore_snapshot_row(sb, y);
        assert(ra && rb);
        for (int x = 0; x < cols; x++) {
            const struct wsap_vtcore_cell *ac = &ra[x], *bc = &rb[x];
            uint32_t acp[32], bcp[32];
            int an = wsap_vtcore_snapshot_cluster(sa, ac->text, acp, 32);
            int bn = wsap_vtcore_snapshot_cluster(sb, bc->text, bcp, 32);
            bool same = an == bn && an <= 32 &&
                        !memcmp(acp, bcp, (size_t)an * sizeof(*acp)) &&
                        ac->fg == bc->fg && ac->bg == bc->bg &&
                        ac->attrs == bc->attrs && ac->width == bc->width;
            if (!same) {
                fprintf(stderr, "x=%d y=%d: a(cp=%x fg=%x bg=%x attrs=%x width=%d) "
                        "b(cp=%x fg=%x bg=%x attrs=%x width=%d)\n", x, y,
                        an ? acp[0] : 0, ac->fg, ac->bg, ac->attrs, ac->width,
                        bn ? bcp[0] : 0, bc->fg, bc->bg, bc->attrs, bc->width);
                assert(0);
            }
        }
    }
    wsap_vtcore_snapshot_free(sa);
    wsap_vtcore_snapshot_free(sb);
}

static void assert_same_screen(struct wsap_vtcore *a, struct wsap_vtcore *b)
{
    assert(tt_cursor_x(a) == tt_cursor_x(b));
    assert(tt_cursor_y(a) == tt_cursor_y(b));
    assert(tt_history(a) == tt_history(b));
    assert(wsap_vtcore_modes(a).synchronized == wsap_vtcore_modes(b).synchronized);
    assert(tt_rows(a) == tt_rows(b));
    assert_same_rows(a, b, tt_rows(a));
}

static void test_read_boundaries(void)
{
    static const char input[] =
        "ASCII 1234567890\r\nT\xc3\xbc" "rk\xe2\x94\x80"
        "\033[31mred\033[m\033(0lqqk\033(B"
        "\033[2;3H!\033[2b\033[1D?\033[K"
        "\033[?1049halt\033[?1049lZ"
        "\033]11;?\007\033Pignored\033\\"
        "\xf0\x9f\x98\x80\xc3XQ©️界\033[2G\033[P";
    struct wsap_vtcore *expected = tt_new(16, 4);
    tt_write(expected, input, sizeof(input) - 1);
    for (size_t split = 0; split < sizeof(input); split++) {
        struct wsap_vtcore *actual = tt_new(16, 4);
        tt_write(actual, input, split);
        tt_write(actual, input + split, sizeof(input) - 1 - split);
        assert_same_screen(actual, expected);
        wsap_vtcore_free(actual);
    }

    struct wsap_vtcore *t = tt_new(16, 4);
    write_seq(t, "\xc3XQ");
    assert(tt_cell_cp(t, tt_cell(t, 0, 0)) == 0xFFFD);
    assert(tt_cell_cp(t, tt_cell(t, 1, 0)) == 'X');
    assert(tt_cell_cp(t, tt_cell(t, 2, 0)) == 'Q');
    wsap_vtcore_free(t);
    wsap_vtcore_free(expected);
}

static void test_ascii_controls(void)
{
    static const char *separators[] = {"\n", "\r", "\b", "\t", "\033[31m", "\177", "é界"};
    for (int bottom = 0; bottom < 2; bottom++) {
        for (size_t c = 0; c < sizeof(separators) / sizeof(separators[0]); c++) {
            char input[256];
            memset(input, 'a', 29);
            size_t n = strlen(separators[c]);
            memcpy(input + 29, separators[c], n);
            memset(input + 29 + n, 'b', 173);
            input[202 + n] = '\0';
            struct wsap_vtcore *bulk = tt_new(120, 6);
            struct wsap_vtcore *bytes = tt_new(120, 6);
            const char *setup = bottom ? "\033[6;1H\033[1;3;4;38;2;12;34;56m"
                                       : "\033[1;3;4;38;2;12;34;56m";
            write_seq(bulk, setup);
            write_seq(bytes, setup);
            tt_write(bulk, input, strlen(input));
            write_seq(bytes, input);
            assert_same_screen(bulk, bytes);
            wsap_vtcore_scroll(bulk, 6);
            wsap_vtcore_scroll(bytes, 6);
            assert_same_screen(bulk, bytes);
            wsap_vtcore_free(bulk);
            wsap_vtcore_free(bytes);
        }
    }
}

static void test_esc_control_strings_and_designators(void)
{
    struct wsap_vtcore *t = tt_new(32, 2);
    assert(t);

    write_seq(t, "A\033_payload ignored\033\\B");
    write_seq(t, "\033^privacy message\007C");
    write_seq(t, "\033Xsos ignored\033\\D");
    assert(row_is(t, 0, "ABCD"));

    write_seq(t, "\033%Gx\033%@y\033%/z");
    assert(row_is(t, 0, "ABCDxyz"));
    write_seq(t, "\033%qx");
    assert(row_is(t, 0, "ABCDxyz%qx"));

    write_seq(t, "\033.Am\033*0n");
    assert(row_is(t, 0, "ABCDxyz%qxmn"));
    wsap_vtcore_free(t);

    t = tt_new(8, 2);
    write_seq(t, "A\033_ignored\033DB");
    assert(row_is(t, 0, "A"));
    assert(row_is(t, 1, " B"));
    wsap_vtcore_free(t);
}

static void test_random_read_boundaries(void)
{
    static const char embedded[] =
        "\033\\\033(B\r\nA\xc3\xbc\xe7\x95\x8c" "e\xcc\x81"
        "\033[31mR\033[m\tX\bY\033]0;title\007\033Pignored\033\\"
        "\033[?2026hS\033[?2026l\xc3XQ\xe0\x80\x80\xf4\x90\x80\x80";
    static const size_t chunks[] = {1, 2, 7, 31, 127};
    for (unsigned seed = 1; seed <= 4; seed++) {
        char input[1024];
        unsigned random = seed;
        for (size_t i = 0; i < sizeof(input); i++) {
            random = random * 1103515245u + 12345u;
            input[i] = (char)(random >> 16);
        }
        memcpy(input + 400, embedded, sizeof(embedded) - 1);
        struct wsap_vtcore *whole = tt_new(32, 8);
        tt_write(whole, input, sizeof(input));
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            struct wsap_vtcore *a = tt_new(32, 8);
            struct wsap_vtcore *b = tt_new(32, 8);
            for (size_t block = 0; block < sizeof(input); block += 128) {
                tt_write(a, input + block, 128);
                for (size_t i = block; i < block + 128;) {
                    size_t n = chunks[c];
                    if (n > block + 128 - i) n = block + 128 - i;
                    tt_write(b, input + i, n);
                    i += n;
                }
                assert_same_screen(a, b);
            }
            assert_same_screen(whole, a);
            write_seq(a, "\033\\\033(B\r\nEND");
            write_seq(b, "\033\\\033(B\r\nEND");
            assert_same_screen(a, b);
            int history = tt_history(a);
            for (int i = 0; i < history; i += 8) {
                wsap_vtcore_scroll(a, 8);
                wsap_vtcore_scroll(b, 8);
                assert_same_screen(a, b);
            }
            wsap_vtcore_free(a);
            wsap_vtcore_free(b);
        }
        wsap_vtcore_free(whole);
    }
}

static void test_csi_recovery(void)
{
    static const struct { const char *input, *first, *second; } cases[] = {
        {"A\033[\r\nB!", "A", ""},
        {"A\033[\xff" "BC", "A?BC", ""},
        {"A\033[\033[2JZ", " Z", ""},
        {"\033[ 31mX", "X", ""},
        {"\033[31$mX", "X", ""},
        {"\033[31;?mX", "X", ""},
        {"\033[31;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0mX", "X", ""},
        {"\033[31\030X", "X", ""},
        {"\033[31\032X", "X", ""},
        {"\033[1$ A X", " X", ""},
        {"AB\033[?2JX", "ABX", ""},
        {"AB\r\033[>4hX", "XB", ""},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t len = strlen(cases[c].input);
        for (size_t split = 0; split <= len; split++) {
            struct wsap_vtcore *t = tt_new(20, 4);
            tt_write(t, cases[c].input, split);
            tt_write(t, cases[c].input + split, len - split);
            assert(row_is(t, 0, cases[c].first));
            assert(row_is(t, 1, cases[c].second));
            for (int x = 0; x < 20; x++) assert(tt_cell(t, x, 0)->fg == WSAP_VTCORE_COLOR_FG);
            wsap_vtcore_free(t);
        }
    }
}

static void test_csi_byte_boundaries(void)
{
    static const char *prefixes[] = {"\033[", "\033[31;", "\033[?", "\033[1 ", "\033[1$ "};
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        for (unsigned byte = 0; byte < 256; byte++) {
            char input[64];
            size_t n = strlen(prefixes[p]);
            memcpy(input, prefixes[p], n);
            input[n++] = (char)byte;
            static const char suffix[] = "mX\033[6n\033[?25l\033[?25h\033[0mY";
            memcpy(input + n, suffix, sizeof(suffix) - 1);
            n += sizeof(suffix) - 1;
            struct wsap_vtcore *whole = tt_new_opts(20, 4, &reply_cb);
            response_len = 0;
            tt_write(whole, input, n);
            char expected_response[64];
            size_t expected_len = response_len;
            memcpy(expected_response, response, expected_len);
            for (size_t split = 0; split <= strlen(prefixes[p]) + 2; split++) {
                struct wsap_vtcore *parts = tt_new_opts(20, 4, &reply_cb);
                response_len = 0;
                tt_write(parts, input, split);
                tt_write(parts, input + split, n - split);
                assert_same_screen(whole, parts);
                assert(wsap_vtcore_cursor(whole).visible == wsap_vtcore_cursor(parts).visible);
                assert(response_len == expected_len);
                assert(!memcmp(response, expected_response, expected_len));
                wsap_vtcore_free(parts);
            }
            wsap_vtcore_free(whole);
        }
    }
}

static void test_parameter_limits(void)
{
    struct wsap_vtcore *t = tt_new(10, 4);
    write_seq(t, "\033[2;4r\033[?6h\033[999999999999999999999;1H");
    assert(tt_cursor_y(t) == 3 && tt_cursor_x(t) == 0);
    write_seq(t, "\033[?6l\033[r");
    write_seq(t, "\033[2;2H\033[999999999999999999999B\033[2147483647C!");
    assert(tt_cursor_y(t) == 3 && tt_cursor_x(t) == 9);
    assert(tt_cell(t, 9, 3)->text == '!');
    write_seq(t, "\033[999999999999999999999A\033[999999999999999999999D");
    assert(tt_cursor_y(t) == 0 && tt_cursor_x(t) == 0);
    write_seq(t, "\033[38;5;999999999999999999999mX");
    assert(tt_cell(t, 0, 0)->fg == WSAP_VTCORE_COLOR_FG);

    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    uint64_t version = view_version(snap, t);
    write_seq(t, "\033[3d");
    assert(tt_cursor_y(t) == 2 && view_version(snap, t) != version);
    wsap_vtcore_snapshot_free(snap);

    write_seq(t, "\033[4;8H\0337");
    tt_resize(t, 5, 2);
    write_seq(t, "\0338Z");
    assert(tt_cursor_y(t) == 1 && tt_cursor_x(t) == 3);
    assert(tt_cell(t, 2, 1)->text == 'Z');
    wsap_vtcore_free(t);
}

static void test_unterminated_control_strings_do_not_swallow_text(void)
{
    struct wsap_vtcore *t = tt_new(20, 4);
    assert(t);

    write_seq(t, "A\033]0;random bytes\tgit\r\nstatus");
    assert(row_is(t, 0, "A       git"));
    assert(row_is(t, 1, "status"));

    write_seq(t, "\033Prandom bytes\tbranch\r\nnext");
    assert(row_is(t, 1, "status  branch"));
    assert(row_is(t, 2, "next"));

    wsap_vtcore_free(t);

    char overlong[1200];
    for (int intro = 0; intro < 2; intro++) {
        int n = snprintf(overlong, sizeof(overlong), intro ? "\033P" : "\033]0;");
        memset(overlong + n, 'T', 1024);
        overlong[n + 1024] = '\0';

        struct wsap_vtcore *u = tt_new(20, 4);
        write_seq(u, overlong);
        write_seq(u, "\r\n[sudo] password:");
        assert(row_is(u, 0, ""));
        assert(row_is(u, 1, "[sudo] password:"));
        wsap_vtcore_free(u);

        struct wsap_vtcore *d = tt_new(20, 4);
        write_seq(d, overlong);
        write_seq(d, "\033\\after");
        assert(row_is(d, 0, "after"));
        wsap_vtcore_free(d);
    }
}

static void test_c0_controls_after_esc_are_executed(void)
{
    struct wsap_vtcore *t = tt_new(20, 4);
    assert(t);

    write_seq(t, "A\033\tB");
    assert(row_is(t, 0, "A       B"));

    wsap_vtcore_free(t);
}

static void test_resize_reflow(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    write_seq(t, "abcdefghijkl");
    tt_resize(t, 16, 4);
    assert(row_is(t, 0, "abcdefghijkl") && row_is(t, 1, ""));
    assert(tt_cursor_y(t) == 0 && tt_cursor_x(t) == 12);
    write_seq(t, "M");
    assert(row_is(t, 0, "abcdefghijklM"));
    tt_resize(t, 5, 4);
    assert(row_is(t, 0, "abcde") && row_is(t, 1, "fghij") && row_is(t, 2, "klM"));
    assert(tt_cursor_y(t) == 2 && tt_cursor_x(t) == 3);
    tt_select(t, 0, 0, 2, 2);
    char *text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "abcdefghijklM"));
    free(text);
    tt_resize(t, 20, 4);
    assert(row_is(t, 0, "abcdefghijklM"));
    wsap_vtcore_free(t);

    t = tt_new(6, 6);
    write_seq(t, "\033[31mab  cdef\033[m\r\nnext");
    wsap_vtcore_color red = tt_cell(t, 0, 0)->fg;
    tt_resize(t, 12, 6);
    assert(row_is(t, 0, "ab  cdef") && row_is(t, 1, "next"));
    assert(tt_cell(t, 7, 0)->fg == red);
    assert(tt_cell(t, 0, 1)->fg == WSAP_VTCORE_COLOR_FG);
    wsap_vtcore_free(t);

    t = tt_new(4, 2);
    write_seq(t, "abcdefghijkl");
    assert(tt_history(t) == 1);
    tt_resize(t, 12, 2);
    assert(tt_history(t) == 0 && row_is(t, 0, "abcdefghijkl"));
    assert(tt_cursor_y(t) == 0 && tt_cursor_x(t) == 11);
    write_seq(t, "M");
    assert(row_is(t, 1, "M"));
    tt_resize(t, 4, 2);
    assert(tt_history(t) == 2);
    assert(row_is(t, 0, "ijkl") && row_is(t, 1, "M"));
    tt_resize(t, 20, 2);
    assert(row_is(t, 0, "abcdefghijklM"));
    wsap_vtcore_free(t);

    t = tt_new(4, 3);
    write_seq(t, "abcd");
    tt_resize(t, 8, 3);
    write_seq(t, "E");
    assert(row_is(t, 0, "abcdE"));
    tt_resize(t, 5, 3);
    write_seq(t, "F");
    assert(row_is(t, 0, "abcdE") && row_is(t, 1, "F"));
    wsap_vtcore_free(t);

    t = tt_new(4, 4);
    write_seq(t, "abcdefghijkl\033[?1049h12345678");
    tt_resize(t, 8, 4);
    assert(row_is(t, 0, "1234") && row_is(t, 1, "5678"));
    tt_resize(t, 16, 4);
    write_seq(t, "\033[?1049lM");
    assert(row_is(t, 0, "abcdefghijklM"));
    assert(tt_cursor_y(t) == 0 && tt_cursor_x(t) == 13);
    wsap_vtcore_free(t);

    t = tt_new(4, 2);
    write_seq(t, "abcd");
    tt_resize(t, 4, 3);
    write_seq(t, "E");
    assert(row_is(t, 0, "abcd") && row_is(t, 1, "E"));
    wsap_vtcore_free(t);

    t = tt_new(80, 4);
    for (int i = 0; i < 1000; i++) write_seq(t, "abcdefgh");
    tt_resize(t, 1, 4);
    assert(tt_history(t) == 1000 * 8 - 4);
    assert(tt_cursor_y(t) == 3 && tt_cursor_x(t) == 0);
    assert(row_is(t, 0, "e") && row_is(t, 3, "h"));
    tt_resize(t, 80, 4);
    assert(tt_cursor_x(t) < 80 && tt_cursor_y(t) < 4);
    write_seq(t, "Z");
    assert(tt_cell(t, 0, 3)->text == 'Z');
    wsap_vtcore_free(t);

}

static void test_resize_exposes_blank_rows(void)
{
    struct wsap_vtcore *t = tt_new(20, 2);
    write_seq(t, "kept");
    for (int pass = 0; pass < 2; pass++) {
        tt_resize(t, 20, 6);
        assert(row_is(t, 0, "kept"));
        for (int y = 2; y < 6; y++) {
            for (int x = 0; x < 20; x++) {
                const struct wsap_vtcore_cell *cell = tt_cell(t, x, y);
                assert(cell->text == ' ' && cell->fg == WSAP_VTCORE_COLOR_FG);
                assert(cell->bg == WSAP_VTCORE_COLOR_BG && cell->attrs == 0 && cell->width == 1);
            }
        }
        write_seq(t, "\033[6;1H\tfile/");
        assert(row_is(t, 5, "        file/"));
        write_seq(t, "\033[1;1H");
        tt_resize(t, 20, 2);
    }
    wsap_vtcore_free(t);
}

static void test_prompt_resize(void)
{
    struct wsap_vtcore *t = tt_new(80, 6);
    const char *prompt = "~/Documents/GitHub/clernen_pi main> ";
    write_seq(t, "\033]133;A;click_events=1\033\\");
    write_seq(t, prompt);
    write_seq(t, "\033]133;B\033\\");

    const struct { int cols; const char *text; bool newline; } cases[] = {
        {40, "~/Documents/GitHub/clernen_pi main> ", false},
        {20, ".b/clernen_pi main> ", true},
        {12, ".n_pi main> ", true},
        {30, "~/GitHub/clernen_pi> ", false},
        {80, "~/Documents/GitHub/clernen_pi main> ", false},
    };
    bool newline = false;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tt_resize(t, cases[i].cols, 6);
        write_seq(t, "\r\r");
        if (newline) write_seq(t, "\033[A");
        write_seq(t, "\033]133;A;click_events=1\033\\");
        write_seq(t, cases[i].text);
        write_seq(t, "\033]133;B\033\\");
        if (cases[i].newline) write_seq(t, "\r\n");
        write_seq(t, "\033[J");
        char expected[80];
        snprintf(expected, sizeof(expected), "%s", cases[i].text);
        expected[strlen(expected) - 1] = 0;
        assert(row_is(t, 0, expected));
        for (int y = 1; y < 6; y++) assert(row_is(t, y, ""));
        assert(tt_history(t) == 0);
        newline = cases[i].newline;
    }

    write_seq(t, "\033]133;C\033\\");
    tt_resize(t, 20, 6);
    assert(row_is(t, 0, "~/Documents/GitHub/c"));
    assert(row_is(t, 1, "lernen_pi main>"));
    wsap_vtcore_free(t);

    t = tt_new(20, 6);
    write_seq(t, "abcdefghijklmnop\r\n\033]133;A\007prompt> input\033]133;B\007");
    tt_resize(t, 8, 6);
    assert(row_is(t, 0, "abcdefgh") && row_is(t, 1, "ijklmnop"));
    assert(row_is(t, 2, "prompt>"));
    tt_resize(t, 30, 6);
    assert(row_is(t, 0, "abcdefghijklmnop") && row_is(t, 1, "prompt>"));
    wsap_vtcore_free(t);
}

static void test_cursor_line_motion(void)
{
    struct wsap_vtcore *t = tt_new(80, 9);
    assert(t);
    const char *lines = "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\nseven\r\n";
    tt_write(t, lines, strlen(lines));
    const char *rewrite = "\033[6Ftwo revised\033[K\033[2Efour revised\033[K\033[3Fone revised\033[K";
    tt_write(t, rewrite, strlen(rewrite));
    assert(row_is(t, 1, "two revised"));
    assert(row_is(t, 3, "four revised"));
    assert(row_is(t, 0, "one revised"));
    wsap_vtcore_free(t);
}

static void test_relative_cursor_motion(void)
{
    struct wsap_vtcore *t = tt_new(10, 5);
    assert(t);
    const char *text = "0123456789\r\nabcdefghij\r\nklmnopqrst";
    tt_write(t, text, strlen(text));
    const char *sequence = "\033[3`X\033[2aY\033[1eZ\033[2dW";
    tt_write(t, sequence, strlen(sequence));
    assert(tt_cell(t, 2, 2)->text == 'X');
    assert(tt_cell(t, 5, 2)->text == 'Y');
    assert(tt_cell(t, 6, 3)->text == 'Z');
    assert(tt_cell(t, 7, 1)->text == 'W');
    wsap_vtcore_free(t);
}

static void test_top_region_scrollback(void)
{
    static const struct {
        const char *scroll;
        int lines;
    } cases[] = {
        {"\n", 1},
        {"\033D", 1},
        {"\033E", 1},
        {"\033[S", 1},
        {"\033[2S", 2},
        {"\033[99S", 4},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct wsap_vtcore *t = tt_new(12, 6);
        write_seq(t, "one\r\ntwo\r\nthree\r\nfour\r\nPROMPT\r\nSTATUS");
        write_seq(t, "\033[1;4r\033[4;1H");
        write_seq(t, cases[i].scroll);
        assert(tt_history(t) == cases[i].lines);
        assert(row_is(t, 0, cases[i].lines == 1 ? "two" :
                            cases[i].lines == 2 ? "three" : ""));
        assert(row_is(t, 3, ""));
        assert(row_is(t, 4, "PROMPT") && row_is(t, 5, "STATUS"));
        wsap_vtcore_scroll(t, cases[i].lines);
        assert(tt_view_offset(t) == cases[i].lines);
        assert(row_is(t, 0, "one") && row_is(t, 1, "two"));
        wsap_vtcore_free(t);
    }

    struct wsap_vtcore *t = tt_new(12, 6);
    struct wsap_vtcore *reference = tt_new(12, 4);
    const char *footer = "\033[5;1H\033[31mPROMPT\r\nSTATUS\033[m\033[1;4r";
    tt_write(t, footer, strlen(footer));
    for (int i = 0; i < 2 * TEST_SCROLLBACK; i++) {
        char line[32];
        int len = snprintf(line, sizeof(line), "line %04d\r\n", i);
        tt_write(t, line, (size_t)len);
        tt_write(reference, line, (size_t)len);
    }
    assert(tt_history(t) == tt_history(reference));
    assert(tt_cursor_x(t) == tt_cursor_x(reference));
    assert(tt_cursor_y(t) == tt_cursor_y(reference));
    assert(row_is(t, 4, "PROMPT") && row_is(t, 5, "STATUS"));
    assert(tt_cell(t, 0, 4)->fg == tt_cell(t, 0, 5)->fg);
    assert(tt_cell(t, 0, 4)->fg != WSAP_VTCORE_COLOR_FG);
    for (int i = 0; i <= tt_history(reference); i++) {
        assert_same_rows(t, reference, tt_rows(reference));
        wsap_vtcore_scroll(t, 1);
        wsap_vtcore_scroll(reference, 1);
    }
    wsap_vtcore_free(t);
    wsap_vtcore_free(reference);
}

static void test_editor_scrolling(void)
{
    struct wsap_vtcore *t = tt_new(80, 24);
    write_seq(t, "\033[?1049hTITLE");
    for (int i = 1; i <= 20; i++) {
        char line[64];
        snprintf(line, sizeof(line), "\033[%d;1Hline %02d", i + 1, i);
        write_seq(t, line);
    }
    write_seq(t, "\033[23;1HHELP\033[24;1HFOOTER");

    write_seq(t, "\033[4;22r\033[22;1H\n\033[1;24r"
                 "\033[21;1Hline 21\033[K\r\033[4d");
    assert(row_is(t, 0, "TITLE") && row_is(t, 2, "line 02"));
    assert(row_is(t, 3, "line 04") && row_is(t, 19, "line 20"));
    assert(row_is(t, 20, "line 21") && row_is(t, 21, ""));
    assert(row_is(t, 22, "HELP") && row_is(t, 23, "FOOTER"));
    assert(tt_cursor_x(t) == 0 && tt_cursor_y(t) == 3);
    assert(tt_history(t) == 0);
    wsap_vtcore_free(t);

    t = tt_new(12, 6);
    write_seq(t, "old\r\nHEAD\r\nA\r\nB\r\nC\r\nD\r\nFOOT");
    assert(tt_history(t) == 1);
    write_seq(t, "\033[2;5r\033[3;2H\033[M");
    assert(row_is(t, 0, "HEAD") && row_is(t, 1, "A"));
    assert(row_is(t, 2, "C") && row_is(t, 3, "D") && row_is(t, 4, ""));
    assert(row_is(t, 5, "FOOT"));
    assert(tt_cursor_x(t) == 0 && tt_cursor_y(t) == 2);
    write_seq(t, "\033[L");
    assert(row_is(t, 2, "") && row_is(t, 3, "C") && row_is(t, 4, "D"));
    write_seq(t, "\033[2;1H\033M");
    assert(row_is(t, 1, "") && row_is(t, 2, "A") && row_is(t, 4, "C"));
    write_seq(t, "\033[5;1H\033D");
    assert(row_is(t, 1, "A") && row_is(t, 3, "C") && row_is(t, 4, ""));
    write_seq(t, "\033[T\033[S");
    assert(row_is(t, 1, "A") && row_is(t, 3, "C"));

    write_seq(t, "\033[5;2r\033[1;999999999999r\033[44m\033[999999999999S");
    assert(row_is(t, 0, "HEAD") && row_is(t, 5, "FOOT"));
    for (int r = 1; r <= 4; r++) {
        assert(row_is(t, r, ""));
        assert(tt_cell(t, 0, r)->bg == tt_cell(t, 11, r)->bg);
        assert(tt_cell(t, 0, r)->bg != WSAP_VTCORE_COLOR_BG);
    }
    assert(tt_history(t) == 1);

    write_seq(t, "\033[6;1H\033[M\033[L");
    assert(row_is(t, 5, "FOOT"));

    tt_resize(t, 12, 4);
    write_seq(t, "\033[4;1H\033[2Kend\n");
    assert(tt_cursor_y(t) == 3 && row_is(t, 2, "end"));
    wsap_vtcore_free(t);
}

static bool pending_decsca_decsed_decsel(void)
{
    struct wsap_vtcore *d = tt_new(20, 3);
    write_seq(d, "\033[H" "AB" "\033[1\"q" "CD" "\033[0\"q" "EF");
    write_seq(d, "\033[?2J");
    bool ok = row_eq(d, 0, "  CD");
    wsap_vtcore_free(d);

    struct wsap_vtcore *l = tt_new(20, 3);
    write_seq(l, "\033[H" "GH" "\033[1\"q" "IJ" "\033[0\"q" "KL");
    write_seq(l, "\033[H\033[?0K");
    ok = ok && row_eq(l, 0, "  IJ");
    wsap_vtcore_free(l);
    return ok;
}

static bool pending_decfra_decera(void)
{
    struct wsap_vtcore *f = tt_new(20, 3);
    write_seq(f, "\033[88;1;2;2;4$x");
    bool ok = row_eq(f, 0, " XXX") && row_eq(f, 1, " XXX");
    wsap_vtcore_free(f);

    struct wsap_vtcore *e = tt_new(20, 3);
    write_seq(e, "\033[Habcde\r\nfghij");
    write_seq(e, "\033[1;2;2;3$z");
    ok = ok && row_eq(e, 0, "a  de") && row_eq(e, 1, "f  ij");
    wsap_vtcore_free(e);
    return ok;
}

static bool pending_deccra(void)
{
    struct wsap_vtcore *t = tt_new(20, 3);
    write_seq(t, "\033[HAB\r\nCD");
    write_seq(t, "\033[1;1;2;2;1;1;5;1$v");
    bool ok = row_eq(t, 0, "AB  AB") && row_eq(t, 1, "CD  CD");
    wsap_vtcore_free(t);
    return ok;
}

static bool pending_decrara(void)
{
    struct wsap_vtcore *t = tt_new(20, 3);
    write_seq(t, "\033[H\033[1mAB\033[mCD");
    write_seq(t, "\033[1;1;1;4;1$t");
    bool ok = !(tt_cell(t, 0, 0)->attrs & WSAP_VTCORE_ATTR_BOLD) &&
              (tt_cell(t, 2, 0)->attrs & WSAP_VTCORE_ATTR_BOLD);
    wsap_vtcore_free(t);
    return ok;
}

static bool pending_decsera(void)
{
    struct wsap_vtcore *t = tt_new(20, 3);
    write_seq(t, "\033[H\033[0\"qAB\033[1\"qCD");
    write_seq(t, "\033[1;1;1;4${");
    bool ok = row_eq(t, 0, "  CD");
    wsap_vtcore_free(t);
    return ok;
}

static bool pending_deccolm(void)
{
    struct wsap_vtcore *t = tt_new(80, 4);
    write_seq(t, "\033[?40h" "keep" "\033[?3h");
    bool ok = tt_cols(t) == 132 && row_eq(t, 0, "") &&
              tt_cursor_x(t) == 0 && tt_cursor_y(t) == 0;
    write_seq(t, "\033[?3l");
    ok = ok && tt_cols(t) == 80;
    wsap_vtcore_free(t);
    return ok;
}

static bool pending_decic_decdc(void)
{
    struct wsap_vtcore *i = tt_new(10, 2);
    write_seq(i, "\033[Habcdef\033[H\033[2'}");
    bool ok = row_eq(i, 0, "  abcdef");
    wsap_vtcore_free(i);

    struct wsap_vtcore *d = tt_new(10, 2);
    write_seq(d, "\033[Habcdef\033[H\033[2'~");
    ok = ok && row_eq(d, 0, "cdef");
    wsap_vtcore_free(d);
    return ok;
}

static bool pending_sl_sr(void)
{
    struct wsap_vtcore *l = tt_new(10, 2);
    write_seq(l, "\033[Habcdef\033[2 @");
    bool ok = row_eq(l, 0, "cdef");
    wsap_vtcore_free(l);

    struct wsap_vtcore *r = tt_new(10, 2);
    write_seq(r, "\033[Habcdef\033[2 A");
    ok = ok && row_eq(r, 0, "  abcdef");
    wsap_vtcore_free(r);
    return ok;
}

static bool pending_sixel(void)
{
    struct wsap_vtcore *t = tt_new(20, 6);
    write_seq(t, "\033[H\033Pq#0;2;100;100;100#0~~~~\033\\");
    bool ok = tt_cursor_y(t) > 0;
    wsap_vtcore_free(t);
    return ok;
}

static void test_unimplemented_da_features(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } cases[] = {
        { "DECSCA/DECSED/DECSEL selective erase (DA1 6)", pending_decsca_decsed_decsel },
        { "DECFRA/DECERA rectangle fill, erase (DA1 28)", pending_decfra_decera },
        { "DECCRA rectangle copy (DA1 28)",               pending_deccra },
        { "DECRARA rectangle attributes (DA1 28)",        pending_decrara },
        { "DECSERA rectangle selective erase (DA1 28)",   pending_decsera },
        { "DECCOLM 132 columns (DA1 1)",                  pending_deccolm },
        { "DECIC/DECDC column insert, delete (DA1 21)",   pending_decic_decdc },
        { "SL/SR scroll left, right (DA1 21)",            pending_sl_sr },
        { "sixel graphics, cursor advance (DA1 4)",       pending_sixel },
    };
    size_t total = sizeof(cases) / sizeof(cases[0]), ready = 0;
    for (size_t i = 0; i < total; i++) {
        if (!cases[i].run()) continue;
        printf("  %s now passes: assert it and add its DA1 parameter\n", cases[i].name);
        ready++;
    }
    printf("unimplemented features: %zu pending, %zu ready to promote\n",
           total - ready, ready);
}

static void test_device_attributes(void)
{
    struct wsap_vtcore *t = tt_new_opts(80, 24, &reply_cb);

    write_seq(t, "\033[c");
    assert(!strcmp(response, "\033[?62;22c"));
    response_len = 0;
    write_seq(t, "\033[0c");
    assert(!strcmp(response, "\033[?62;22c"));

    write_seq(t, "\033[>c");
    assert(!strcmp(response, "\033[>0;10;0c"));

    response_len = 0;
    write_seq(t, "\033[=c");
    assert(response_len == 0);

    wsap_vtcore_free(t);
}

static void expect_flags(struct wsap_vtcore *t, const char *commands, const char *reply)
{
    write_seq(t, commands);
    write_seq(t, "\033[?u");
    assert(!strcmp(response, reply));
}
static void test_keyboard_negotiation(void)
{
    struct wsap_vtcore *t = tt_new_opts(80, 24, &reply_cb);
    assert(!wsap_vtcore_modes(t).app_cursor);
    write_seq(t, "\033[?1h");
    assert(wsap_vtcore_modes(t).app_cursor);
    write_seq(t, "\033[?1l");
    assert(!wsap_vtcore_modes(t).app_cursor);
    expect_flags(t, "", "\033[?0u");
    expect_flags(t, "\033[=1u", "\033[?1u");
    expect_flags(t, "\033[>3u\033[>31u", "\033[?31u");
    expect_flags(t, "\033[<u", "\033[?3u");
    expect_flags(t, "\033[=4;2u", "\033[?7u");
    expect_flags(t, "\033[=2;3u", "\033[?5u");
    expect_flags(t, "\033[=31;99u", "\033[?5u");
    expect_flags(t, "\033[<u", "\033[?1u");
    expect_flags(t, "\033[<999999999999999999999999u", "\033[?0u");
    expect_flags(t, "\033[>1:2u\033[=3;1;4u\033[1>31u", "\033[?0u");
    expect_flags(t, "\033[=255u", "\033[?31u");
    expect_flags(t, "\033[>u", "\033[?0u");
    expect_flags(t, "\033[<u", "\033[?31u");
    expect_flags(t, "\033[>1u\033[>2u\033[<2u", "\033[?31u");
    expect_flags(t, "\033[?1049h", "\033[?0u");
    expect_flags(t, "\033[>3u", "\033[?3u");
    expect_flags(t, "\033[?1049l", "\033[?31u");

    for (int i = 0; i < 1000; i++) write_seq(t, "\033[>1u");
    expect_flags(t, "\033[<u", "\033[?1u");
    expect_flags(t, "\033[<2147483647u", "\033[?0u");
    write_seq(t, "hello");
    assert(row_is(t, 0, "hello"));
    wsap_vtcore_free(t);
}

static void expect_selection(struct wsap_vtcore *t, int x1, int y1, int x2, int y2, const char *want)
{
    tt_select(t, x1, y1, x2, y2);
    char *text = wsap_vtcore_selection_text(t);
    if (!text || strcmp(text, want))
        fprintf(stderr, "selection: got '%s', want '%s'\n", text ? text : "NULL", want);
    assert(text && !strcmp(text, want));
    free(text);
}

static void test_wide_characters(void)
{
    struct wsap_vtcore *t = tt_new(8, 4);
    write_seq(t, "A😀界B");
    assert(tt_cursor_x(t) == 6);
    assert(tt_cell(t, 1, 0)->text == 0x1f600 && tt_cell(t, 3, 0)->text == 0x754c);
    expect_selection(t, 2, 0, 2, 0, "😀");
    write_seq(t, "\033[3Gx\033[4GX");
    assert(row_is(t, 0, "A xX B"));
    write_seq(t, "\033[2J\033[HA😀BC\033[2G\033[@");
    expect_selection(t, 0, 0, 7, 0, "A 😀BC");
    write_seq(t, "\033[3G\033[P");
    expect_selection(t, 0, 0, 7, 0, "A  BC");
    write_seq(t, "\033[2J\033[HA😀BC\033[3G\033[X");
    assert(row_is(t, 0, "A  BC"));

    write_seq(t, "\033[4;1H😀\r \033[4;1H😀\033[4;2H\033[38;2;1;2;3mX");
    assert(row_is(t, 3, " X"));
    wsap_vtcore_free(t);

    t = tt_new(5, 8);
    write_seq(t, "abcd😀Z");
    assert(tt_cursor_y(t) == 1 && tt_cursor_x(t) == 3);
    expect_selection(t, 0, 0, 4, 1, "abcd😀Z");
    tt_resize(t, 3, 8);
    assert(tt_cell(t, 1, 1)->text == 0x1f600 && tt_cell(t, 0, 2)->text == 'Z');
    assert(tt_cursor_y(t) == 2 && tt_cursor_x(t) == 1);
    expect_selection(t, 0, 0, 2, 2, "abcd😀Z");
    tt_resize(t, 12, 8);
    assert(tt_cursor_x(t) == 7 && tt_cursor_y(t) == 0);
    expect_selection(t, 0, 0, 11, 0, "abcd😀Z");
    tt_resize(t, 1, 8);
    tt_resize(t, 12, 8);
    expect_selection(t, 0, 0, 11, 0, "abcd😀Z");
    assert(tt_cursor_x(t) == 7);
    wsap_vtcore_free(t);

    t = tt_new(4, 3);
    write_seq(t, "abc©️)");
    assert(tt_cursor_y(t) == 1 && tt_cursor_x(t) == 3);
    expect_selection(t, 0, 0, 3, 1, "abc©️)");
    write_seq(t, "\033[2J\033[H😀︎x");
    assert(tt_cursor_x(t) == 2 && tt_cell(t, 1, 0)->text == 'x');
    expect_selection(t, 0, 0, 3, 0, "😀︎x");
    write_seq(t, "\033[2J\033[H\033[?7labc😀");
    assert(tt_cursor_y(t) == 0 && tt_cursor_x(t) == 3);
    wsap_vtcore_free(t);

    t = tt_new(4, 2);
    write_seq(t, "😀😀界界AB");
    wsap_vtcore_scroll(t, 1);
    expect_selection(t, 0, 0, 3, 1, "😀😀界界");
    wsap_vtcore_scroll_to(t, 0);
    tt_resize(t, 10, 2);
    expect_selection(t, 0, 0, 9, 0, "😀😀界界AB");
    wsap_vtcore_free(t);
}

static void test_wide_runs(void)
{
    static const char *const runs[] = {
        "日本語漢字仮名交じり文中文測試終端輸出性能比較日本語漢字仮名交じり文",
        "    日本語漢字仮名交じり文中文測試終端輸出性能比較",
        "┌─────────────────────┬─────────────────────┐",
        "日本語漢字仮名交じり文abc中文測試終端輸出性能比較",
        "日本語漢字仮名交じり文̈中文測試終端輸出性能比較",
        "한국어를테스트하는줄을적습니다한국어를테스트하는줄을적습니다",
        "！＂＃＄％＆＇（）＊＋，－．／０１２３４５６７８９",
        "日本語漢字仮名交じり文😀中文測試終端輸出性能比較",
    };
    for (size_t k = 0; k < sizeof(runs) / sizeof(*runs); k++) {
        char text[512];
        snprintf(text, sizeof(text), "\033[999;1H%s", runs[k]);
        struct wsap_vtcore *bulk = tt_new(120, 6);
        struct wsap_vtcore *split = tt_new(120, 6);
        tt_write(bulk, text, strlen(text));
        write_seq(split, text);
        assert_same_screen(bulk, split);

        expect_selection(bulk, 0, 5, 119, 5, runs[k]);
        wsap_vtcore_free(bulk);
        wsap_vtcore_free(split);
    }
}

static void test_invalid_utf8_runs(void)
{
    static const unsigned char invalid[][3] = {
        {0xe0, 0x82, 0xa1}, {0xed, 0xa0, 0x80}, {0xe0, 0x9f, 0xbf},
    };
    for (size_t k = 0; k < sizeof(invalid) / sizeof(*invalid); k++) {
        char input[128];
        size_t n = 0;
        for (int i = 0; i < 32; i++) {
            memcpy(input + n, invalid[k], 3);
            n += 3;
        }
        memcpy(input + n, "END", 3);
        n += 3;
        struct wsap_vtcore *bulk = tt_new(120, 4);
        struct wsap_vtcore *split = tt_new(120, 4);
        write_seq(bulk, "\033[4;1H\n");
        write_seq(split, "\033[4;1H\n");
        tt_write(bulk, input, n);
        for (size_t i = 0; i < n; i++) tt_write(split, input + i, 1);
        for (int c = 0; c < 96; c++)
            assert(tt_cell_cp(bulk, tt_cell(bulk, c, 3)) == 0xFFFD);
        for (int c = 0; c < 3; c++)
            assert(tt_cell_cp(bulk, tt_cell(bulk, 96 + c, 3)) == (uint32_t)"END"[c]);
        assert_same_screen(bulk, split);
        wsap_vtcore_free(bulk);
        wsap_vtcore_free(split);
    }
}

static void test_recycled_scrollback_blanks(void)
{
    for (int mode = 0; mode < 3; mode++) {
        struct wsap_vtcore *t = tt_new(32, 3);
        write_seq(t, "\033[3;1H");
        const char *line = mode == 2 ? "abcdefghijklmnopqrstuvwxyz\r\033[S" :
                                       "abcdefghijklmnopqrstuvwxyz\r\n";
        for (int i = 0; i < TEST_SCROLLBACK + 10; i++) {
            if (mode == 1) write_seq(t, line);
            else tt_write(t, line, strlen(line));
        }
        write_seq(t, "\tfile/");
        assert(row_is(t, 2, "        file/"));
        wsap_vtcore_free(t);
    }
}

static void test_scroll_view_clamps_extreme_deltas(void)
{
    struct wsap_vtcore *t = tt_new(20, 3);
    for (int i = 0; i < 10; i++)
        write_seq(t, "line\r\n");
    int max = tt_history(t);
    assert(max > 1);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    wsap_vtcore_scroll(t, 1);
    wsap_vtcore_scroll(t, INT_MAX);
    assert(tt_view_offset(t) == max);
    uint64_t version = view_version(snap, t);
    wsap_vtcore_scroll(t, 1);
    assert(view_version(snap, t) == version);
    wsap_vtcore_scroll(t, -1);
    wsap_vtcore_scroll(t, INT_MIN);
    assert(tt_view_offset(t) == 0);
    version = view_version(snap, t);
    wsap_vtcore_scroll(t, -1);
    assert(view_version(snap, t) == version);
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
}

static void test_batched_recycled_lines(void)
{
    static const char *const endings[] = {
        "short\r\nx\r\nyz\r\n",
        "short\r\nx\r\nyz\r\n\tfile/",
        "abcdefghijklmnopqrstuvwxyz012345more\r\nx\r\nyz\r\n\033[2CZ",
        "short\r\nx\r\nyz\r\n界\xcc\x81",
        "short\r\nx\r\nyz\r\npartial\r",
    };
    for (size_t k = 0; k < sizeof(endings) / sizeof(endings[0]); k++) {
        struct wsap_vtcore *bulk = tt_new(32, 3);
        struct wsap_vtcore *bytes = tt_new(32, 3);
        const char *old = "\033[31;44mabcdefghijklmnopqrst界界界\r\n";
        for (int i = 0; i < TEST_SCROLLBACK + 10; i++) {
            tt_write(bulk, old, strlen(old));
            write_seq(bytes, old);
        }
        write_seq(bulk, "\033[0;1;3m");
        write_seq(bytes, "\033[0;1;3m");
        tt_write(bulk, endings[k], strlen(endings[k]));
        write_seq(bytes, endings[k]);

        assert_same_screen(bulk, bytes);
        for (int i = 0; i < tt_history(bulk); i += 3) {
            wsap_vtcore_scroll(bulk, 3);
            wsap_vtcore_scroll(bytes, 3);
            assert_same_screen(bulk, bytes);
        }
        wsap_vtcore_free(bulk);
        wsap_vtcore_free(bytes);
    }
}

static void test_full_width_line_endings(void)
{
    static const char *const feeds[] = {
        "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff",
        "aaaa\r\nbbbb\r\ncccc\r\ndddd\neeee",
        "aaaa\r\nbbbb\r\ncccc\r\ndddd\re",
        "aaaa\r\nbbbb\r\ncccc\r\nddddeeeeff",
        "aa\r\nbb\r\ncc\r\nd中d\r\nef",
    };
    for (size_t k = 0; k < sizeof(feeds) / sizeof(feeds[0]); k++) {
        struct wsap_vtcore *bulk = tt_new(4, 3);
        struct wsap_vtcore *bytes = tt_new(4, 3);
        tt_write(bulk, feeds[k], strlen(feeds[k]));
        write_seq(bytes, feeds[k]);
        assert_same_screen(bulk, bytes);
        wsap_vtcore_free(bulk);
        wsap_vtcore_free(bytes);
    }

    struct wsap_vtcore *t = tt_new(4, 3);
    tt_write(t, feeds[0], strlen(feeds[0]));
    assert(tt_history(t) == 3);
    assert(row_is(t, 0, "dddd") && row_is(t, 1, "eeee") && row_is(t, 2, "ffff"));
    wsap_vtcore_free(t);
}

static void test_batched_line_masks(void)
{
    static const char *const tails[] = {
        "\r\nx\r\ny\r\n", "\r\nx\ny\r\n", "\r\nx\r\ny\t",
        "\r\nx\r\ny\rQ", "\r\nx\033[32my\r\n",
    };
    for (int offset = 0; offset <= 65; offset++) {
        for (size_t k = 0; k < sizeof(tails) / sizeof(tails[0]); k++) {
            char input[256];
            memset(input, 'a', (size_t)offset);
            size_t n = (size_t)offset + strlen(tails[k]);
            memcpy(input + offset, tails[k], strlen(tails[k]));
            memset(input + n, 'z', 128);
            n += 128;
            struct wsap_vtcore *bulk = tt_new(80, 3), *bytes = tt_new(80, 3);
            write_both(bulk, bytes, "\033[3;1H\033[31m");
            tt_write(bulk, input, n);
            for (size_t i = 0; i < n; i++) tt_write(bytes, input + i, 1);
            write_both(bulk, bytes, "\033[b");
            assert_same_screen(bulk, bytes);
            wsap_vtcore_scroll_to(bulk, INT_MAX);
            wsap_vtcore_scroll_to(bytes, INT_MAX);
            assert_same_screen(bulk, bytes);
            wsap_vtcore_free(bulk);
            wsap_vtcore_free(bytes);
        }
    }
}

static void test_bulk_line_flood(void)
{
    static const struct wsap_vtcore_vector2i geoms[] = {
        {.x = 80, .y = 24}, {.x = 27, .y = 8}, {.x = 10, .y = 5}, {.x = 120, .y = 80},
    };
    for (size_t g = 0; g < 2 * sizeof(geoms) / sizeof(geoms[0]); g++) {
        int cols = geoms[g / 2].x, rows = geoms[g / 2].y;
        size_t cap = 1u << 19;
        char *buf = malloc(cap);
        assert(buf);
        size_t n = 0;
        unsigned state = 12345;
        while (n < cap / 2) {
            state = state * 1103515245u + 12345u;
            unsigned pick = (state >> 16) % 27;
            buf[n++] = pick == 26 ? '\n' : (char)('a' + pick);
        }
        for (int k = -1; k <= 1; k++) {
            memset(buf + n, 'x', (size_t)(cols + k));
            n += (size_t)(cols + k);
            buf[n++] = '\n';
            memset(buf + n, 'y', (size_t)(2 * cols + k));
            n += (size_t)(2 * cols + k);
            buf[n++] = '\n';
        }
        while (n < cap) {
            state = state * 1103515245u + 12345u;
            unsigned pick = (state >> 16) % 27;
            buf[n++] = pick == 26 ? '\n' : (char)('a' + pick);
        }

        struct wsap_vtcore *bulk = tt_new(cols, rows);
        struct wsap_vtcore *stream = tt_new(cols, rows);
        struct wsap_vtcore *bytes = tt_new(cols, rows);
        unsigned flags = g % 2 != 0 ? WSAP_VTCORE_FEED_ONLCR : 0;
        for (size_t i = 0; i < n; i += 65536)
            wsap_vtcore_feed(bulk, buf + i, n - i < 65536 ? n - i : 65536, flags);
        wsap_vtcore_feed(stream, buf, n, flags);
        for (size_t i = 0; i < n; i++) wsap_vtcore_feed(bytes, buf + i, 1, flags);

        assert_same_screen(bulk, bytes);
        assert_same_screen(stream, bytes);
        for (int i = 0; i < tt_history(bulk); i += 7) {
            wsap_vtcore_scroll(bulk, 7);
            wsap_vtcore_scroll(stream, 7);
            wsap_vtcore_scroll(bytes, 7);
            assert_same_screen(bulk, bytes);
            assert_same_screen(stream, bytes);
        }
        wsap_vtcore_free(bulk);
        wsap_vtcore_free(stream);
        wsap_vtcore_free(bytes);
        free(buf);
    }
}

static void test_ascii_flood_edits(void)
{
    static const int widths[] = {1, 2, 7, 120};
    char input[65536];
    unsigned state = 43;
    for (size_t i = 0; i < sizeof(input); i++) {
        state = state * 1103515245u + 12345u;
        unsigned pick = (state >> 16) % 5;
        input[i] = pick == 4 ? '\n' : (char)('a' + pick);
    }
    memset(input + 1024, '\n', 256);
    static const char edit[] =
        "\033[1;1Hé漢x̂\033[T\033[2;1HEND"
        "\033[?1049hALT\033[?1049l\033[6;1Htail\n";
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (size_t chunk = 4096; chunk <= sizeof(input); chunk *= 16) {
            struct wsap_vtcore *bulk = tt_new(widths[w], 6);
            struct wsap_vtcore *bytes = tt_new(widths[w], 6);
            for (size_t i = 0; i < sizeof(input); i += chunk)
                wsap_vtcore_feed(bulk, input + i, chunk, WSAP_VTCORE_FEED_ONLCR);
            for (size_t i = 0; i < sizeof(input); i++)
                wsap_vtcore_feed(bytes, input + i, 1, WSAP_VTCORE_FEED_ONLCR);
            assert_same_screen(bulk, bytes);
            wsap_vtcore_feed(bulk, edit, sizeof(edit) - 1, WSAP_VTCORE_FEED_ONLCR);
            for (size_t i = 0; i < sizeof(edit) - 1; i++)
                wsap_vtcore_feed(bytes, edit + i, 1, WSAP_VTCORE_FEED_ONLCR);
            assert_same_screen(bulk, bytes);
            for (int i = 0; i < tt_history(bulk); i += 6) {
                wsap_vtcore_scroll(bulk, 6);
                wsap_vtcore_scroll(bytes, 6);
                assert_same_screen(bulk, bytes);
            }
            wsap_vtcore_scroll_to(bulk, 0);
            wsap_vtcore_scroll_to(bytes, 0);
            assert(tt_resize(bulk, widths[w] + 3, 7));
            assert(tt_resize(bytes, widths[w] + 3, 7));
            assert_same_screen(bulk, bytes);
            wsap_vtcore_free(bulk);
            wsap_vtcore_free(bytes);
        }
    }
}

static void test_margin_linefeed_bursts(void)
{
    static const char *bursts[] = {
        "\n\n", "\n\n\n\n\n", "\n\n\n\n\n\n\n\n",
        "\r\n\r\n", "\r\n\r\n\r\n\r\n\r\n\r\n",
        "\n\n\r\n\r\n\n\n",
    };
    for (size_t k = 0; k < sizeof(bursts) / sizeof(bursts[0]); k++) {
        for (int mode = 0; mode < 3; mode++) {
            struct wsap_vtcore *bulk = tt_new(8, 8);
            struct wsap_vtcore *bytes = tt_new(8, 8);
            const char *setup = "\033[1;1Htop\033[2;1Ha\033[3;1Hb\033[4;1Hc"
                                "\033[5;1Hd\033[6;1He\033[7;1Hf\033[8;1Hfoot"
                                "\033[2;7r\033[7;3H";
            tt_write(bulk, setup, strlen(setup));
            tt_write(bytes, setup, strlen(setup));
            if (mode == 1) {
                write_seq(bulk, "\033[20h");
                write_seq(bytes, "\033[20h");
            }
            unsigned flags = mode == 2 ? WSAP_VTCORE_FEED_ONLCR : 0;
            wsap_vtcore_feed(bulk, bursts[k], strlen(bursts[k]), flags);
            feed_seq(bytes, bursts[k], flags);
            assert_same_screen(bulk, bytes);
            assert(row_is(bulk, 0, "top"));
            assert(row_is(bulk, 7, "foot"));
            wsap_vtcore_free(bulk);
            wsap_vtcore_free(bytes);
        }
    }
}

static void test_alt_linefeed_burst_after_resize(void)
{
    struct wsap_vtcore *bulk = tt_new(8, 5);
    struct wsap_vtcore *bytes = tt_new(8, 5);
    const char *setup = "\033[?1049h\033[5;1H\n\n\n\n";
    tt_write(bulk, setup, strlen(setup));
    write_seq(bytes, setup);
    tt_resize(bulk, 8, 3);
    tt_resize(bytes, 8, 3);
    const char *burst = "\n\n\n\n\r\n\r\n";
    tt_write(bulk, burst, strlen(burst));
    write_seq(bytes, burst);
    assert_same_screen(bulk, bytes);
    tt_resize(bulk, 12, 4);
    tt_resize(bytes, 12, 4);
    assert_same_screen(bulk, bytes);
    wsap_vtcore_free(bulk);
    wsap_vtcore_free(bytes);
}

static void test_alt_resize_keeps_row_order(void)
{
    const char *setup = "\033[?1049h\033[5;1H\n\n\n"
                        "\033[1;1HA\033[2;1HB\033[3;1HC\033[4;1HD\033[5;1HE";

    struct wsap_vtcore *grow = tt_new(20, 5);
    write_seq(grow, setup);
    tt_resize(grow, 20, 8);
    assert(row_is(grow, 0, "A") && row_is(grow, 4, "E"));
    assert(row_is(grow, 5, "") && row_is(grow, 7, ""));
    wsap_vtcore_free(grow);

    struct wsap_vtcore *shrink = tt_new(20, 5);
    write_seq(shrink, setup);
    tt_resize(shrink, 20, 3);
    assert(row_is(shrink, 0, "C") && row_is(shrink, 2, "E"));
    wsap_vtcore_free(shrink);
}

static void test_alt_shrink_then_flood(void)
{
    struct wsap_vtcore *t = tt_new(80, 40);
    tt_write(t, "\033[?1049h\033[40;1H", 16);
    for (int i = 0; i < 35; i++)
        tt_write(t, "x\n", 2);
    tt_resize(t, 80, 10);
    char line[32];
    for (int i = 0; i < 6000; i++) {
        int len = snprintf(line, sizeof(line), "\rline %04d\n", i);
        tt_write(t, line, (size_t)len);
    }
    assert(row_is(t, 8, "line 5999") && row_is(t, 0, "line 5991"));
    wsap_vtcore_free(t);
}

static void test_alt_erase_scrollback(void)
{
    struct wsap_vtcore *t = tt_new(20, 5);
    for (int i = 0; i < 30; i++)
        tt_write(t, "line\r\n", 6);
    assert(tt_history(t) > 0);
    write_seq(t, "\033[?1049h\033[3J\033[?1049l");
    assert(tt_history(t) == 0);
    wsap_vtcore_free(t);
}

static void test_snapshot_follows_new_terminal(void)
{
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    for (int i = 0; i < 8; i++) {
        struct wsap_vtcore *t = tt_new(10, 2);
        char text[2] = { (char)('a' + i), 0 };
        tt_puts(t, text);
        assert(wsap_vtcore_snapshot_update(s, t));
        assert(wsap_vtcore_snapshot_row(s, 0)[0].text == (uint32_t)text[0]);
        wsap_vtcore_free(t);
    }
    wsap_vtcore_snapshot_free(s);
}

static void test_snapshot_null_and_range(void)
{
    uint32_t cps[4] = { 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA };
    assert(wsap_vtcore_snapshot_view(NULL) == NULL);
    assert(wsap_vtcore_snapshot_row(NULL, 0) == NULL);
    assert(wsap_vtcore_snapshot_row_key(NULL, 0) == 0);
    struct wsap_vtcore_span none = wsap_vtcore_snapshot_row_selection(NULL, 0);
    assert(none.begin == 0 && none.end == 0);
    assert(wsap_vtcore_snapshot_cluster(NULL, 'a', cps, 4) == 0);
    assert(wsap_vtcore_snapshot_cluster(NULL, WSAP_VTCORE_TEXT_CLUSTER, cps, 4) == 0);
    assert(cps[0] == 0xAAAA);

    struct wsap_vtcore *t = tt_new(4, 2);
    tt_write(t, "e\xcc\x81", 3);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(wsap_vtcore_snapshot_update(s, t));

    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(s);
    const uint32_t *palette = view->palette;
    assert(wsap_vtcore_snapshot_update(s, t));
    assert(wsap_vtcore_snapshot_view(s) == view && view->palette == palette);

    assert(wsap_vtcore_snapshot_row(s, -1) == NULL);
    assert(wsap_vtcore_snapshot_row(s, 2) == NULL);
    assert(wsap_vtcore_snapshot_row_key(s, -1) == 0);
    assert(wsap_vtcore_snapshot_row_key(s, 2) == 0);
    struct wsap_vtcore_span off = wsap_vtcore_snapshot_row_selection(s, 2);
    assert(off.begin == 0 && off.end == 0);

    uint32_t text = wsap_vtcore_snapshot_row(s, 0)[0].text;
    assert(text & WSAP_VTCORE_TEXT_CLUSTER);
    assert(wsap_vtcore_snapshot_cluster(s, text, NULL, 0) == 2);
    assert(wsap_vtcore_snapshot_cluster(s, text, cps, 0) == 2);
    assert(cps[0] == 0xAAAA);
    assert(wsap_vtcore_snapshot_cluster(s, text, NULL, 4) == 2);
    assert(wsap_vtcore_snapshot_cluster(s, 'a', NULL, 0) == 1);
    assert(wsap_vtcore_snapshot_cluster(s, 'a', cps, 0) == 1);
    assert(cps[0] == 0xAAAA);
    assert(wsap_vtcore_snapshot_cluster(s, 'a', cps, 4) == 1 && cps[0] == 'a');

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
}

static void test_string_payloads(void)
{
    static const char *prefixes[] = {"\033]0;", "\033]7;file://host/", "\033P"};
    static const char *ends[] = {"\007", "\033\\"};
    static const size_t chunks[] = {1, 7, 4096};
    for (size_t p = 0; p < 3; p++) {
        for (size_t e = 0; e < 2; e++) {
            for (int large = 0; large < 2; large++) {
                char input[1024];
                size_t n = (size_t)snprintf(input, sizeof(input), "%s⠋ çalışma/clernen_pi", prefixes[p]);
                if (large) {
                    memset(input + n, 'a', 600);
                    n += 600;
                }
                n += (size_t)snprintf(input + n, sizeof(input) - n, "%sOK", ends[e]);
                for (size_t c = 0; c < 3; c++) {
                    title[0] = 0;
                    response_len = 0;
                    struct wsap_vtcore *t = tt_new_opts(80, 4, &title_cb);
                    for (size_t i = 0; i < n;) {
                        size_t take = n - i < chunks[c] ? n - i : chunks[c];
                        tt_write(t, input + i, take);
                        i += take;
                    }
                    assert(row_is(t, 0, "OK"));
                    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
                    assert(response_len == 0);
                    if (p == 0 && !large)
                        assert(!strcmp(title, "⠋ çalışma/clernen_pi"));
                    if (large) {
                        if (p == 0) {
                            assert(!strncmp(title, "⠋ çalışma/clernen_pi",
                                            strlen("⠋ çalışma/clernen_pi")));
                            assert(title[sizeof(title) - 1] == 0);
                        } else {
                            assert(title[0] == 0);
                        }
                    }
                    wsap_vtcore_free(t);
                }
            }
        }
    }
}

static void test_host_name_rejected(void)
{
    static const char *const bad[] = {
        "\033x", "a\tb", "\177", "\300", "a\nb",
    };
    char longest[65];
    memset(longest, 'a', 64);
    longest[64] = 0;
    char fits[64];
    memset(fits, 'b', 63);
    fits[63] = 0;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct wsap_vtcore_options opt = { .size = { .x = 10, .y = 3 }, .name = bad[i] };
        assert(!wsap_vtcore_new(&opt));
    }
    struct wsap_vtcore_options opt = { .size = { .x = 10, .y = 3 }, .name = longest };
    assert(!wsap_vtcore_new(&opt));

    opt.name = fits;
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    wsap_vtcore_free(t);
}

static void test_newline_policy_survives_reset(void)
{
    static const char *const resets[] = { "\033c", "\033[!p", "\033[20l" };
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
        struct wsap_vtcore *t = tt_new(20, 6);
        wsap_vtcore_feed(t, "a\n", 2, WSAP_VTCORE_FEED_ONLCR);
        assert(tt_cursor_x(t) == 0);
        write_seq(t, resets[i]);
        wsap_vtcore_feed(t, "b\nc", 3, WSAP_VTCORE_FEED_ONLCR);
        assert(tt_cursor_x(t) == 1);
        assert(row_is(t, tt_cursor_y(t), "c"));
        wsap_vtcore_free(t);
    }
}

static void test_lnm_mode_alone_is_not_sticky(void)
{
    struct wsap_vtcore *t = tt_new(20, 6);
    write_seq(t, "\033[20h\033c");
    tt_write(t, "b\nc", 3);
    assert(tt_cursor_x(t) == 2);
    wsap_vtcore_free(t);
}

static void test_c0_ends_utf8_sequence(void)
{
    static const char input[] = "\xc3\n\xa9z";
    for (size_t split = 0; split < sizeof(input) - 1; split++) {
        struct wsap_vtcore *t = tt_new(20, 4);
        tt_write(t, input, split);
        tt_write(t, input + split, sizeof(input) - 1 - split);
        assert(tt_cell_cp(t, tt_cell(t, 0, 0)) == 0xFFFD);
        assert(tt_cell_cp(t, tt_cell(t, 1, 1)) == 0xFFFD);
        assert(tt_cell_cp(t, tt_cell(t, 2, 1)) == 'z');
        wsap_vtcore_free(t);
    }
}

static void test_bulk_rows_survive_edits(void)
{
    enum { COLS = 40, ROWS = 6 };
    static char flood[1 << 16];
    for (size_t i = 0; i < sizeof(flood); i++)
        flood[i] = (char)('a' + (i * 7 + i / COLS) % 26);

    struct wsap_vtcore *bulk = tt_new(COLS, ROWS);
    struct wsap_vtcore *bytes = tt_new(COLS, ROWS);
    write_seq(bulk, "\033[1;4;38;2;9;8;7;48;2;1;2;3m");
    write_seq(bytes, "\033[1;4;38;2;9;8;7;48;2;1;2;3m");
    tt_write(bulk, flood, sizeof(flood));
    for (size_t i = 0; i < sizeof(flood); i++) tt_write(bytes, flood + i, 1);
    assert_same_screen(bulk, bytes);

    const char *edit = "\033[m\033[2;5HXYZ\033[3;1H\033[K\033[4;10H\033[1K";
    write_seq(bulk, edit);
    write_seq(bytes, edit);
    assert_same_screen(bulk, bytes);
    assert(tt_view_cell(bulk, 4, 1)->text == 'X');

    const char *region = "\033[2;5r\033[5;1H\n\n\033[T\033[r";
    write_seq(bulk, region);
    write_seq(bytes, region);
    assert_same_screen(bulk, bytes);

    write_seq(bulk, "\033[?1049hALT\033[?1049l");
    write_seq(bytes, "\033[?1049hALT\033[?1049l");
    assert_same_screen(bulk, bytes);
    for (int i = 0; i < 3; i++) {
        wsap_vtcore_scroll(bulk, 100);
        wsap_vtcore_scroll(bytes, 100);
        assert_same_screen(bulk, bytes);
    }
    wsap_vtcore_scroll_to(bulk, 0);
    wsap_vtcore_scroll_to(bytes, 0);

    wsap_vtcore_scroll(bulk, 50);
    wsap_vtcore_scroll(bytes, 50);
    tt_select(bulk, 0, 0, COLS - 1, 2);
    tt_select(bytes, 0, 0, COLS - 1, 2);
    char *a = wsap_vtcore_selection_text(bulk), *b = wsap_vtcore_selection_text(bytes);
    assert(a && b && !strcmp(a, b) && strlen(a) >= COLS);
    free(a);
    free(b);
    wsap_vtcore_scroll_to(bulk, 0);
    wsap_vtcore_scroll_to(bytes, 0);

    tt_resize(bulk, 29, ROWS);
    tt_resize(bytes, 29, ROWS);
    assert_same_screen(bulk, bytes);
    tt_resize(bulk, 61, ROWS + 2);
    tt_resize(bytes, 61, ROWS + 2);
    assert_same_screen(bulk, bytes);
    wsap_vtcore_scroll(bulk, 20);
    wsap_vtcore_scroll(bytes, 20);
    assert_same_screen(bulk, bytes);

    wsap_vtcore_free(bulk);
    wsap_vtcore_free(bytes);
}

static void test_erase_keeps_used_rows(void)
{
    for (int row = 0; row < 40; row++) {
        struct wsap_vtcore *t = tt_new(20, 40);
        char seq[64];
        int len = snprintf(seq, sizeof(seq), "\033[%d;1Hsaved\033[2J", row + 1);
        tt_write(t, seq, (size_t)len);
        assert(tt_history(t) == row + 1);
        for (int y = 0; y < 40; y++)
            assert(row_eq(t, y, ""));
        wsap_vtcore_scroll(t, 1);
        assert(row_eq(t, 0, "saved"));
        wsap_vtcore_free(t);
    }
}

static void test_blank_rows(void)
{
    struct wsap_vtcore *t = tt_new(10, 4);
    write_seq(t, "one\r\ntwo\r\nthree");
    write_seq(t, "\033[2J");
    assert(tt_history(t) == 3);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 10; x++) {
            const struct wsap_vtcore_cell *c = tt_view_cell(t, x, y);
            assert(c->text == ' ' && c->fg == WSAP_VTCORE_COLOR_FG &&
                   c->bg == WSAP_VTCORE_COLOR_BG);
            assert(tt_cell(t, x, y)->text == ' ');
        }
    wsap_vtcore_scroll(t, 3);
    assert(row_is(t, 0, "one") && row_is(t, 1, "two") && row_is(t, 2, "three"));
    wsap_vtcore_scroll_to(t, 0);

    write_seq(t, "\033[4;1Hlast\r\n\r\n\r\n");
    for (int x = 0; x < 10; x++)
        assert(tt_view_cell(t, x, 3)->text == ' ');
    write_seq(t, "\033[4;6Hhi");
    assert(row_is(t, 3, "     hi"));
    wsap_vtcore_free(t);

    t = tt_new(10, 6);
    write_seq(t, "top\033[6;1H     ");
    assert(tt_history(t) == 0);
    write_seq(t, "\033[2J");
    assert(tt_history(t) == 1);
    wsap_vtcore_scroll(t, 1);
    assert(row_is(t, 0, "top"));
    wsap_vtcore_scroll_to(t, 0);

    tt_resize(t, 20, 6);
    for (int x = 0; x < 20; x++)
        assert(tt_cell(t, x, 5)->text == ' ' &&
               tt_cell(t, x, 5)->bg == WSAP_VTCORE_COLOR_BG);
    wsap_vtcore_free(t);

    t = tt_new(10, 4);
    write_seq(t, "\033[2J\033[41m\033[2K");
    assert(tt_cell(t, 0, 0)->bg != WSAP_VTCORE_COLOR_BG);
    assert(tt_cell(t, 9, 0)->bg == tt_cell(t, 0, 0)->bg);
    wsap_vtcore_free(t);
}

static void test_csi_text_chunks(void)
{
    static const char *cases[] = {
        "\033[2;3Hhello\033[2DXY\033[K\033[1A\033[3C!\r\n",
        "\033[H日本語\033[2GXY\033[7mabc\033[m!\033[2Knext",
        "\033[Hscreen\033[2J\033[Hnext\033[2K\r\n",
        "\033[H012345678901234567890123\033[2DXY\033[J!",
        "\033[?7l\033[H012345678901234567890123\033[1CXY",
        "\033[41m\033[2J\033[Htext\033[0Jtail\033[1Jend",
        "\033[2;3Htext\033[3Jnext\033[0K\033[1K\033[2K!",
        "\033[?6h\033[2;3Htext\033[Jnext",
        "\033[16;1Ha\033[17;1Hb\033[32;1Hc\033[40;1Hd\033[2J",
        "\033[40;1H \033[17;1Ha\033[2J\033[Hnext",
        "\033[?1049h\033[40;1H\n\033[17;1Htext\033[2J",
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const char *seq = cases[c];
        size_t len = strlen(seq);
        struct wsap_vtcore *expected = tt_new(20, 40);
        write_seq(expected, seq);
        for (size_t split = 0; split <= len; split++) {
            struct wsap_vtcore *actual = tt_new(20, 40);
            tt_write(actual, seq, split);
            tt_write(actual, seq + split, len - split);
            assert(tt_cursor_x(actual) == tt_cursor_x(expected));
            assert(tt_cursor_y(actual) == tt_cursor_y(expected));
            assert(tt_history(actual) == tt_history(expected));
            int history = tt_history(expected);
            wsap_vtcore_scroll(actual, history);
            wsap_vtcore_scroll(expected, history);
            for (int page = 0; page <= history; page++) {
                assert_same_rows(actual, expected, 40);
                wsap_vtcore_scroll(actual, -1);
                wsap_vtcore_scroll(expected, -1);
            }
            wsap_vtcore_free(actual);
        }
        wsap_vtcore_free(expected);
    }
}

static void test_grow_past_reservation(void)
{
    struct wsap_vtcore *t = tt_new(40, 10);
    for (int i = 0; i < 30; i++) {
        char line[32];
        int len = snprintf(line, sizeof(line), "hist %02d\r\n", i);
        tt_write(t, line, (size_t)len);
    }
    int history = tt_history(t);
    assert(history > 0);

    tt_resize(t, 40, 700);
    assert(tt_rows(t) == 700);
    tt_write(t, "\033[700;1Hbottom", 14);
    assert(row_is(t, 699, "bottom"));

    tt_resize(t, 3000, 700);
    assert(tt_cols(t) == 3000);
    tt_write(t, "\033[698;2995Hedge!", 16);
    assert(tt_cell(t, 2994, 697)->text == 'e' && tt_cell(t, 2998, 697)->text == '!');

    tt_write(t, "\033[700;1H", 8);
    for (int i = 0; i < 1000; i++) {
        char line[32];
        int len = snprintf(line, sizeof(line), "\r\nfill %04d", i);
        tt_write(t, line, (size_t)len);
    }
    int history_before = tt_history(t);
    assert(history_before > 0);

    tt_write(t, "\033[?1049h", 8);
    tt_write(t, "alt", 3);
    tt_resize(t, 3000, 2500);
    assert(tt_rows(t) == 2500);
    assert(row_is(t, 0, "alt"));
    tt_write(t, "\033[?1049l", 8);

    bool found = false;
    wsap_vtcore_scroll(t, tt_history(t));
    for (int y = 0; y < tt_rows(t) && !found; y++)
        found = row_eq(t, y, "hist 00");
    assert(found);
    wsap_vtcore_scroll(t, -tt_history(t));
    bool last = false;
    for (int y = 0; y < tt_rows(t) && !last; y++)
        last = row_eq(t, y, "fill 0999");
    assert(last);
    wsap_vtcore_free(t);
}

struct concurrent_worker_args {
    struct wsap_vtcore *t;
    _Atomic bool done;
};

static const char concurrent_stream[] =
    "The quick brown fox jumps over the lazy dog.\r\n"
    "\033[31mRed text\033[0m and \033[32mgreen text\033[0m.\r\n"
    "Some unicode: 漢字, ひらがな, and emojis: 😀🚀🎉\r\n"
    "\033[2J\033[HCleared and reset\r\n"
    "Line 1\r\nLine 2\r\nLine 3\r\nLine 4\r\nLine 5\r\n"
    "\033]7;file:///tmp/dir\007\033]22;pointer\007"
    "\033]8;;https://example.com\007link\033]8;;\007\r\n";

static void *concurrent_feed_worker(void *arg)
{
    struct concurrent_worker_args *w = arg;
    size_t len = strlen(concurrent_stream);

    for (int i = 0; i < 2000; i++) {
        wsap_vtcore_feed(w->t, concurrent_stream, len, 0);
    }
    atomic_store(&w->done, true);
    return NULL;
}

static void test_concurrent_feed_and_snapshot(void)
{
    struct wsap_vtcore_options opt = {
        .size = { .x = 80, .y = 24 },
        .scrollback = 1000,
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t != NULL);

    struct concurrent_worker_args w = { .t = t, .done = false };
    pthread_t th;
    int err = pthread_create(&th, NULL, concurrent_feed_worker, &w);
    assert(err == 0);

    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap != NULL);

    while (!atomic_load(&w.done)) {
        bool ok = wsap_vtcore_snapshot_update(snap, t);
        assert(ok);
        struct wsap_vtcore_vector2i sz = wsap_vtcore_size(t);
        assert(sz.x > 0 && sz.y > 0);
        wsap_vtcore_scroll(t, 2);
        wsap_vtcore_scroll_to(t, 0);
        struct wsap_vtcore_modes m = wsap_vtcore_modes(t);
        (void)m;
        struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t);
        (void)c;

        wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 0 }, false, WSAP_VTCORE_SELECT_WORD);
        wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 40, .y = 10 }, false);
        (void)wsap_vtcore_has_selection(t);
        wsap_vtcore_text_free(wsap_vtcore_selection_text(t));
        wsap_vtcore_text_free(wsap_vtcore_link_at(t, (struct wsap_vtcore_vector2i){ .x = 1, .y = 1 }));
        wsap_vtcore_text_free(wsap_vtcore_cwd(t));
        wsap_vtcore_text_free(wsap_vtcore_pointer_shape(t));
        (void)wsap_vtcore_history_rows(t);
        wsap_vtcore_text_free(wsap_vtcore_history_text(t, 5));
        struct wsap_vtcore_scrollback sb = wsap_vtcore_scrollback(t);
        assert(sb.offset >= 0 && sb.total >= 0);
    }

    pthread_join(th, NULL);
    bool ok = wsap_vtcore_snapshot_update(snap, t);
    assert(ok);

    struct wsap_vtcore *ref = wsap_vtcore_new(&opt);
    assert(ref != NULL);
    for (int i = 0; i < 2000; i++)
        tt_write(ref, concurrent_stream, strlen(concurrent_stream));
    assert(tt_cols(t) == tt_cols(ref) && tt_rows(t) == tt_rows(ref));
    assert(tt_history(t) == tt_history(ref));
    assert(tt_cursor_x(t) == tt_cursor_x(ref) && tt_cursor_y(t) == tt_cursor_y(ref));
    char *got = malloc((size_t)tt_cols(t) + 1);
    char *want = malloc((size_t)tt_cols(ref) + 1);
    assert(got && want);
    for (int y = 0; y < tt_rows(t); y++) {
        row_text(t, y, got);
        row_text(ref, y, want);
        assert(!strcmp(got, want));
    }
    free(got);
    free(want);
    char *text = wsap_vtcore_history_text(t, 1000);
    char *text_ref = wsap_vtcore_history_text(ref, 1000);
    assert(text && text_ref && !strcmp(text, text_ref));
    wsap_vtcore_text_free(text);
    wsap_vtcore_text_free(text_ref);

    wsap_vtcore_free(ref);
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
}

static void test_reserved_options_must_be_zero(void)
{
    struct wsap_vtcore_options opt = { .size = { .x = 10, .y = 2 } };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t != NULL);
    wsap_vtcore_free(t);

    size_t words = sizeof(opt.reserved) / sizeof(opt.reserved[0]);
    for (size_t i = 0; i < words; i++) {
        opt.reserved[i] = 1;
        assert(wsap_vtcore_new(&opt) == NULL);
        opt.reserved[i] = 0;
    }
    opt.reserved[words - 1] = 0x80000000u;
    assert(wsap_vtcore_new(&opt) == NULL);
    opt.reserved[words - 1] = 0;
    t = wsap_vtcore_new(&opt);
    assert(t != NULL);
    wsap_vtcore_free(t);
}

struct lock_probe {
    struct wsap_vtcore *t;
    _Atomic bool entered;
};

static void *lock_probe_worker(void *arg)
{
    struct lock_probe *p = arg;
    wsap_vtcore_lock(p->t);
    atomic_store(&p->entered, true);
    wsap_vtcore_unlock(p->t);
    return NULL;
}

static void test_lock_reentrant_across_threads(void)
{
    struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 80, .y = 24 },
    });
    assert(t != NULL);

    wsap_vtcore_lock(t);
    wsap_vtcore_lock(t);
    assert(tt_cols(t) == 80);
    wsap_vtcore_unlock(t);

    struct lock_probe p = { .t = t, .entered = false };
    pthread_t th;
    assert(pthread_create(&th, NULL, lock_probe_worker, &p) == 0);
    for (int i = 0; i < 1000; i++)
        sched_yield();
    assert(!atomic_load(&p.entered));
    wsap_vtcore_unlock(t);

    assert(pthread_join(th, NULL) == 0);
    assert(atomic_load(&p.entered));
    wsap_vtcore_free(t);
}

int main(void)
{
    test_reserved_options_must_be_zero();
    test_concurrent_feed_and_snapshot();
    test_lock_reentrant_across_threads();
    test_grow_past_reservation();
    test_skipped_cells_read_blank();
    test_csi_text_chunks();
    test_erase_keeps_used_rows();
    test_blank_rows();
    test_lnm_mode_alone_is_not_sticky();
    test_newline_policy_survives_reset();
    test_esc_control_strings_and_designators();
    test_string_payloads();
    test_recycled_scrollback_blanks();
    test_scroll_view_clamps_extreme_deltas();
    test_batched_recycled_lines();
    test_invalid_utf8_runs();
    test_c0_ends_utf8_sequence();
    test_full_width_line_endings();
    test_batched_line_masks();
    test_bulk_line_flood();
    test_ascii_flood_edits();
    test_margin_linefeed_bursts();
    test_alt_linefeed_burst_after_resize();
    test_alt_resize_keeps_row_order();
    test_alt_shrink_then_flood();
    test_alt_erase_scrollback();
    test_snapshot_follows_new_terminal();
    test_snapshot_null_and_range();
    test_bulk_rows_survive_edits();
    test_host_name_rejected();

    assert(!strcmp(WSAP_VTCORE_UNICODE_DATA_VERSION, "18.0.0"));
    assert(!strcmp(wsap_vtcore_unicode_version(), WSAP_VTCORE_UNICODE_DATA_VERSION));
    assert(wsap_vtcore_unicode_width('A') == 1);
    assert(wsap_vtcore_unicode_width(0x0301) == 0);
    assert(wsap_vtcore_unicode_width(0x754C) == 2);
    assert(wsap_vtcore_unicode_width(0x1FAE0) == 2);
    assert(wsap_vtcore_unicode_emoji_presentation(0x1FAE0));
    assert(wsap_vtcore_unicode_width(0x1FAEB) == 2);
    assert(wsap_vtcore_unicode_emoji_presentation(0x1FAEB));
    {
        struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 10, .y = 2 } });
        wsap_vtcore_feed(t, "\xe1\x84\x80\xed\x9e\xb0", 6, 0);
        assert(wsap_vtcore_cursor(t).pos.x == 2);
        wsap_vtcore_free(t);
    }

    test_wide_characters();
    test_wide_runs();
    test_editor_scrolling();
    test_top_region_scrollback();
    test_device_attributes();
    test_unimplemented_da_features();
    test_keyboard_negotiation();
    test_resize_reflow();
    test_resize_exposes_blank_rows();
    test_prompt_resize();
    test_cursor_line_motion();
    test_relative_cursor_motion();
    test_read_boundaries();
    test_ascii_controls();
    test_random_read_boundaries();
    test_csi_recovery();
    test_csi_byte_boundaries();
    test_parameter_limits();
    test_unterminated_control_strings_do_not_swallow_text();
    test_c0_controls_after_esc_are_executed();
    struct wsap_vtcore *t = tt_new_opts(10, 4, &reply_cb);
    write_seq(t, "\033(Babc\r\nD");
    assert(tt_cell(t, 0, 0)->text == 'a');
    assert(tt_cell(t, 0, 1)->text == 'D');
    write_seq(t, "\033[H/tmp/proj\r\n$ ");
    assert(tt_cell(t, 0, 1)->text == '$');
    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 1);
    write_seq(t, "\r>");
    assert(tt_cell(t, 0, 1)->text == '>');
    assert(tt_cursor_x(t) == 1 && tt_cursor_y(t) == 1);
    write_seq(t, "\r\n$");
    assert(tt_cell(t, 0, 2)->text == '$');
    assert(tt_cursor_x(t) == 1 && tt_cursor_y(t) == 2);
    write_seq(t, "\033[H\033(0lqqk\033(B");
    assert(tt_cell(t, 0, 0)->text == 0x250c);
    assert(tt_cell(t, 3, 0)->text == 0x2510);
    write_seq(t, "\033[3d\rX\033[4bY");
    assert(tt_cell(t, 4, 2)->text == 'X');
    assert(tt_cell(t, 5, 2)->text == 'Y');
    write_seq(t, "\033[3;3H\033[2X");
    assert(tt_cell(t, 2, 2)->text == ' ' && tt_cell(t, 3, 2)->text == ' ');
    assert(tt_cell(t, 4, 2)->text == 'X' && tt_cursor_x(t) == 2);

    write_seq(t, "\033[2J\033[H1234567890");
    assert(tt_cell(t, 9, 0)->text == '0');
    assert(tt_cursor_x(t) == 9 && tt_cursor_y(t) == 0);
    write_seq(t, "A");
    assert(tt_cell(t, 0, 1)->text == 'A');
    assert(tt_cursor_x(t) == 1 && tt_cursor_y(t) == 1);

    write_seq(t, "\033[2J\033[Habcdefghij\r\nX");
    assert(tt_cell(t, 9, 0)->text == 'j');
    assert(tt_cell(t, 0, 1)->text == 'X' && tt_cursor_y(t) == 1);

    write_seq(t, "\033[2J\033[H\033[99GZ");
    assert(tt_cell(t, 9, 0)->text == 'Z' && tt_cursor_x(t) == 9);
    write_seq(t, "\033[2J\033[9;99HY");
    assert(tt_cursor_y(t) == 3 && tt_cell(t, 9, 3)->text == 'Y');

    write_seq(t, "\033[2J\033[H\033[?7l1234567890AB");
    assert(tt_cell(t, 9, 0)->text == 'B' && tt_cursor_y(t) == 0);
    write_seq(t, "\033[?7h");

    write_seq(t, "\033[2J\033[Habcd\033[2G\033[4hX\033[4l");
    assert(row_is(t, 0, "aXbcd"));

    write_seq(t, "\033[2;4r\033[?6h\033[2;3H@");
    assert(tt_cell(t, 2, 2)->text == '@');
    write_seq(t, "\033[?6l\033[r");

    write_seq(t, "\033[2J\033[H\033[3g\033[4G\033H\r\tT");
    assert(tt_cell(t, 3, 0)->text == 'T');

    write_seq(t, "\033[3;5H\033[5n");
    assert(!strcmp(response, "\033[0n"));
    write_seq(t, "\033[6n");
    assert(!strcmp(response, "\033[3;5R"));

    write_seq(t, "\033[2J\033[?69h\033[3;6s\033[?6h\033[Habcdef");
    assert(tt_cell(t, 2, 0)->text == 'a' && tt_cell(t, 5, 0)->text == 'd');
    assert(tt_cell(t, 2, 1)->text == 'e' && tt_cell(t, 3, 1)->text == 'f');
    write_seq(t, "\033[?6;69l");

    write_seq(t, "\033[?1;5;1004;2026h\033[!p");
    struct wsap_vtcore_modes modes = wsap_vtcore_modes(t);
    assert(!modes.app_cursor && !modes.focus_events);
    assert(!modes.synchronized && !modes.reverse_video);
    write_seq(t, "kept\033c");
    assert(row_is(t, 0, ""));

    write_seq(t, "\033[2J\033[Hz\033[11b");
    assert(tt_cell(t, 9, 0)->text == 'z' && tt_cell(t, 1, 1)->text == 'z');
    assert(tt_cell(t, 2, 1)->text == ' ');
    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 1);
    write_seq(t, "\033[2J\033[H1234567890\rA");
    assert(tt_cell(t, 0, 0)->text == 'A');
    write_seq(t, "\033[2;4H\033[2J");
    assert(tt_cursor_x(t) == 3 && tt_cursor_y(t) == 1);
    assert(wsap_vtcore_modes(t).mouse == WSAP_VTCORE_MOUSE_OFF);
    write_seq(t, "\033[?1000;1006h");
    assert(wsap_vtcore_modes(t).mouse == WSAP_VTCORE_MOUSE_NORMAL);
    assert(wsap_vtcore_modes(t).mouse_encoding == WSAP_VTCORE_MOUSE_ENC_SGR);
    write_seq(t, "\033[?1002h");
    assert(wsap_vtcore_modes(t).mouse == WSAP_VTCORE_MOUSE_BUTTON);
    assert(wsap_vtcore_modes(t).mouse_encoding == WSAP_VTCORE_MOUSE_ENC_SGR);
    write_seq(t, "\033[?1003h");
    assert(wsap_vtcore_modes(t).mouse == WSAP_VTCORE_MOUSE_ANY);
    write_seq(t, "\033[?1006l");
    assert(wsap_vtcore_modes(t).mouse_encoding == WSAP_VTCORE_MOUSE_ENC_X10);
    write_seq(t, "\033[?1015h");
    assert(wsap_vtcore_modes(t).mouse_encoding == WSAP_VTCORE_MOUSE_ENC_URXVT);
    write_seq(t, "\033[?1016h");
    assert(wsap_vtcore_modes(t).mouse_encoding == WSAP_VTCORE_MOUSE_ENC_SGR_PIXELS);
    write_seq(t, "\033[?1004h");
    wsap_vtcore_focus(t, false);
    assert(!strcmp(response, "\033[O"));
    wsap_vtcore_focus(t, true);
    assert(!strcmp(response, "\033[I"));
    write_seq(t, "\033[?1003l");
    assert(wsap_vtcore_modes(t).mouse == WSAP_VTCORE_MOUSE_OFF);
    response_len = 0;
    write_seq(t, "\033]10;?\007");
    assert(!strcmp(response, "\033]10;rgb:ffff/ffff/ffff\033\\"));
    write_seq(t, "\033P+q696e646e\033\\");
    assert(!strcmp(response, "\033P0+r696e646e\033\\"));
    write_seq(t, "\033P+q6b696e64\033\\");
    assert(!strcmp(response, "\033P1+r6b696e64=1b5b313b3242\033\\"));
    wsap_vtcore_free(t);
    t = tt_new(40, 4);
    write_seq(t, "hello 世界  \r\nsecond line\r\nhttps://example.com/path.");
    tt_select(t, 9, 0, 0, 0);
    char *text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "hello 世界"));
    free(text);
    tt_select(t, 5, 1, 0, 0);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "hello 世界\nsecond"));
    free(text);
    assert(tt_selected(t, 0, 0) && tt_selected(t, 5, 1));
    assert(!tt_selected(t, 6, 1));
    text = tt_link_at(t, 10, 2);
    assert(text && !strcmp(text, "https://example.com/path"));
    free(text);
    assert(!tt_link_at(t, 10, 1));
    write_seq(t, "\033[?2004h");
    assert(wsap_vtcore_modes(t).bracketed_paste);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "hello 世界\nsecond"));
    free(text);
    write_seq(t, "\033[?2004l\r\nlast\r\n");
    assert(!wsap_vtcore_modes(t).bracketed_paste);
    wsap_vtcore_scroll(t, 1);
    tt_select(t, 0, 0, 9, 0);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "hello 世界"));
    free(text);
    tt_resize(t, 50, 4);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "hello 世界"));
    free(text);
    wsap_vtcore_free(t);
    t = tt_new(40, 4);
    write_seq(t, "\033[31;44mA\033[>4;0mB\033[>4;1m\033[31mC");
    assert(tt_cell(t, 0, 0)->fg == tt_cell(t, 1, 0)->fg);
    assert(tt_cell(t, 0, 0)->bg == tt_cell(t, 1, 0)->bg);
    assert(tt_cell(t, 0, 0)->fg == tt_cell(t, 2, 0)->fg);
    assert(tt_cell(t, 0, 0)->bg == tt_cell(t, 2, 0)->bg);
    assert(tt_cursor_x(t) == 3 && tt_cursor_y(t) == 0);
    write_seq(t, "\033[0mD");
    assert(tt_cell(t, 3, 0)->fg == WSAP_VTCORE_COLOR_FG);
    assert(tt_cell(t, 3, 0)->bg == WSAP_VTCORE_COLOR_BG);
    wsap_vtcore_free(t);

    t = tt_new(10, 4);
    write_seq(t, "ABCDEFGHIJ\r\n\r\n\r\n\r\n");
    wsap_vtcore_scroll(t, 1);
    assert(tt_view_cell(t, 0, 0)->text == 'A');
    assert(tt_view_cell(t, 9, 0)->text == 'J');
    wsap_vtcore_scroll_to(t, 0);

    for (int i = 0; i < TEST_SCROLLBACK; i++)
        write_seq(t, "xy\r\n");
    wsap_vtcore_scroll(t, 1);
    assert(tt_view_cell(t, 0, 0)->text == 'x');
    for (int c = 2; c < 10; c++) {
        assert(tt_view_cell(t, c, 0)->text == ' ');
        assert(tt_view_cell(t, c, 0)->bg == WSAP_VTCORE_COLOR_BG);
    }
    wsap_vtcore_free(t);

    t = tt_new(10, 4);
    write_seq(t, "\033[41m\033[2K");
    wsap_vtcore_color erased_bg = tt_cell(t, 0, 0)->bg;
    assert(erased_bg != WSAP_VTCORE_COLOR_BG);
    assert(tt_cell(t, 9, 0)->bg == erased_bg);
    write_seq(t, "\033[m\n\n\n\n");
    wsap_vtcore_scroll(t, 1);
    assert(tt_view_cell(t, 9, 0)->bg == erased_bg);
    assert(tt_view_cell(t, 10, 0)->bg == WSAP_VTCORE_COLOR_BG);
    wsap_vtcore_free(t);

    t = tt_new(10, 4);
    write_seq(t, "\033[Hnormal");
    write_seq(t, "\033[?1049h");
    for (int c = 0; c < 10; c++) {
        assert(tt_cell(t, c, 0)->text == ' ');
        assert(tt_cell(t, c, 0)->bg == WSAP_VTCORE_COLOR_BG);
    }
    write_seq(t, "\033[Halt\n\033[2J");
    write_seq(t, "\033[?1049l");
    assert(tt_cell(t, 0, 0)->text == 'n' && tt_cell(t, 5, 0)->text == 'l');
    wsap_vtcore_free(t);

    t = tt_new(20, 8);
    write_seq(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n$ ");
    assert(tt_cursor_y(t) == 5 && tt_history(t) == 0);
    tt_resize(t, 20, 3);
    assert(tt_cursor_y(t) == 2 && tt_history(t) == 3);
    assert(row_is(t, 0, "four") && row_is(t, 1, "five") && row_is(t, 2, "$"));
    wsap_vtcore_scroll(t, 3);
    assert(row_is(t, 0, "one") && row_is(t, 1, "two") && row_is(t, 2, "three"));
    wsap_vtcore_scroll_to(t, 0);
    tt_resize(t, 20, 8);
    assert(tt_cursor_y(t) == 5 && tt_history(t) == 0);
    assert(row_is(t, 0, "one") && row_is(t, 2, "three") && row_is(t, 5, "$"));
    assert(row_is(t, 6, "") && row_is(t, 7, ""));

    tt_resize(t, 20, 6);
    assert(tt_cursor_y(t) == 5 && tt_history(t) == 0);
    assert(row_is(t, 0, "one") && row_is(t, 5, "$"));
    wsap_vtcore_free(t);

    t = tt_new(20, 6);
    write_seq(t, "a\r\nb\r\nc\r\nd\r\ne\r\n");
    write_seq(t, "\033[?1049h\033[6;1Hbottom");
    int before = tt_history(t);
    tt_resize(t, 20, 3);
    assert(tt_history(t) == before);
    tt_resize(t, 20, 6);
    assert(tt_history(t) == before);
    write_seq(t, "\033[?1049l");
    assert(row_is(t, 0, "a") && row_is(t, 4, "e"));
    wsap_vtcore_free(t);

    t = tt_new(40, 6);
    write_seq(t, "\033[?1049h\033[1;38Hxy");
    tt_resize(t, 12, 6);
    write_seq(t, "\033[?1049l");
    assert(tt_cursor_x(t) < tt_cols(t));
    write_seq(t, "Q");
    assert(tt_cell(t, tt_cursor_x(t) - 1, tt_cursor_y(t))->text == 'Q');
    wsap_vtcore_free(t);

    t = tt_new(8, 4);
    write_seq(t, "abcdefghijkl");
    tt_select(t, 0, 0, 3, 1);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "abcdefghijkl"));
    free(text);
    write_seq(t, "\033[2J\033[Habcd\r\nefgh");
    tt_select(t, 0, 0, 3, 1);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "abcd\nefgh"));
    free(text);
    wsap_vtcore_free(t);

    t = tt_new(10, 4);
    write_seq(t, "abcdef");
    write_seq(t, "\033[1;3H\033[P");
    assert(row_is(t, 0, "abdef"));
    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
    write_seq(t, "\033[2P");
    assert(row_is(t, 0, "abf"));
    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
    write_seq(t, "\033[10P");
    assert(row_is(t, 0, "ab"));
    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
    wsap_vtcore_free(t);

    t = tt_new(8, 4);
    write_seq(t, "abcdef");
    write_seq(t, "\033[1;3H\033[@");
    assert(row_is(t, 0, "ab cdef"));
    assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
    write_seq(t, "\033[2@");
    assert(row_is(t, 0, "ab   cde"));
    wsap_vtcore_free(t);

    t = tt_new(10, 4);
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK);
    assert(wsap_vtcore_cursor(t).blink == true);

    write_seq(t, "\033[2 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK);
    assert(wsap_vtcore_cursor(t).blink == false);

    write_seq(t, "\033[3 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_UNDERLINE);
    assert(wsap_vtcore_cursor(t).blink == true);

    write_seq(t, "\033[4 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_UNDERLINE);
    assert(wsap_vtcore_cursor(t).blink == false);

    write_seq(t, "\033[5 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BAR);
    assert(wsap_vtcore_cursor(t).blink == true);

    write_seq(t, "\033[6 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BAR);
    assert(wsap_vtcore_cursor(t).blink == false);

    write_seq(t, "\033[0 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK);
    assert(wsap_vtcore_cursor(t).blink == true);

    write_seq(t, "\033[5 q");
    write_seq(t, "\033[ q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK);
    assert(wsap_vtcore_cursor(t).blink == true);

    write_seq(t, "\033[6 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BAR && !wsap_vtcore_cursor(t).blink);
    write_seq(t, "\033[?1049h");
    write_seq(t, "\033[3 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_UNDERLINE && wsap_vtcore_cursor(t).blink);
    write_seq(t, "\033[?1049l");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BAR && !wsap_vtcore_cursor(t).blink);

    wsap_vtcore_set_cursor_default(t, WSAP_VTCORE_CURSOR_UNDERLINE, true);

    write_seq(t, "\033[5 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BAR && wsap_vtcore_cursor(t).blink);
    write_seq(t, "\033[0 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_UNDERLINE && !wsap_vtcore_cursor(t).blink);

    write_seq(t, "\033[2 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK && !wsap_vtcore_cursor(t).blink);
    write_seq(t, "\033[ q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_UNDERLINE && !wsap_vtcore_cursor(t).blink);

    write_seq(t, "\033[1 q");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK && wsap_vtcore_cursor(t).blink);
    write_seq(t, "\033[!p");
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_UNDERLINE && !wsap_vtcore_cursor(t).blink);
    wsap_vtcore_free(t);

    t = tt_new(20, 4);
    write_seq(t, "\033[1;2;3;4;9;53mA\033[21mB\033[4:2mC\033[22;24mD\033[0mE");
    const uint16_t styles = WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_FAINT | WSAP_VTCORE_ATTR_ITALIC |
                            WSAP_VTCORE_ATTR_STRIKE | WSAP_VTCORE_ATTR_OVERLINE;
    assert((tt_cell(t, 0, 0)->attrs & styles) == styles);
    assert(wsap_vtcore_attr_underline(tt_cell(t, 0, 0)->attrs) == WSAP_VTCORE_UNDERLINE_SINGLE);
    assert(wsap_vtcore_attr_underline(tt_cell(t, 1, 0)->attrs) == WSAP_VTCORE_UNDERLINE_DOUBLE);
    assert(wsap_vtcore_attr_underline(tt_cell(t, 2, 0)->attrs) == WSAP_VTCORE_UNDERLINE_DOUBLE);
    assert(!(tt_cell(t, 3, 0)->attrs & (WSAP_VTCORE_ATTR_BOLD | WSAP_VTCORE_ATTR_FAINT)));
    assert(wsap_vtcore_attr_underline(tt_cell(t, 3, 0)->attrs) == WSAP_VTCORE_UNDERLINE_NONE);
    assert(tt_cell(t, 4, 0)->attrs == 0);
    wsap_vtcore_free(t);

    t = tt_new(20, 4);
    write_seq(t, "\033[30mA\033[0;1;30mB\033[0;90mC");
    assert(tt_cell(t, 0, 0)->fg == tt_cell(t, 1, 0)->fg);
    assert(tt_cell(t, 1, 0)->fg != tt_cell(t, 2, 0)->fg);
    assert(!(tt_cell(t, 0, 0)->attrs & WSAP_VTCORE_ATTR_BOLD));
    assert(tt_cell(t, 1, 0)->attrs & WSAP_VTCORE_ATTR_BOLD);
    wsap_vtcore_free(t);

    const char *colon_rgb = "\033[48;2;145;2;34m\033[38;2;12;34;56mA"
                            "\033[38:2::12:34:56mB\033[;mC";
    for (size_t split = 0; split <= strlen(colon_rgb); split++) {
        t = tt_new(20, 4);
        tt_write(t, colon_rgb, split);
        tt_write(t, colon_rgb + split, strlen(colon_rgb) - split);
        assert(row_is(t, 0, "ABC"));
        for (int x = 0; x < 2; x++) {
            assert(tt_cell(t, x, 0)->fg == 0xFF0C2238);
            assert(tt_cell(t, x, 0)->bg == 0xFF910222);
        }
        assert(tt_cell(t, 2, 0)->fg == WSAP_VTCORE_COLOR_FG);
        assert(tt_cell(t, 2, 0)->bg == WSAP_VTCORE_COLOR_BG);
        wsap_vtcore_free(t);
    }

    t = tt_new(20, 4);
    write_seq(t, "\033[38;2;246;226;183;49mX");
    assert(tt_cell(t, 0, 0)->fg == 0xFFF6E2B7);
    assert(tt_cell(t, 0, 0)->bg == WSAP_VTCORE_COLOR_BG);
    wsap_vtcore_free(t);

    t = tt_new(20, 4);
    const char *clusters = "(\xc2\xa9\xef\xb8\x8f)\xe2\x98\xba\xef\xb8\x8e"
                           "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x8c\xbe";
    write_seq(t, clusters);

    assert(tt_cursor_x(t) == 7);
    assert((tt_cell(t, 1, 0)->attrs & WSAP_VTCORE_ATTR_EMOJI) && tt_cell(t, 1, 0)->width == 2);
    assert(tt_cell(t, 2, 0)->width == 0);
    assert((tt_cell(t, 4, 0)->attrs & WSAP_VTCORE_ATTR_TEXT) && tt_cell(t, 4, 0)->width == 1);
    assert((tt_cell(t, 5, 0)->attrs & WSAP_VTCORE_ATTR_EMOJI) && tt_cell(t, 5, 0)->width == 2);
    assert(tt_cell(t, 6, 0)->width == 0);

    tt_select(t, 0, 0, 6, 0);
    text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, clusters));
    free(text);
    wsap_vtcore_free(t);

    t = tt_new(5, 2);
    write_seq(t, "12345");
    assert(row_is(t, 0, "12345"));
    assert(tt_cursor_x(t) == 4 && tt_cursor_y(t) == 0);
    write_seq(t, "\b");
    assert(tt_cursor_x(t) == 3 && tt_cursor_y(t) == 0);
    write_seq(t, "X");
    assert(row_is(t, 0, "123X5"));
    assert(tt_cursor_x(t) == 4 && tt_cursor_y(t) == 0);
    write_seq(t, "\b \b");
    assert(row_is(t, 0, "123 5"));
    assert(tt_cursor_x(t) == 3 && tt_cursor_y(t) == 0);
    wsap_vtcore_free(t);

    t = tt_new(10, 2);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    write_seq(t, "abc");
    uint64_t version = view_version(snap, t);
    write_seq(t, "\b");
    assert(tt_cursor_x(t) == 2 && view_version(snap, t) != version);
    version = view_version(snap, t);
    write_seq(t, "\r");
    assert(tt_cursor_x(t) == 0 && view_version(snap, t) != version);
    version = view_version(snap, t);
    write_seq(t, "\t");
    assert(tt_cursor_x(t) == 8 && view_version(snap, t) != version);
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);

    t = tt_new(20, 2);
    tt_write(t, "hello world", 11);
    assert(row_is(t, 0, "hello world"));
    assert(tt_cursor_x(t) == 11 && tt_cursor_y(t) == 0);
    wsap_vtcore_free(t);

    title[0] = 0;
    t = tt_new_opts(10, 4, &title_cb);
    write_seq(t, "\033]0;My Shell\007");
    assert(!strcmp(title, "My Shell"));
    write_seq(t, "\033]2;Second Title\033\\");
    assert(!strcmp(title, "Second Title"));
    write_seq(t, "\033]1;Icon Name\007");
    assert(!strcmp(title, "Icon Name"));
    wsap_vtcore_free(t);

    t = tt_new(20, 4);
    write_seq(t, "$ /mnt/work");
    const char *round_trip = "\033[?1049h\033[?1049l";
    tt_write(t, round_trip, strlen(round_trip));
    assert(row_is(t, 0, "$ /mnt/work"));
    wsap_vtcore_free(t);

    t = tt_new(20, 4);
    snap = wsap_vtcore_snapshot_new();
    write_seq(t, "$ /mnt/work");
    version = view_version(snap, t);
    write_seq(t, "\033[?1049h");
    assert(view_version(snap, t) != version);
    version = view_version(snap, t);
    write_seq(t, "\033[?1049l");
    assert(view_version(snap, t) != version);
    assert(row_is(t, 0, "$ /mnt/work"));
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);

    t = tt_new(20, 4);
    write_seq(t, "output");
    write_seq(t, "\r\033[K../");
    assert(row_is(t, 0, "../"));
    wsap_vtcore_free(t);

    t = tt_new(80, 24);
    assert(!wsap_vtcore_modes(t).alternate);
    write_seq(t, "\033[?1049h");
    assert(wsap_vtcore_modes(t).alternate);
    write_seq(t, "\033[?1049l");
    assert(!wsap_vtcore_modes(t).alternate);
    wsap_vtcore_free(t);

    const char *color_runs[] = {
        "\033[1;1H\033[48;2;10;20;30m\033[38;2;200;210;220ma"
        "\033[48;2;100;110;120m\033[38;2;230;240;250mb",
        "\033[1;1H\033[38;2;200;210;220m\033[48;2;10;20;30ma"
        "\033[38;2;230;240;250m\033[48;2;100;110;120mb",
    };
    for (size_t run = 0; run < sizeof(color_runs) / sizeof(color_runs[0]); run++) {
        const char *seq = color_runs[run];
        size_t len = strlen(seq);
        for (size_t split = 0; split <= len; split++) {
            t = tt_new(100, 2);
            tt_write(t, seq, split);
            tt_write(t, seq + split, len - split);
            assert(row_is(t, 0, "ab"));
            assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
            assert(tt_cell(t, 0, 0)->fg == 0xffc8d2dc);
            assert(tt_cell(t, 0, 0)->bg == 0xff0a141e);
            assert(tt_cell(t, 1, 0)->fg == 0xffe6f0fa);
            assert(tt_cell(t, 1, 0)->bg == 0xff646e78);
            for (int col = 2; col < 100; col++)
                assert(tt_cell(t, col, 0)->bg == WSAP_VTCORE_COLOR_BG);
            assert(row_is(t, 1, ""));
            wsap_vtcore_free(t);
        }
    }

    puts("PASS: terminal regression tests");
    return 0;
}
