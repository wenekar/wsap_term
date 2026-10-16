#include "term_api.h"
#include "wsap_vtcore/unicode/unicode.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_seq(struct wsap_vtcore *t, const char *s)
{
    for (; *s; s++) tt_write(t, s, 1);
}

static void write_buf(struct wsap_vtcore *t, const char *s, size_t len)
{
    tt_write(t, s, len);
}

static char reply[256];
static void capture_reply(void *ud, const char *s, size_t len)
{
    (void)ud;
    snprintf(reply, sizeof(reply), "%.*s", (int)len, s);
}

static char title[256];
static void capture_title(void *ud, const char *s)
{
    (void)ud;
    snprintf(title, sizeof(title), "%s", s ? s : "");
}

static char clip_sel[16];
static char clip_data[4096];
static size_t clip_len;
static int clip_count;
static void capture_clip(void *ud, const char *sel, const char *data, size_t len)
{
    (void)ud;
    snprintf(clip_sel, sizeof(clip_sel), "%s", sel ? sel : "");
    assert(len < sizeof(clip_data));
    memcpy(clip_data, data, len);
    clip_len = len;
    clip_count++;
}

static char notify_title[256];
static char notify_body[1024];
static int notify_count;
static void capture_notify(void *ud, const char *t_, const char *b)
{
    (void)ud;
    snprintf(notify_title, sizeof(notify_title), "%s", t_ ? t_ : "");
    snprintf(notify_body, sizeof(notify_body), "%s", b ? b : "");
    notify_count++;
}

static struct wsap_vtcore *osc_test_term(void)
{
    struct wsap_vtcore *t = tt_new_opts(80, 24, &(struct wsap_vtcore_callbacks){
        .reply = capture_reply,
        .title = capture_title,
        .clipboard = capture_clip,
        .notify = capture_notify,
    });
    assert(t);
    return t;
}

/* Check the OSC 8 link or the detected URL at a viewport cell. NULL means
 * no link. */
static void assert_link(struct wsap_vtcore *t, int col, int row, const char *want)
{
    char *url = tt_link_at(t, col, row);
    if (want)
        assert(url && !strcmp(url, want));
    else
        assert(!url);
    free(url);
}

static uint32_t cursor_color(struct wsap_vtcore *t)
{
    return wsap_vtcore_snapshot_view(tt_snapshot_for(t))->palette[WSAP_VTCORE_PALETTE_CURSOR];
}

/* Send a color query and check the reply. */
static void assert_query(struct wsap_vtcore *t, const char *query, const char *want)
{
    reply[0] = 0;
    write_seq(t, query);
    if (strcmp(reply, want)) {
        fprintf(stderr, "reply to %s: got \"%s\" want \"%s\"\n", query, reply, want);
        assert(!"reply mismatch");
    }
}

static void reset_captures(void)
{
    reply[0] = 0;
    title[0] = clip_sel[0] = clip_data[0] = notify_title[0] = notify_body[0] = 0;
    clip_len = 0;
    clip_count = 0;
    notify_count = 0;
}

