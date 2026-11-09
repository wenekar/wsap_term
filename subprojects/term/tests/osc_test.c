#include "term_api.h"
#include "wsap_vtcore/unicode/unicode.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_seq(struct wsap_vtcore *t, const char *s)
{
    for (; *s; s++) tt_write(t, s, 1);
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
static char clip_data[8192];
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

static enum wsap_vtcore_progress progress_state;
static int progress_percent;
static int progress_count;
static void capture_progress(void *ud, enum wsap_vtcore_progress state, int percent)
{
    (void)ud;
    progress_state = state;
    progress_percent = percent;
    progress_count++;
}

static char var_name[512];
static char var_value[8192];
static size_t var_len;
static int var_count;
static void capture_user_var(void *ud, const char *name, const char *value, size_t len)
{
    (void)ud;
    snprintf(var_name, sizeof(var_name), "%s", name);
    assert(len < sizeof(var_value) && value[len] == '\0');
    memcpy(var_value, value, len + 1);
    var_len = len;
    var_count++;
}

static struct wsap_vtcore *osc_test_term(void)
{
    struct wsap_vtcore *t = tt_new_opts(80, 24, &(struct wsap_vtcore_callbacks){
        .reply = capture_reply,
        .title = capture_title,
        .clipboard = capture_clip,
        .notify = capture_notify,
        .progress = capture_progress,
        .user_var = capture_user_var,
    });
    assert(t);
    return t;
}

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
    progress_count = 0;
    var_count = 0;
}

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
    tt_write(t, bulk, sizeof(bulk) - 1);
    assert(clip_count == 1 && clip_len == 5 && !memcmp(clip_data, "hello", 5));

    reset_captures();
    char big[7 + 400 + 1];
    memcpy(big, "\033]52;c;", 7);
    for (int i = 0; i < 400; i += 4)
        memcpy(big + 7 + i, "QUJD", 4);
    big[407] = '\007';
    tt_write(t, big, sizeof(big));
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

    reset_captures();
    void wsap_vtcore_osc52(struct wsap_vtcore *t, const char *params);
    static const char ro_params[] = "c;aGVsbG8=";
    wsap_vtcore_osc52(t, ro_params);
    assert(clip_count == 1 && clip_len == 5 && !memcmp(clip_data, "hello", 5));
    assert(!strcmp(ro_params, "c;aGVsbG8="));

    reset_captures();
    char huge[7 + 6000 + 1];
    memcpy(huge, "\033]52;c;", 7);
    for (int i = 0; i < 6000; i += 4)
        memcpy(huge + 7 + i, "QUJD", 4);
    huge[7 + 6000] = '\007';
    tt_write(t, huge, sizeof(huge));
    assert(clip_count == 1 && clip_len == 4500);
    for (int i = 0; i < 4500; i += 3)
        assert(!memcmp(clip_data + i, "ABC", 3));

    wsap_vtcore_free(t);
}