/* The notification default is wsap_vtcore_options.name. A DCS +p does not change it. */
static void test_host_name_notify(void)
{
    struct wsap_vtcore_options opt = {
        .size = { .x = 80, .y = 24 },
        .scrollback = TEST_SCROLLBACK,
        .name = "myterm",
        .callbacks = { .notify = capture_notify },
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    reset_captures();
    write_seq(t, "\033]9;hello\007");
    assert(notify_count == 1);
    assert(!strcmp(notify_title, "myterm"));
    assert(!strcmp(notify_body, "hello"));

    write_seq(t, "\033P+p6f74686572\033\\");
    reset_captures();
    write_seq(t, "\033]9;again\007");
    assert(notify_count == 1);
    assert(!strcmp(notify_title, "myterm"));

    write_seq(t, "\033]777;notify;Title;Body\007");
    assert(notify_count == 2);
    assert(!strcmp(notify_title, "Title"));
    wsap_vtcore_free(t);
}

static void test_osc52(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();
    write_seq(t, "\033]52;c;aGVsbG8=\007");
    assert(clip_count == 1 && !strcmp(clip_sel, "c"));
    assert(clip_len == 5 && !memcmp(clip_data, "hello", 5));

    reset_captures();
    write_seq(t, "\033]52;c;aGVsbG8=\033\\");
    assert(clip_count == 1 && clip_len == 5 && !memcmp(clip_data, "hello", 5));

    reset_captures();
    static const char bulk[] = "\033]52;c;aGVsbG8=\007";
    write_buf(t, bulk, sizeof(bulk) - 1);
    assert(clip_count == 1 && clip_len == 5 && !memcmp(clip_data, "hello", 5));

    reset_captures();
    char big[7 + 400 + 1];
    memcpy(big, "\033]52;c;", 7);
    for (int i = 0; i < 400; i += 4)
        memcpy(big + 7 + i, "QUJD", 4);
    big[407] = '\007';
    write_buf(t, big, sizeof(big));
    assert(clip_count == 1 && clip_len == 300);
    for (int i = 0; i < 300; i += 3)
        assert(!memcmp(clip_data + i, "ABC", 3));

    reset_captures();
    write_seq(t, "\033]52;c;\007");
    assert(clip_count == 1 && clip_len == 0);

    reset_captures();
    write_seq(t, "\033]52;c;?\007");
    write_seq(t, "\033]52;c;!!!invalid!!!\007");
    write_seq(t, "\033]52;aGVsbG8=\007");
    write_seq(t, "\033]52;;aGVsbG8=\007");
    assert(clip_count == 0);

    write_seq(t, "\033]52;pc;aGVsbG8=\007");
    assert(clip_count == 1 && !strcmp(clip_sel, "pc"));
    assert(clip_len == 5 && !memcmp(clip_data, "hello", 5));

    reset_captures();
    write_seq(t, "\033]52;s;AAH9\007");
    assert(clip_count == 1 && !strcmp(clip_sel, "s") && clip_len == 3);
    assert(clip_data[0] == 0 && clip_data[1] == 1 && clip_data[2] == (char)0xfd);
    wsap_vtcore_free(t);
}

/* An SGR reset must not drop an open link.
 *
 * The fast SGR path caches the cell flags, and those flags carry the link id.
 * The reset used to set the cached value to zero, which cleared the link from
 * every cell written after it. Write the whole sequence at once so the fast
 * path runs. */
static void test_sgr_reset_keeps_link(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();

    static const char seq[] = "\033]8;;https://reset.dev\033\\\033[0mX";
    write_buf(t, seq, sizeof(seq) - 1);
    assert(tt_cell(t, 0, 0)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 0, 0, "https://reset.dev");

    /* The same reset in its own write, which takes the normal parser. */
    static const char seq2[] = "\033]8;;https://second.dev\033\\\033[0m";
    write_buf(t, seq2, sizeof(seq2) - 1);
    write_buf(t, "Y", 1);
    assert(tt_cell(t, 1, 0)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 1, 0, "https://second.dev");

    wsap_vtcore_free(t);
}

static void test_osc8(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();

    write_seq(t, "\033]8;;https://example.com\033\\link text\033]8;;\033\\after");
    for (int c = 0; c < 9; c++) {
        assert(tt_cell(t, c, 0)->attrs & WSAP_VTCORE_ATTR_LINK);
        assert_link(t, c, 0, "https://example.com");
    }
    for (int c = 9; c < 14; c++) {
        assert(!(tt_cell(t, c, 0)->attrs & WSAP_VTCORE_ATTR_LINK));
        assert_link(t, c, 0, NULL);
    }

    /* Each cell keeps the URI of its own link. */
    write_seq(t, "\033[2;1H\033]8;;https://other.org\033\\bbb\033]8;;\033\\");
    write_seq(t, "\033[3;1H\033]8;;https://example.com\033\\ccc\033]8;;\033\\");
    assert(tt_cell(t, 0, 1)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert(tt_cell(t, 0, 2)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 0, 1, "https://other.org");
    assert_link(t, 2, 1, "https://other.org");
    assert_link(t, 0, 2, "https://example.com");
    assert_link(t, 0, 0, "https://example.com");

    /* An erase removes the link. */
    write_seq(t, "\033[4;1H\033]8;;https://gone.io\033\\zzz\033]8;;\033\\");
    assert(tt_cell(t, 0, 3)->attrs & WSAP_VTCORE_ATTR_LINK);
    write_seq(t, "\033[4;1H\033[K");
    assert(!(tt_cell(t, 0, 3)->attrs & WSAP_VTCORE_ATTR_LINK));
    assert_link(t, 0, 3, NULL);

    write_seq(t, "\033[5;1H\033]8;x\033\\nop\033]8;xyz\033\\qq");
    assert(!(tt_cell(t, 0, 4)->attrs & WSAP_VTCORE_ATTR_LINK));
    assert_link(t, 0, 4, NULL);
    assert(!(tt_cell(t, 4, 4)->attrs & WSAP_VTCORE_ATTR_LINK));
    write_seq(t, "\033]8;;https://stay.net\033\\\033]8;bogus\033\\ST\033]8;;\033\\");
    assert(tt_cell(t, 5, 4)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 5, 4, "https://stay.net");

    write_seq(t, "\033[6;1H\033]8;id=grp;https://params.dev\007p1\033]8;;\007");
    assert(tt_cell(t, 0, 5)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 0, 5, "https://params.dev");

    /* A link stays on its cells when they scroll into the history. */
    for (int i = 0; i < 30; i++)
        write_buf(t, "filler\r\n", 8);
    assert(tt_history(t) > 0);
    wsap_vtcore_scroll(t, tt_history(t));
    assert(tt_view_cell(t, 0, 0)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 0, 0, "https://example.com");
    wsap_vtcore_scroll_to(t, 0);

    write_seq(t, "\033[7;1H");
    for (int i = 0; i < 8300; i++) {
        char seq[64];
        int n = snprintf(seq, sizeof(seq),
                         "\033]8;;https://h/%d\033\\A\010", i);
        write_buf(t, seq, (size_t)n);
    }
    assert_link(t, 0, 6, "https://h/8299");
    assert_link(t, -1, 0, NULL);
    assert_link(t, 0, -1, NULL);
    assert_link(t, 80, 0, NULL);
    assert_link(t, 0, 24, NULL);

    write_seq(t, "\033[?1049h");
    write_seq(t, "\033[1;1H\033]8;;https://altscreen.test\033\\ALT\033]8;;\033\\");
    assert_link(t, 0, 0, "https://altscreen.test");
    write_seq(t, "\033[?1049l");
    wsap_vtcore_scroll(t, tt_history(t));
    assert_link(t, 0, 0, "https://example.com");
    wsap_vtcore_scroll_to(t, 0);
    write_seq(t, "\033[8;1HSee file:///home/user/code/file.c#L10-L20 for info");
    assert_link(t, 10, 7, "file:///home/user/code/file.c#L10-L20");

    wsap_vtcore_free(t);
}

static bool text_is(char *(*get)(const struct wsap_vtcore *),
                    const struct wsap_vtcore *t, const char *want)
{
    char *text = get(t);
    bool same = text && !strcmp(text, want);
    wsap_vtcore_text_free(text);
    return same;
}

static void test_osc_misc(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();

    write_seq(t, "\033]7;file://host/home/user/proj\007");
    assert(text_is(wsap_vtcore_cwd, t, "/home/user/proj"));

    write_seq(t, "\033]7;file:///a%20b\033\\");
    assert(text_is(wsap_vtcore_cwd, t, "/a b"));

    const char *bulk = "\033]7;file://h/bulk%20dir\007";
    write_buf(t, bulk, strlen(bulk));
    assert(text_is(wsap_vtcore_cwd, t, "/bulk dir"));

    write_seq(t, "\033]7;https://x/y\007");
    write_seq(t, "\033]7;notauri\007");
    assert(text_is(wsap_vtcore_cwd, t, "/bulk dir"));

    write_seq(t, "\033]9;hello there\033\\");
    assert(notify_count == 1);
    assert(!strcmp(notify_title, "wsap_vtcore"));
    assert(!strcmp(notify_body, "hello there"));
    write_seq(t, "\033]9;4;3;50\007");
    assert(notify_count == 1);

    write_seq(t, "\033]777;notify;Title;Body text\007");
    assert(notify_count == 2);
    assert(!strcmp(notify_title, "Title"));
    assert(!strcmp(notify_body, "Body text"));
    write_seq(t, "\033]777;other;T;B\007");
    assert(notify_count == 2);

    write_seq(t, "\033]22;pointer\033\\");
    assert(text_is(wsap_vtcore_pointer_shape, t, "pointer"));
    write_seq(t, "\033]22;text\007");
    assert(text_is(wsap_vtcore_pointer_shape, t, "text"));
    write_seq(t, "\033]22;\033\\");
    assert(!wsap_vtcore_pointer_shape(t));

    wsap_vtcore_free(t);
}

/* OSC 4, 10, 11, and 12 change the colors of one terminal. The reset codes
 * restore the colors of its theme. */
static void test_osc_colors(void)
{
    struct wsap_vtcore_theme theme;
    wsap_vtcore_theme_default(&theme);
    struct wsap_vtcore *t = osc_test_term();
    struct wsap_vtcore *other = osc_test_term();
    reset_captures();

    /* tt_new starts with the built-in theme. */
    assert(tt_color(t, WSAP_VTCORE_COLOR_FG) == 0xFFFFFFFFu && theme.fg == 0xFFFFFFFFu);
    assert(tt_color(t, WSAP_VTCORE_COLOR_BG) == 0xFF1E1E2Eu && theme.bg == 0xFF1E1E2Eu);
    assert(cursor_color(t) == 0xFF89B4FAu && theme.cursor == 0xFF89B4FAu);
    assert_query(t, "\033]10;?\007", "\033]10;rgb:ffff/ffff/ffff\033\\");

    write_seq(t, "\033]10;#ff0000\007");
    assert(tt_color(t, WSAP_VTCORE_COLOR_FG) == 0xFFFF0000u);
    assert_query(t, "\033]10;?\007", "\033]10;rgb:ffff/0000/0000\033\\");
    write_seq(t, "\033]110\007");
    assert(tt_color(t, WSAP_VTCORE_COLOR_FG) == theme.fg);
    assert_query(t, "\033]10;?\033\\", "\033]10;rgb:ffff/ffff/ffff\033\\");

    write_seq(t, "\033]11;#00ff00\033\\");
    assert(tt_color(t, WSAP_VTCORE_COLOR_BG) == 0xFF00FF00u);
    assert_query(t, "\033]11;?\007", "\033]11;rgb:0000/ffff/0000\033\\");
    write_seq(t, "\033]111\007");
    assert(tt_color(t, WSAP_VTCORE_COLOR_BG) == theme.bg);
    assert_query(t, "\033]11;?\007", "\033]11;rgb:1e1e/1e1e/2e2e\033\\");

    write_seq(t, "\033]12;#0000ff\007");
    assert(cursor_color(t) == 0xFF0000FFu);
    assert_query(t, "\033]12;?\007", "\033]12;rgb:0000/0000/ffff\033\\");
    write_seq(t, "\033]112\033\\");
    assert(cursor_color(t) == theme.cursor);
    assert_query(t, "\033]12;?\007", "\033]12;rgb:8989/b4b4/fafa\033\\");

    uint32_t ansi5 = tt_color(t, wsap_vtcore_color_index(5));
    assert(ansi5 == theme.ansi[5]);
    write_seq(t, "\033]4;5;#ff0000\007");
    assert(tt_color(t, wsap_vtcore_color_index(5)) == 0xFFFF0000u);
    assert(tt_color(t, wsap_vtcore_color_index(4)) == theme.ansi[4]);

    /* A color change in one terminal does not change another terminal. */
    write_seq(t, "\033]10;#123456\007");
    assert(tt_color(other, wsap_vtcore_color_index(5)) == theme.ansi[5]);
    assert(tt_color(other, WSAP_VTCORE_COLOR_FG) == theme.fg);
    assert_query(other, "\033]10;?\007", "\033]10;rgb:ffff/ffff/ffff\033\\");
    assert(tt_color(t, WSAP_VTCORE_COLOR_FG) == 0xFF123456u);

    write_seq(t, "\033]104;5\007");
    assert(tt_color(t, wsap_vtcore_color_index(5)) == ansi5);
    write_seq(t, "\033]4;1;#010203;2;#040506\007");
    assert(tt_color(t, wsap_vtcore_color_index(1)) == 0xFF010203u);
    assert(tt_color(t, wsap_vtcore_color_index(2)) == 0xFF040506u);
    write_seq(t, "\033]104\007");
    assert(tt_color(t, wsap_vtcore_color_index(1)) == theme.ansi[1]);
    assert(tt_color(t, wsap_vtcore_color_index(2)) == theme.ansi[2]);

    wsap_vtcore_free(other);
    wsap_vtcore_free(t);
}

static void test_osc_legacy_titles(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();

    write_seq(t, "\033]legacy title\007");
    assert(!strcmp(title, "legacy title"));
    write_seq(t, "\033]\007");
    assert(!strcmp(title, ""));
    write_seq(t, "\033]2adjacent\033\\");
    assert(!strcmp(title, "adjacent"));

    wsap_vtcore_free(t);
}

static void test_palette_and_theme(void)
{
    uint32_t c;
    /* Hex formats */
    assert(wsap_vtcore_color_parse("#f00", &c) && c == 0xFFFF0000u);
    assert(wsap_vtcore_color_parse("#00ff00", &c) && c == 0xFF00FF00u);
    assert(wsap_vtcore_color_parse(" #0000ff \t", &c) && c == 0xFF0000FFu);
    assert(wsap_vtcore_color_parse("123456", &c) && c == 0xFF123456u);
    assert(wsap_vtcore_color_parse("abc", &c) && c == 0xFFAABBCCu);
    assert(wsap_vtcore_color_parse("#123456789abc", &c) && c == 0xFF12569au);
    assert(wsap_vtcore_color_parse("#123456789", &c) && c == 0xFF124578u);
    assert(!wsap_vtcore_color_parse("#12g456789", &c));
    assert(!wsap_vtcore_color_parse("#123456789abg", &c));

    /* rgb: / rgbi: */
    assert(wsap_vtcore_color_parse("rgb:ff/80/00", &c) && c == 0xFFFF8000u);
    assert(wsap_vtcore_color_parse("rgb:f/123/4567", &c) && c == 0xFFFF1245u);
    assert(!wsap_vtcore_color_parse("rgb:fffff/0/0", &c));
    assert(wsap_vtcore_color_parse("rgbi:1.0/0.0/0.5", &c) && c == 0xFFFF0080u);
    assert(wsap_vtcore_color_parse("rgbi:.25/.5/.75", &c));

    /* Errors */
    assert(!wsap_vtcore_color_parse(NULL, &c));
    assert(!wsap_vtcore_color_parse("#ff", &c));
    assert(!wsap_vtcore_color_parse("", &c));
    assert(!wsap_vtcore_color_parse("rgb:ff/80", &c));
    assert(!wsap_vtcore_color_parse("rgb:ff/80/00/11", &c));
    assert(!wsap_vtcore_color_parse("rgbi:2.0/0/0", &c));
    assert(!wsap_vtcore_color_parse("rgbi:./0/0", &c));

    /* Theme APIs */
    struct wsap_vtcore_theme theme;
    wsap_vtcore_theme_default(&theme);
    assert(wsap_vtcore_theme_set_key(&theme, "foreground", "#112233") && theme.fg == 0xFF112233u);
    assert(wsap_vtcore_theme_set_key(&theme, "foreground", "") && theme.fg == 0xFFFFFFFFu);
    assert(wsap_vtcore_theme_set_key(&theme, "background", "#445566") && theme.bg == 0xFF445566u);
    assert(wsap_vtcore_theme_set_key(&theme, "background", "") && theme.bg == 0xFF1E1E2Eu);
    assert(wsap_vtcore_theme_set_key(&theme, "color0", "#010203") && theme.ansi[0] == 0xFF010203u);
    assert(wsap_vtcore_theme_set_key(&theme, "color15", "#fafbfc") && theme.ansi[15] == 0xFFFAFBFCu);
    assert(wsap_vtcore_theme_set_key(&theme, "color0", "") && theme.ansi[0] == 0xFF585B70u);
    assert(!wsap_vtcore_theme_set_key(&theme, "color16", "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, "color01", "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, "color", "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, "color1x", "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, "color100000000000000000000", "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, "invalid", "#000000"));
    assert(!wsap_vtcore_theme_set_key(NULL, "foreground", "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, NULL, "#000000"));
    assert(!wsap_vtcore_theme_set_key(&theme, "foreground", NULL));

    struct wsap_vtcore_theme hc;
    wsap_vtcore_theme_high_contrast(&hc);
    assert(hc.bg == 0xFF000000u && hc.fg == 0xFFFFFFFFu);
}

int main(void)
{
    test_osc52();
    test_osc8();
    test_sgr_reset_keeps_link();
    test_osc_misc();
    test_host_name_notify();
    test_osc_colors();
    test_osc_legacy_titles();
    test_palette_and_theme();
    puts("PASS: OSC tests");
    return 0;
}