static void test_sgr_reset_keeps_link(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();

    static const char seq[] = "\033]8;;https://reset.dev\033\\\033[0mX";
    tt_write(t, seq, sizeof(seq) - 1);
    assert(tt_cell(t, 0, 0)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 0, 0, "https://reset.dev");

    static const char seq2[] = "\033]8;;https://second.dev\033\\\033[0m";
    tt_write(t, seq2, sizeof(seq2) - 1);
    tt_write(t, "Y", 1);
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

    write_seq(t, "\033[2;1H\033]8;;https://other.org\033\\bbb\033]8;;\033\\");
    write_seq(t, "\033[3;1H\033]8;;https://example.com\033\\ccc\033]8;;\033\\");
    assert(tt_cell(t, 0, 1)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert(tt_cell(t, 0, 2)->attrs & WSAP_VTCORE_ATTR_LINK);
    assert_link(t, 0, 1, "https://other.org");
    assert_link(t, 2, 1, "https://other.org");
    assert_link(t, 0, 2, "https://example.com");
    assert_link(t, 0, 0, "https://example.com");

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

    for (int i = 0; i < 30; i++)
        tt_write(t, "filler\r\n", 8);
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
        tt_write(t, seq, (size_t)n);
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
    tt_write(t, bulk, strlen(bulk));
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

static void test_osc_colors(void)
{
    struct wsap_vtcore_theme theme;
    wsap_vtcore_theme_default(&theme);
    struct wsap_vtcore *t = osc_test_term();
    struct wsap_vtcore *other = osc_test_term();
    reset_captures();

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
    assert(wsap_vtcore_color_parse("#f00", &c) && c == 0xFFFF0000u);
    assert(wsap_vtcore_color_parse("#00ff00", &c) && c == 0xFF00FF00u);
    assert(wsap_vtcore_color_parse(" #0000ff \t", &c) && c == 0xFF0000FFu);
    assert(wsap_vtcore_color_parse("123456", &c) && c == 0xFF123456u);
    assert(wsap_vtcore_color_parse("abc", &c) && c == 0xFFAABBCCu);
    assert(wsap_vtcore_color_parse("#123456789abc", &c) && c == 0xFF12569au);
    assert(wsap_vtcore_color_parse("#123456789", &c) && c == 0xFF124578u);
    assert(!wsap_vtcore_color_parse("#12g456789", &c));
    assert(!wsap_vtcore_color_parse("#123456789abg", &c));

    assert(wsap_vtcore_color_parse("rgb:ff/80/00", &c) && c == 0xFFFF8000u);
    assert(wsap_vtcore_color_parse("rgb:f/123/4567", &c) && c == 0xFFFF1245u);
    assert(!wsap_vtcore_color_parse("rgb:fffff/0/0", &c));
    assert(wsap_vtcore_color_parse("rgbi:1.0/0.0/0.5", &c) && c == 0xFFFF0080u);
    assert(wsap_vtcore_color_parse("rgbi:.25/.5/.75", &c));

    assert(!wsap_vtcore_color_parse(NULL, &c));
    assert(!wsap_vtcore_color_parse("#ff", &c));
    assert(!wsap_vtcore_color_parse("", &c));
    assert(!wsap_vtcore_color_parse("rgb:ff/80", &c));
    assert(!wsap_vtcore_color_parse("rgb:ff/80/00/11", &c));
    assert(!wsap_vtcore_color_parse("rgbi:2.0/0/0", &c));
    assert(!wsap_vtcore_color_parse("rgbi:./0/0", &c));

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

static void assert_progress(struct wsap_vtcore *t, const char *seq, int count,
                            enum wsap_vtcore_progress state, int percent)
{
    write_seq(t, seq);
    if (progress_count != count || (count && (progress_state != state || progress_percent != percent))) {
        fprintf(stderr, "%s: got %d events, state %d, percent %d\n", seq + 1, progress_count,
                (int)progress_state, progress_percent);
        assert(!"progress mismatch");
    }
}

static void test_osc_progress(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();
    assert_progress(t, "\033]9;4;1;50\007", 1, WSAP_VTCORE_PROGRESS_NORMAL, 50);
    assert_progress(t, "\033]9;4;1\033\\", 2, WSAP_VTCORE_PROGRESS_NORMAL, 0);
    assert_progress(t, "\033]9;4;1;250\007", 3, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    assert_progress(t, "\033]9;4;2\007", 4, WSAP_VTCORE_PROGRESS_ERROR, -1);
    assert_progress(t, "\033]9;4;2;30\007", 5, WSAP_VTCORE_PROGRESS_ERROR, 30);
    assert_progress(t, "\033]9;4;3;50\007", 6, WSAP_VTCORE_PROGRESS_INDETERMINATE, -1);
    assert_progress(t, "\033]9;4;4;70\007", 7, WSAP_VTCORE_PROGRESS_PAUSED, 70);
    assert_progress(t, "\033]9;4;0;70\007", 8, WSAP_VTCORE_PROGRESS_REMOVE, -1);
    assert_progress(t, "\033]9;4\007", 9, WSAP_VTCORE_PROGRESS_REMOVE, -1);
    assert_progress(t, "\033]9;4;;40\007", 10, WSAP_VTCORE_PROGRESS_REMOVE, -1);
    assert_progress(t, "\033]9;4;1;99999999999999999999\007", 11, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    /* Invalid states and values change nothing. */
    assert_progress(t, "\033]9;4;5;10\007", 11, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    assert_progress(t, "\033]9;4;x\007", 11, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    assert_progress(t, "\033]9;4;1;5x\007", 11, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    assert_progress(t, "\033]9;4;-1;5\007", 11, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    assert_progress(t, "\033]9;45\007", 11, WSAP_VTCORE_PROGRESS_NORMAL, 100);
    /* OSC 9;45 is a notification. Progress reports are not. */
    assert(notify_count == 1 && !strcmp(notify_body, "45"));
    wsap_vtcore_free(t);

    /* Without a progress callback, the report does nothing. */
    struct wsap_vtcore *quiet = tt_new_opts(20, 4, &(struct wsap_vtcore_callbacks){ .notify = capture_notify });
    reset_captures();
    write_seq(quiet, "\033]9;4;1;20\007");
    assert(notify_count == 0);
    wsap_vtcore_free(quiet);
}

static void test_osc_shell_cwd(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();
    write_seq(t, "\033]9;9;/srv/a b\007");
    assert(text_is(wsap_vtcore_cwd, t, "/srv/a b"));
    write_seq(t, "\033]9;9;\"C:\\Users\\me\"\033\\");
    assert(text_is(wsap_vtcore_cwd, t, "C:\\Users\\me"));
    write_seq(t, "\033]9;9;\007\033]9;9\007\033]9;9;\"\"\007");
    assert(text_is(wsap_vtcore_cwd, t, "C:\\Users\\me"));
    write_seq(t, "\033]1337;CurrentDir=/home/u/proj\007");
    assert(text_is(wsap_vtcore_cwd, t, "/home/u/proj"));
    write_seq(t, "\033]1337;CurrentDir=\007");
    assert(text_is(wsap_vtcore_cwd, t, "/home/u/proj"));
    /* A path longer than 4096 bytes changes nothing. */
    char seq[4200];
    int n = snprintf(seq, sizeof(seq), "\033]1337;CurrentDir=/");
    memset(seq + n, 'x', 4096);
    memcpy(seq + n + 4096, "\007", 2);
    write_seq(t, seq);
    assert(text_is(wsap_vtcore_cwd, t, "/home/u/proj"));
    seq[n + 4095] = '\007';
    seq[n + 4096] = '\0';
    write_seq(t, seq);
    char *cwd = wsap_vtcore_cwd(t);
    assert(cwd && strlen(cwd) == 4096);
    wsap_vtcore_text_free(cwd);
    assert(notify_count == 0);
    wsap_vtcore_free(t);
}

static void test_osc_user_vars(void)
{
    struct wsap_vtcore *t = osc_test_term();
    reset_captures();
    write_seq(t, "\033]1337;SetUserVar=foo=YmFy\007");
    assert(var_count == 1 && !strcmp(var_name, "foo") && var_len == 3 && !strcmp(var_value, "bar"));
    write_seq(t, "\033]1337;SetUserVar=k=v=YQBi\033\\");
    assert(var_count == 1);
    write_seq(t, "\033]1337;SetUserVar=bin=YQBi\033\\");
    assert(var_count == 2 && var_len == 3 && !memcmp(var_value, "a\0b", 3));
    write_seq(t, "\033]1337;SetUserVar=empty=\007");
    assert(var_count == 3 && !strcmp(var_name, "empty") && var_len == 0);
    /* Invalid forms and other keys do nothing. */
    write_seq(t, "\033]1337;SetUserVar=noval\007");
    write_seq(t, "\033]1337;SetUserVar==YmFy\007");
    write_seq(t, "\033]1337;SetUserVar=foo=Y\007");
    write_seq(t, "\033]1337;SetUserVar=foo=Ym!y\007");
    write_seq(t, "\033]1337;File=name=eA==;inline=1:eA==\007");
    write_seq(t, "\033]1337;SetMark\007\033]1337;ClearScrollback\007\033]1337\007");
    assert(var_count == 3 && notify_count == 0);
    char *cwd = wsap_vtcore_cwd(t);
    assert(!cwd);

    /* A name has at most 255 bytes. A value has at most 4096 bytes. */
    static char seq[8192], name[300];
    memset(name, 'n', 255);
    name[255] = '\0';
    snprintf(seq, sizeof(seq), "\033]1337;SetUserVar=%s=YQ==\007", name);
    write_seq(t, seq);
    assert(var_count == 4 && strlen(var_name) == 255);
    snprintf(seq, sizeof(seq), "\033]1337;SetUserVar=%sn=YQ==\007", name);
    write_seq(t, seq);
    assert(var_count == 4);
    int n = snprintf(seq, sizeof(seq), "\033]1337;SetUserVar=big=");
    for (int i = 0; i < 4096 / 3 * 4; i += 4)
        memcpy(seq + n + i, "YWJj", 4);
    n += 4096 / 3 * 4;
    memcpy(seq + n, "YQ==\007", 6);
    write_seq(t, seq);
    assert(var_count == 5 && var_len == 4096 && var_value[4095] == 'a');
    memcpy(seq + n, "YWI=\007", 6);
    write_seq(t, seq);
    assert(var_count == 5);
    wsap_vtcore_free(t);
}

#define MARK_A "\033]133;A\007"
#define MARK_B "\033]133;B\007"
#define MARK_C "\033]133;C\007"

/* The view row of the nearest prompt, or INT_MIN when there is none. */
static int prompt_from(struct wsap_vtcore *t, int row, int dir)
{
    int found = INT_MIN;
    if (!wsap_vtcore_find_prompt(t, row, dir, &found))
        return INT_MIN;
    assert(found != INT_MIN);
    return found;
}

static struct wsap_vtcore_command command(struct wsap_vtcore *t, int row)
{
    struct wsap_vtcore_command cmd;
    bool ok = wsap_vtcore_command_at(t, row, &cmd);
    assert(ok);
    return cmd;
}

static void assert_output(struct wsap_vtcore *t, int row, int prompt, int first, int last)
{
    struct wsap_vtcore_command cmd = command(t, row);
    if (cmd.prompt != prompt || !cmd.has_output || cmd.output_first != first || cmd.output_last != last) {
        fprintf(stderr, "row %d: prompt %d, output %d (%d..%d), want %d, %d..%d\n", row, cmd.prompt,
                cmd.has_output, cmd.output_first, cmd.output_last, prompt, first, last);
        assert(!"command mismatch");
    }
}

static void assert_status(struct wsap_vtcore *t, int row, bool finished, bool has_status, int status)
{
    struct wsap_vtcore_command cmd = command(t, row);
    assert(cmd.finished == finished);
    assert(cmd.has_exit_status == has_status);
    assert(cmd.exit_status == (has_status ? status : 0));
}

/* The text of view row y without trailing blanks. */
static const char *view_text(struct wsap_vtcore *t, int y)
{
    static char text[256];
    int n = 0;
    for (int x = 0; x < tt_cols(t) && n < 255; x++) {
        uint32_t cp = tt_cell_cp(t, tt_view_cell(t, x, y));
        text[n++] = cp >= 0x20 && cp < 0x7f ? (char)cp : '?';
    }
    while (n && text[n - 1] == ' ')
        n--;
    text[n] = '\0';
    return text;
}

/* The text of a row outside the view: scroll to it and back. */
static const char *row_text(struct wsap_vtcore *t, int row)
{
    int shift = row < 0 ? row : row >= tt_rows(t) ? row - tt_rows(t) + 1 : 0;
    wsap_vtcore_scroll(t, -shift);
    const char *text = view_text(t, row - shift);
    wsap_vtcore_scroll(t, shift);
    return text;
}

static void test_osc_marks(void)
{
    struct wsap_vtcore *t = tt_new(20, 8);
    assert(t);
    write_seq(t, MARK_A "$ " MARK_B "ls\r\n" MARK_C "a\r\nb\r\n\033]133;D;0\007"
                 MARK_A "$ " MARK_B "false\r\n" MARK_C "\033]133;D;1\007"
                 "\033]133;A;aid=7;cl=m\007$ ");
    assert(prompt_from(t, 8, -1) == 4);
    assert(prompt_from(t, 4, -1) == 3);
    assert(prompt_from(t, 3, -1) == 0);
    assert(prompt_from(t, 0, -1) == INT_MIN);
    assert(prompt_from(t, -5, 1) == 0);
    assert(prompt_from(t, 0, 1) == 3);
    assert(prompt_from(t, 3, 1) == 4);
    assert(prompt_from(t, 4, 1) == INT_MIN);
    int found = 99;
    assert(!wsap_vtcore_find_prompt(t, 4, 0, &found) && found == 99);
    assert(!wsap_vtcore_find_prompt(t, 4, -1, NULL));
    assert(!wsap_vtcore_find_prompt(NULL, 4, -1, &found));

    assert_output(t, 0, 0, 1, 2);
    assert_output(t, 2, 0, 1, 2);
    assert_status(t, 1, true, true, 0);
    struct wsap_vtcore_command cmd = command(t, 3);
    assert(cmd.prompt == 3 && !cmd.has_output);
    assert_status(t, 3, true, true, 1);
    cmd = command(t, 7);
    assert(cmd.prompt == 4 && !cmd.has_output && !cmd.finished && !cmd.has_exit_status);
    assert(!wsap_vtcore_command_at(t, -1, &cmd) && cmd.prompt == 0 && !cmd.finished);
    assert(!wsap_vtcore_command_at(t, 8, &cmd));
    assert(!wsap_vtcore_command_at(t, 0, NULL));
    assert(!wsap_vtcore_command_at(NULL, 0, &cmd));

    /* A secondary prompt does not start a command. Other options do not
     * matter. The output of a running command ends at the cursor. */
    write_seq(t, "for x\r\n\033]133;A;k=s\007> done\r\n" MARK_C "1\r\n2");
    assert(prompt_from(t, 8, -1) == 4);
    assert_output(t, 7, 4, 6, 7);
    assert_status(t, 7, false, false, 0);
    /* The screen scrolls. A cursor in the first column ends the output on
     * the row above it. */
    write_seq(t, "\r\n");
    assert_output(t, 7, 3, 5, 6);

    /* The forms of D. */
    static const struct { const char *d; bool has; int status; } forms[] = {
        { "D", false, 0 }, { "D;", false, 0 }, { "D;abc", false, 0 }, { "D;-2", true, -2 },
        { "D;7;aid=1", true, 7 }, { "D;2147483647", true, 2147483647 },
        { "D;2147483648", false, 0 }, { "D;-2147483648", true, -2147483647 - 1 }, { "D;1x", false, 0 },
    };
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        char seq[64];
        snprintf(seq, sizeof(seq), "\033[H\033[2J" MARK_A "$ x\r\n\033]133;%s\007", forms[i].d);
        write_seq(t, seq);
        assert_status(t, 0, true, forms[i].has, forms[i].status);
    }

    /* The alternate screen has no marks. The normal screen keeps its marks. */
    write_seq(t, "\033[?1049h" MARK_A "$ ");
    assert(prompt_from(t, 8, -1) == INT_MIN);
    assert(!wsap_vtcore_command_at(t, 0, &cmd));
    write_seq(t, "\033[?1049l");
    assert(prompt_from(t, 8, -1) == 0);
    assert_status(t, 0, true, false, 0);

    /* A prompt row that becomes empty loses its mark. ED 2 moved the older
     * prompts to history. */
    write_seq(t, "\033[H\033[2K");
    assert(prompt_from(t, 8, -1) < 0);
    wsap_vtcore_free(t);
}

/* Each command prints its prompt, then lines output lines. */
static void run_commands(struct wsap_vtcore *t, int count, int lines)
{
    char seq[128];
    for (int i = 0; i < count; i++) {
        snprintf(seq, sizeof(seq), MARK_A "$ " MARK_B "cmd%d\r\n" MARK_C, i);
        write_seq(t, seq);
        for (int j = 0; j < lines; j++) {
            snprintf(seq, sizeof(seq), "o%d_%d\r\n", i, j);
            write_seq(t, seq);
        }
        snprintf(seq, sizeof(seq), "\033]133;D;%d\007", i);
        write_seq(t, seq);
    }
    write_seq(t, MARK_A "$ ");
}

/* A row keeps a mark through a partial erase. A whole-line erase removes it. */
static void test_osc_marks_reset(void)
{
    struct wsap_vtcore *t = tt_new(20, 6);
    assert(t);

    /* A full reset clears the marks of the screen. */
    write_seq(t, MARK_A);
    assert(prompt_from(t, 6, -1) == 0);
    write_seq(t, "\033c");
    assert(prompt_from(t, 6, -1) == INT_MIN);
    struct wsap_vtcore_command cmd;
    assert(!wsap_vtcore_command_at(t, 0, &cmd));

    /* A full-line erase removes the mark of an empty row. */
    write_seq(t, "\033[H" MARK_A);
    write_seq(t, "\033[2K");
    assert(prompt_from(t, 6, -1) == INT_MIN);

    /* A partial erase keeps the mark. */
    write_seq(t, "\033[H" MARK_A "$ ");
    write_seq(t, "\033[2G\033[0K");
    assert(prompt_from(t, 6, -1) == 0);
    write_seq(t, "\033[2K");
    assert(prompt_from(t, 6, -1) == INT_MIN);

    /* ED 0 and ED 1 remove the mark of an empty row at the cursor. */
    write_seq(t, "\033[H" MARK_A "\033[J");
    assert(prompt_from(t, 6, -1) == INT_MIN);
    write_seq(t, "\033[H" MARK_A "\033[1J");
    assert(prompt_from(t, 6, -1) == INT_MIN);
    /* ED 1 also removes the mark of an empty row above the cursor. */
    write_seq(t, "\033[H" MARK_A "\033[3;1H\033[1J");
    assert(prompt_from(t, 6, -1) == INT_MIN);

    /* ED 2 clears an empty marked row below the content. The content
     * rows go to history. */
    write_seq(t, "\033[H" MARK_A "$ \r\nout\r\n" MARK_A);
    write_seq(t, "\033[2J");
    int found;
    assert(wsap_vtcore_find_prompt(t, 1, -1, &found) && found < 0);
    wsap_vtcore_free(t);
}

/* The alternate screen has no marks. A reset on it clears the normal marks. */
static void test_osc_marks_alternate_reset(void)
{
    struct wsap_vtcore *t = tt_new(20, 6);
    assert(t);
    write_seq(t, MARK_A "$ ");
    assert(prompt_from(t, 6, -1) == 0);
    write_seq(t, "\033[?1049h" MARK_A "$ ");
    assert(prompt_from(t, 6, -1) == INT_MIN);
    write_seq(t, "\033[?1049l");
    assert(prompt_from(t, 6, -1) == 0);
    write_seq(t, "\033[?1049h\033c");
    assert(prompt_from(t, 6, -1) == INT_MIN);
    struct wsap_vtcore_command cmd;
    assert(!wsap_vtcore_command_at(t, 0, &cmd));
    wsap_vtcore_free(t);
}

static void test_osc_marks_history(void)
{
    struct wsap_vtcore *t = tt_new(20, 4);
    assert(t);
    run_commands(t, 3, 5);
    /* Command i starts at row 6i. Rows 15 to 18 are on the screen. */
    assert(tt_history(t) == 15);
    assert(prompt_from(t, 4, -1) == 3);
    assert(prompt_from(t, 3, -1) == -3);
    assert(prompt_from(t, -3, -1) == -9);
    assert(prompt_from(t, -9, -1) == -15);
    assert(prompt_from(t, -15, -1) == INT_MIN);
    assert(prompt_from(t, -100, 1) == -15);
    assert(prompt_from(t, -15, 1) == -9);
    assert(!strcmp(row_text(t, -15), "$ cmd0"));
    assert(!strcmp(row_text(t, -10), "o0_4"));
    assert_output(t, -3, -3, -2, 2);
    assert_output(t, -12, -15, -14, -10);
    assert_status(t, -11, true, true, 0);
    assert_status(t, -1, true, true, 2);
    assert_status(t, 3, false, false, 0);

    /* Rows follow the view. */
    wsap_vtcore_scroll(t, 7);
    assert(prompt_from(t, 0, -1) == -2);
    assert(prompt_from(t, 0, 1) == 4);
    assert_output(t, 0, -2, -1, 3);
    assert_status(t, 0, true, true, 1);
    wsap_vtcore_scroll(t, 2);
    assert(!strcmp(view_text(t, 0), "$ cmd1"));

    /* Select the output of the last command, as a host does. */
    wsap_vtcore_scroll_to(t, 0);
    struct wsap_vtcore_command cmd = command(t, prompt_from(t, 3, -1));
    assert(cmd.output_first < 0 && cmd.output_last >= 0 && cmd.output_last < 4);
    wsap_vtcore_scroll(t, -cmd.output_first);
    wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 0 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_scroll(t, cmd.output_first);
    wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = 19, .y = cmd.output_last }, true);
    char *text = wsap_vtcore_selection_text(t);
    assert(text && !strcmp(text, "o2_0\no2_1\no2_2\no2_3\no2_4"));
    wsap_vtcore_text_free(text);
    wsap_vtcore_select_clear(t);
    wsap_vtcore_scroll_to(t, 0);

    /* ED 3 removes the marks of history. */
    write_seq(t, "\033[3J");
    assert(prompt_from(t, 3, -1) == INT_MIN);
    assert(!wsap_vtcore_command_at(t, 2, &cmd));
    assert(prompt_from(t, -1, 1) == 3);
    wsap_vtcore_free(t);
}

/* A D after an erased prompt must not change the command above it. */
static void test_osc_marks_finished(void)
{
    struct wsap_vtcore *t = tt_new(20, 3);
    assert(t);
    write_seq(t, MARK_A "$ " MARK_B "one\r\n" MARK_C "o1\r\no2\r\n" "\033]133;D;0\007");
    assert(tt_history(t) == 1);
    assert_status(t, -1, true, true, 0);
    write_seq(t, MARK_A "$ \033[2K\033]133;D;7\007");
    assert_status(t, -1, true, true, 0);
    wsap_vtcore_free(t);
}

static void test_osc_marks_resize(void)
{
    struct wsap_vtcore *t = tt_new(20, 6);
    assert(t);
    write_seq(t, MARK_A "$ " MARK_B "echo aaaaaaaaaaaaaaaaaaaaaaaaa\r\n" MARK_C "out1\r\nout2\r\n"
                 "\033]133;D;3\007" MARK_A "$ ");
    assert_output(t, 1, 0, 2, 3);
    assert(prompt_from(t, 6, -1) == 4);

    /* The command line wraps to four rows. One row goes to history. */
    assert(tt_resize(t, 10, 6));
    assert(tt_history(t) == 1);
    assert_output(t, 0, -1, 3, 4);
    assert_status(t, 2, true, true, 3);
    assert(!strcmp(row_text(t, -1), "$ echo aaa"));
    assert(!strcmp(row_text(t, 3), "out1"));
    assert(prompt_from(t, 6, -1) == 5);

    /* The open history line comes back with its mark. */
    assert(tt_resize(t, 20, 6));
    assert(tt_history(t) == 0);
    assert_output(t, 0, 0, 2, 3);
    assert_status(t, 3, true, true, 3);
    assert(prompt_from(t, 6, -1) == 4);

    /* Fewer rows push the prompt row to history. More rows pull it back. */
    assert(tt_resize(t, 20, 3));
    assert(tt_history(t) == 2);
    assert_output(t, 0, -2, 0, 1);
    assert_status(t, -2, true, true, 3);
    assert(tt_resize(t, 20, 8));
    assert(tt_history(t) == 0);
    assert_output(t, 3, 0, 2, 3);
    assert_status(t, 0, true, true, 3);
    assert(prompt_from(t, 0, 1) == 4);

    /* A narrow width wraps the history line of the prompt. */
    assert(tt_resize(t, 20, 3));
    assert(tt_resize(t, 7, 3));
    struct wsap_vtcore_command cmd = command(t, prompt_from(t, 3, -1) - 1);
    assert(!strcmp(row_text(t, cmd.prompt), "$ echo"));
    assert(cmd.has_output && !strcmp(row_text(t, cmd.output_first), "out1"));
    assert(!strcmp(row_text(t, cmd.output_last), "out2"));
    assert(cmd.has_exit_status && cmd.exit_status == 3);
    assert(tt_resize(t, 20, 8));
    assert_output(t, 3, 0, 2, 3);
    wsap_vtcore_free(t);

    /* An empty prompt row at the bottom keeps its mark through a resize. */
    t = tt_new(20, 4);
    assert(t);
    write_seq(t, MARK_A "$ cmd\r\nout\r\n" MARK_A);
    assert(prompt_from(t, 4, -1) == 2);
    assert(tt_resize(t, 10, 4));
    assert(prompt_from(t, 4, -1) == 2);
    wsap_vtcore_free(t);
}

/* A prompt mark and a double-size line on one row stay together through a
 * height change, a width change, and the history. ED clears both. */
static void test_osc_marks_line_size(void)
{
    struct wsap_vtcore *t = tt_new(10, 4);
    assert(t);
    /* DEC Special Graphics maps the four cells. DECDWL halves the row. */
    write_seq(t, "\033(0" MARK_A "lqqk\033(B\033#6");
    assert(wsap_vtcore_snapshot_row_size(tt_snapshot_for(t), 0) == WSAP_VTCORE_LINE_DOUBLE_WIDTH);
    assert(tt_cell_cp(t, tt_cell(t, 0, 0)) == 0x250C && tt_cell_cp(t, tt_cell(t, 3, 0)) == 0x2510);
    assert(prompt_from(t, 4, -1) == 0);

    /* A height change keeps the mark and the line size. */
    assert(tt_resize(t, 10, 5));
    assert(prompt_from(t, 5, -1) == 0);
    assert(wsap_vtcore_snapshot_row_size(tt_snapshot_for(t), 0) == WSAP_VTCORE_LINE_DOUBLE_WIDTH);
    assert(tt_cell_cp(t, tt_cell(t, 0, 0)) == 0x250C);

    /* A width change keeps them too. */
    assert(tt_resize(t, 12, 5));
    assert(prompt_from(t, 5, -1) == 0);
    assert(wsap_vtcore_snapshot_row_size(tt_snapshot_for(t), 0) == WSAP_VTCORE_LINE_DOUBLE_WIDTH);
    assert(tt_cell_cp(t, tt_cell(t, 3, 0)) == 0x2510);

    /* The row goes to history with its mark and only its visible cells. */
    write_seq(t, "\r\n1\r\n2\r\n3\r\n4\r\n5\r\n6");
    assert(tt_history(t) == 2);
    assert(prompt_from(t, 5, -1) == -2);
    wsap_vtcore_scroll(t, 2);
    assert(prompt_from(t, 5, -1) == 0);
    assert(wsap_vtcore_snapshot_row_size(tt_snapshot_for(t), 0) == WSAP_VTCORE_LINE_SINGLE);
    assert(tt_cell_cp(t, tt_view_cell(t, 0, 0)) == 0x250C &&
           tt_cell_cp(t, tt_view_cell(t, 3, 0)) == 0x2510);
    wsap_vtcore_scroll(t, -2);
    wsap_vtcore_free(t);

    /* ED 2 clears a blank double-size row that has a prompt. */
    t = tt_new(10, 4);
    assert(t);
    write_seq(t, MARK_A "\033#6");
    assert(prompt_from(t, 4, -1) == 0);
    assert(wsap_vtcore_snapshot_row_size(tt_snapshot_for(t), 0) == WSAP_VTCORE_LINE_DOUBLE_WIDTH);
    write_seq(t, "\033[2J");
    assert(prompt_from(t, 4, -1) == INT_MIN);
    assert(wsap_vtcore_snapshot_row_size(tt_snapshot_for(t), 0) == WSAP_VTCORE_LINE_SINGLE);
    wsap_vtcore_free(t);
}

/* Compare the prompts and commands of two terminals from the bottom up. */
static int same_marks(struct wsap_vtcore *a, struct wsap_vtcore *b)
{
    int row = tt_rows(a), count = 0;
    for (;;) {
        int ra = prompt_from(a, row, -1), rb = prompt_from(b, row, -1);
        assert(ra == rb);
        if (ra == INT_MIN)
            return count;
        struct wsap_vtcore_command ca = command(a, ra), cb = command(b, ra);
        if (ca.prompt != cb.prompt || ca.has_output != cb.has_output ||
            ca.output_first != cb.output_first || ca.output_last != cb.output_last ||
            ca.finished != cb.finished || ca.has_exit_status != cb.has_exit_status ||
            ca.exit_status != cb.exit_status) {
            fprintf(stderr, "prompt %d: output %d %d..%d vs %d %d..%d, status %d/%d/%d vs %d/%d/%d\n",
                    ra, ca.has_output, ca.output_first, ca.output_last, cb.has_output, cb.output_first,
                    cb.output_last, ca.finished, ca.has_exit_status, ca.exit_status, cb.finished,
                    cb.has_exit_status, cb.exit_status);
            assert(!"marks differ");
        }
        count++;
        row = ra;
    }
}

/* Feed the same shell integration output in one write and byte by byte.
 * Compare the callbacks and the marks, then do it again after a resize. */
static void test_osc_marks_splits(void)
{
    static char out[32768];
    size_t n = 0;
    n += (size_t)snprintf(out + n, sizeof(out) - n, "\r\n\r\n" MARK_A "$ " MARK_B "run\r\n" MARK_C);
    for (int i = 0; i < 12; i++)
        n += (size_t)snprintf(out + n, sizeof(out) - n, "out %02d\r\n", i);
    n += (size_t)snprintf(out + n, sizeof(out) - n,
                          "\033]9;4;1;47\007\033]9;9;/srv/split\007"
                          "\033]1337;CurrentDir=/srv/split/dir\007"
                          "\033]1337;SetUserVar=split=dmFsdWU=\007"
                          "\033]133;D;4\007" MARK_A "$ ");

    struct wsap_vtcore *whole = osc_test_term(), *bytes = osc_test_term();
    assert(whole && bytes);
    reset_captures();
    tt_write(whole, out, n);
    char *cwd = wsap_vtcore_cwd(whole);
    assert(cwd && !strcmp(cwd, "/srv/split/dir"));
    assert(progress_count == 1 && progress_state == WSAP_VTCORE_PROGRESS_NORMAL && progress_percent == 47);
    assert(var_count == 1 && !strcmp(var_name, "split") && var_len == 5 && !strcmp(var_value, "value"));
    int current = prompt_from(whole, tt_rows(whole), -1);
    int finished = prompt_from(whole, current, -1);
    struct wsap_vtcore_command last = command(whole, finished);
    assert(last.has_output && last.has_exit_status && last.exit_status == 4);

    reset_captures();
    write_seq(bytes, out);
    char *cwd_bytes = wsap_vtcore_cwd(bytes);
    assert(cwd_bytes && !strcmp(cwd_bytes, cwd));
    assert(progress_count == 1 && progress_state == WSAP_VTCORE_PROGRESS_NORMAL && progress_percent == 47);
    assert(var_count == 1 && !strcmp(var_name, "split") && var_len == 5 && !strcmp(var_value, "value"));
    assert(same_marks(whole, bytes) == 2);
    wsap_vtcore_text_free(cwd);
    wsap_vtcore_text_free(cwd_bytes);

    assert(tt_resize(whole, 12, 5) && tt_resize(bytes, 12, 5));
    assert(same_marks(whole, bytes) == 2);
    wsap_vtcore_free(whole);
    wsap_vtcore_free(bytes);
}

static void test_osc_marks_streams(void)
{
    /* Line streams take writes of 1 KiB or more. Feed the same output in
     * one write and byte by byte. */
    static char out[65536];
    size_t n = 0;
    for (int k = 0; k < 4; k++) {
        n += (size_t)snprintf(out + n, sizeof(out) - n, MARK_A "$ " MARK_B "run %d\r\n" MARK_C, k);
        for (int j = 0; j < 150; j++)
            n += (size_t)snprintf(out + n, sizeof(out) - n, k & 1 ? "line %03d\n" : "line %03d\r\n", j);
        n += (size_t)snprintf(out + n, sizeof(out) - n, "\033]133;D;%d\007", k);
    }
    n += (size_t)snprintf(out + n, sizeof(out) - n, MARK_A "$ ");
    struct wsap_vtcore *bulk = tt_new(30, 5), *bytes = tt_new(30, 5);
    assert(bulk && bytes);
    write_seq(bulk, "\033[20h");
    write_seq(bytes, "\033[20h");
    tt_write(bulk, out, n);
    write_seq(bytes, out);
    assert(same_marks(bulk, bytes) == 5);
    struct wsap_vtcore_command cmd = command(bulk, prompt_from(bulk, 5, -1) - 1);
    assert(cmd.has_exit_status && cmd.exit_status == 3);
    assert(cmd.output_last - cmd.output_first == 149);
    assert(!strcmp(row_text(bulk, cmd.output_first), "line 000"));
    assert(!strcmp(row_text(bulk, cmd.prompt), "$ run 3"));
    wsap_vtcore_free(bulk);
    wsap_vtcore_free(bytes);
}

static void test_osc_marks_limit(void)
{
    /* History keeps the marks of the newest 65536 rows. */
    struct wsap_vtcore_options opt = { .size = { .x = 10, .y = 3 }, .scrollback = 100000 };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    char seq[64];
    for (int i = 0; i < 70000; i++) {
        int len = snprintf(seq, sizeof(seq), MARK_A "%d\r\n", i);
        tt_write(t, seq, (size_t)len);
    }
    wsap_vtcore_scroll_to(t, INT_MAX);
    assert(!strcmp(view_text(t, 0), "0"));
    int first = prompt_from(t, -1, 1);
    assert(first > 0);
    wsap_vtcore_scroll(t, -first);
    int oldest = atoi(view_text(t, 0));
    assert(oldest > 70000 - 65536 - 3 && oldest < 70000 - 65536 + 3);
    assert(prompt_from(t, 0, 1) == 1 && prompt_from(t, 0, -1) == INT_MIN);
    wsap_vtcore_free(t);
}

int main(void)
{
    test_osc52();
    test_osc8();
    test_sgr_reset_keeps_link();
    test_osc_misc();
    test_host_name_notify();
    test_osc_progress();
    test_osc_shell_cwd();
    test_osc_user_vars();
    test_osc_marks();
    test_osc_marks_reset();
    test_osc_marks_alternate_reset();
    test_osc_marks_history();
    test_osc_marks_finished();
    test_osc_marks_resize();
    test_osc_marks_line_size();
    test_osc_marks_splits();
    test_osc_marks_streams();
    test_osc_marks_limit();
    test_osc_colors();
    test_osc_legacy_titles();
    test_palette_and_theme();
    puts("PASS: OSC tests");
    return 0;
}
