#include "term_api.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char response[16384];
static size_t response_len;
static void respond(void *ud, const char *data, size_t len)
{
    (void)ud;
    if (len >= sizeof(response)) len = sizeof(response) - 1;
    memcpy(response, data, len);
    response[len] = 0;
    response_len = len;
}
static void write_seq(struct wsap_vtcore *t, const char *s)
{
    for (; *s; s++) tt_write(t, s, 1);
}
static const char *row_text(struct wsap_vtcore *t, int r, char *buf)
{
    int n = 0;
    for (int c = 0; c < tt_cols(t); c++)
        buf[n++] = (char)tt_view_cell(t, c, r)->text;
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = 0;
    return buf;
}
static bool row_is(struct wsap_vtcore *t, int r, const char *want)
{
    char got[WSAP_VTCORE_MAX_COLS + 1];
    if (strcmp(row_text(t, r, got), want)) {
        fprintf(stderr, "row %d: got \"%s\" want \"%s\"\n", r, got, want);
        return false;
    }
    return true;
}
static struct wsap_vtcore *new_term(int cols, int rows)
{
    struct wsap_vtcore *t = tt_new_opts(cols, rows, &(struct wsap_vtcore_callbacks){ .reply = respond });
    assert(t);
    response_len = 0;
    response[0] = 0;
    return t;
}
static void expect_response(const char *want)
{
    if (response_len != strlen(want) || memcmp(response, want, response_len)) {
        fprintf(stderr, "response: got len %zu \"%s\" want \"%s\"\n",
                response_len, response, want);
        assert(!"response mismatch");
    }
}
static void expect_no_response(void)
{
    assert(response_len == 0);
}

static void hex_encode(char *out, const char *s)
{
    for (; *s; s++) {
        snprintf(out, 3, "%02x", (unsigned char)*s);
        out += 2;
    }
    *out = 0;
}

static void assert_rqss(struct wsap_vtcore *t, const char *spec, const char *payload)
{
    char req[128], want[256];
    snprintf(req, sizeof(req), "\033P$q%s\033\\", spec);
    snprintf(want, sizeof(want), "\033P1$r%s\033\\", payload);
    response_len = 0;
    write_seq(t, req);
    expect_response(want);
}
static void assert_rqss_invalid(struct wsap_vtcore *t, const char *spec)
{
    char req[128];
    snprintf(req, sizeof(req), "\033P$q%s\033\\", spec);
    response_len = 0;
    write_seq(t, req);
    expect_response("\033P0$r\033\\");
}

static void test_decrqss_sgr(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    assert_rqss(t, "m", "m");
    write_seq(t, "\033[1;31m");
    assert_rqss(t, "m", "0;1;31m");
    wsap_vtcore_free(t);

    t = new_term(80, 24);
    write_seq(t, "\033[4:3m");
    assert_rqss(t, "m", "0;4:3m");
    wsap_vtcore_free(t);

    t = new_term(80, 24);
    write_seq(t, "\033[38;2;10;20;30m");
    assert_rqss(t, "m", "0;38:2:10:20:30m");
    wsap_vtcore_free(t);
}

static void test_decrqss_margins_and_page(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    assert_rqss(t, "r", "1;24r");
    assert_rqss(t, "t", "24t");
    assert_rqss(t, "$|", "80$|");
    assert_rqss(t, "*|", "24*|");
    write_seq(t, "\033[3;10r");
    assert_rqss(t, "r", "3;10r");
    wsap_vtcore_free(t);

    t = new_term(80, 24);
    assert_rqss(t, "s", "1;80s");
    write_seq(t, "\033[?69h\033[5;20s");
    assert_rqss(t, "s", "5;20s");
    wsap_vtcore_free(t);
}

static void test_decrqss_cursor_style(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    assert_rqss(t, " q", "1 q");
    write_seq(t, "\033[5 q");
    assert_rqss(t, " q", "5 q");
    write_seq(t, "\033[4 q");
    assert_rqss(t, " q", "4 q");
    wsap_vtcore_free(t);
}

static void test_decrqss_misc(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    assert_rqss(t, "\"p", "64;1\"p");
    assert_rqss(t, "\"q", "0\"q");
    assert_rqss(t, "$}", "0$}");
    assert_rqss(t, "$~", "0$~");
    assert_rqss(t, "*x", "0*x");
    write_seq(t, "\033[2*x");
    assert_rqss(t, "*x", "2*x");
    wsap_vtcore_free(t);
}

static void test_decrqss_invalid(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    static const char *bad[] = {
        "){", ",|", ",}", ">0f", ">0m", ">0t", "z", "ABC",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert_rqss_invalid(t, bad[i]);

    response_len = 0;
    write_seq(t, "\033P$x\033\\");
    expect_no_response();
    wsap_vtcore_free(t);
}

static void assert_tcap(struct wsap_vtcore *t, const char *name, const char *value)
{
    char hn[128], hv[512], req[256], want[1024];
    hex_encode(hn, name);
    hex_encode(hv, value);
    snprintf(req, sizeof(req), "\033P+q%s\033\\", hn);
    snprintf(want, sizeof(want), "\033P1+r%s=%s\033\\", hn, hv);
    response_len = 0;
    write_seq(t, req);
    expect_response(want);
}

static void test_xtgettcap(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    assert_tcap(t, "kind", "\033[1;2B");
    assert_tcap(t, "TN", "wsap_vtcore");
    assert_tcap(t, "name", "wsap_vtcore");
    assert_tcap(t, "Co", "16777216");
    assert_tcap(t, "colors", "16777216");
    assert_tcap(t, "RGB", "8/8/8");
    assert_tcap(t, "kbs", "\177");
    assert_tcap(t, "kcbt", "\033[Z");
    assert_tcap(t, "kcuu1", "\033OA");
    assert_tcap(t, "kcud1", "\033OB");
    assert_tcap(t, "kcuf1", "\033OC");
    assert_tcap(t, "kcub1", "\033OD");
    assert_tcap(t, "khome", "\033OH");
    assert_tcap(t, "kend", "\033OF");
    assert_tcap(t, "kent", "\033OM");
    assert_tcap(t, "kich1", "\033[2~");
    assert_tcap(t, "kdch1", "\033[3~");
    assert_tcap(t, "kpp", "\033[5~");
    assert_tcap(t, "knp", "\033[6~");
    assert_tcap(t, "kf1", "\033OP");
    assert_tcap(t, "kf2", "\033OQ");
    assert_tcap(t, "kf3", "\033OR");
    assert_tcap(t, "kf4", "\033OS");
    assert_tcap(t, "kf5", "\033[15~");
    assert_tcap(t, "kf6", "\033[17~");
    assert_tcap(t, "kf7", "\033[18~");
    assert_tcap(t, "kf8", "\033[19~");
    assert_tcap(t, "kf9", "\033[20~");
    assert_tcap(t, "kf10", "\033[21~");
    assert_tcap(t, "kf11", "\033[23~");
    assert_tcap(t, "kf12", "\033[24~");
    assert_tcap(t, "kf13", "\033[1;2P");
    assert_tcap(t, "kf24", "\033[24;2~");
    assert_tcap(t, "kUP", "\033[1;2A");
    assert_tcap(t, "kDN", "\033[1;2B");
    assert_tcap(t, "kLFT", "\033[1;2D");
    assert_tcap(t, "kRIT", "\033[1;2C");
    assert_tcap(t, "kmous", "\033[<");
    assert_tcap(t, "kri", "\033[1;2A");
    assert_tcap(t, "kxIN", "\033[I");
    assert_tcap(t, "kxOUT", "\033[O");
    wsap_vtcore_free(t);
}

static void test_xtgettcap_multi(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    char hk[16], htn[16], hb[16], hv[32], req[256], want[1024];
    hex_encode(hk, "kind");
    hex_encode(htn, "TN");
    hex_encode(hb, "BOGUS");
    hex_encode(hv, "\033[1;2B");

    snprintf(req, sizeof(req), "\033P+q%s;%s\033\\", hk, htn);
    snprintf(want, sizeof(want), "\033P1+r%s=%s;%s=%s\033\\",
             hk, hv, htn, "777361705f7674636f7265");
    response_len = 0;
    write_seq(t, req);
    expect_response(want);

    snprintf(req, sizeof(req), "\033P+q%s;%s;%s\033\\", hk, hb, htn);
    snprintf(want, sizeof(want), "\033P1+r%s=%s;%s\033\\", hk, hv, hb);
    response_len = 0;
    write_seq(t, req);
    expect_response(want);
    assert(!strstr(response, htn));

    snprintf(req, sizeof(req), "\033P+q%s\033\\", hb);
    snprintf(want, sizeof(want), "\033P0+r%s\033\\", hb);
    response_len = 0;
    write_seq(t, req);
    expect_response(want);
    wsap_vtcore_free(t);
}

static void test_xtgettcap_malformed(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    static const char *bad[] = {"6b696", "zz", "6g", "6b69 6e"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char req[128];
        snprintf(req, sizeof(req), "\033P+q%s\033\\", bad[i]);
        response_len = 0;
        write_seq(t, req);
        assert(response_len > 4);
        assert(!memcmp(response, "\033P0+r", 5));
        assert(response[response_len - 2] == '\033');
        assert(response[response_len - 1] == '\\');
    }
    wsap_vtcore_free(t);
}

static void test_xtsettcap(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    char hn[32];
    hex_encode(hn, "myterm");
    char req[128];
    snprintf(req, sizeof(req), "\033P+p%s\033\\", hn);
    response_len = 0;
    write_seq(t, req);
    expect_no_response();
    assert_tcap(t, "TN", "myterm");
    wsap_vtcore_free(t);
}

static void test_xtgetxres(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    char hn[64], req[128], want[256];
    hex_encode(hn, "allowTcapOps");
    snprintf(req, sizeof(req), "\033P+Q%s\033\\", hn);
    snprintf(want, sizeof(want), "\033P0+R%s\033\\", hn);
    response_len = 0;
    write_seq(t, req);
    expect_response(want);

    response_len = 0;
    write_seq(t, "\033P+Q6\033\\");
    expect_no_response();
    write_seq(t, "\033P+Q6g\033\\");
    expect_no_response();
    write_seq(t, "\033P+Q\033\\");
    expect_no_response();

    wsap_vtcore_free(t);
}

static void test_decrsps_cir(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    write_seq(t, "\033P1$t3;5;1;I;@;H;00;@;BBBB\033\\");
    assert(tt_cursor_y(t) == 2 && tt_cursor_x(t) == 4);

    write_seq(t, "Z");
    const struct wsap_vtcore_cell *cell = tt_cell(t, 4, 2);
    assert(cell->text == 'Z');
    assert(cell->attrs & WSAP_VTCORE_ATTR_BOLD);
    assert(cell->fg == WSAP_VTCORE_COLOR_BG);
    wsap_vtcore_free(t);

    t = new_term(80, 24);
    write_seq(t, "\033P1$t3;5;1;@;@;H;00;@;BBBB\033\\");
    write_seq(t, "\033[1;80Hab");
    assert(tt_cell(t, 79, 0)->text == 'a');
    assert(tt_cell(t, 0, 1)->text == 'b');
    wsap_vtcore_free(t);

    t = new_term(80, 24);
    write_seq(t, "\033[5;15r");
    write_seq(t, "\033P1$t3;5;1;@;@;I;00;@;BBBB\033\\");
    write_seq(t, "\033[1;1H");
    assert(tt_cursor_y(t) == 4 && tt_cursor_x(t) == 0);
    wsap_vtcore_free(t);
}

static void test_decrsps_tabsr(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    write_seq(t, "\033P2$t4/10\033\\");
    write_seq(t, "\033[1;1H");
    write_seq(t, "\t");
    assert(tt_cursor_x(t) == 3);
    write_seq(t, "\t");
    assert(tt_cursor_x(t) == 9);
    write_seq(t, "\t");
    assert(tt_cursor_x(t) == 79);
    wsap_vtcore_free(t);
}

static void test_synchronized_output(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    write_seq(t, "\033P=1s\033\\");
    assert(wsap_vtcore_modes(t).synchronized);
    write_seq(t, "\033P=2s\033\\");
    assert(!wsap_vtcore_modes(t).synchronized);

    write_seq(t, "\033P=10s\033\\");
    assert(!wsap_vtcore_modes(t).synchronized);
    write_seq(t, "\033P=1;s\033\\");
    assert(wsap_vtcore_modes(t).synchronized);
    write_seq(t, "\033P=xs\033\\");
    assert(wsap_vtcore_modes(t).synchronized);
    write_seq(t, "\033P=2;s\033\\");
    assert(!wsap_vtcore_modes(t).synchronized);
    wsap_vtcore_free(t);
}

static void test_ignored_dcs(void)
{
    static const char *seqs[] = {
        "\033P0;1|17/0d\033\\",
        "\033P0!u%5\033\\",
        "\033P1!uA\033\\",
        "\033P1;0;1;10;0;2;0;1{~\033\\",
        "\033P1;0;0q#0~~\033\\",
        "\033P1pS(E)\033\\",
        "\033P?~junk\033\\",
    };
    for (size_t i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        struct wsap_vtcore *t = new_term(80, 24);
        write_seq(t, "A");
        write_seq(t, seqs[i]);
        write_seq(t, "B");
        assert(row_is(t, 0, "AB"));
        assert(tt_cursor_x(t) == 2 && tt_cursor_y(t) == 0);
        expect_no_response();
        wsap_vtcore_free(t);
    }
}

static void test_xtversion(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    response_len = 0;
    write_seq(t, "\033[>0q");
    expect_response("\033P>|wsap_vtcore\033\\");
    wsap_vtcore_free(t);
}

static void test_host_name(void)
{
    struct wsap_vtcore_options opt = {
        .size = { .x = 80, .y = 24 },
        .scrollback = TEST_SCROLLBACK,
        .name = "myterm",
        .callbacks = { .reply = respond },
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    response_len = 0;
    write_seq(t, "\033[>0q");
    expect_response("\033P>|myterm\033\\");
    assert_tcap(t, "TN", "myterm");
    assert_tcap(t, "name", "myterm");

    response_len = 0;
    write_seq(t, "\033P+p6f74686572\033\\");
    expect_no_response();
    assert_tcap(t, "TN", "other");
    response_len = 0;
    write_seq(t, "\033[>0q");
    expect_response("\033P>|myterm\033\\");
    wsap_vtcore_free(t);

    opt.name = "";
    t = wsap_vtcore_new(&opt);
    assert(t);
    response_len = 0;
    write_seq(t, "\033[>0q");
    expect_response("\033P>|wsap_vtcore\033\\");
    wsap_vtcore_free(t);
    opt.name = NULL;
    t = wsap_vtcore_new(&opt);
    assert(t);
    response_len = 0;
    write_seq(t, "\033[>0q");
    expect_response("\033P>|wsap_vtcore\033\\");
    wsap_vtcore_free(t);
}

static void test_bel_terminator(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    response_len = 0;
    write_seq(t, "\033P+q6b696e64\007");
    expect_response("\033P1+r6b696e64=1b5b313b3242\033\\");

    response_len = 0;
    write_seq(t, "\033P$qm\007");
    expect_response("\033P1$rm\033\\");
    wsap_vtcore_free(t);
}

static void test_dcs_long_query(void)
{
    enum { NAMES = 60 };
    char hk[16], hv[32];
    hex_encode(hk, "kind");
    hex_encode(hv, "\033[1;2B");
    static char req[4096], want[8192];
    int rn = snprintf(req, sizeof(req), "\033P+q");
    int wn = snprintf(want, sizeof(want), "\033P1+r");
    for (int i = 0; i < NAMES; i++) {
        rn += snprintf(req + rn, sizeof(req) - (size_t)rn, "%s%s", i ? ";" : "", hk);
        wn += snprintf(want + wn, sizeof(want) - (size_t)wn, "%s%s=%s",
                       i ? ";" : "", hk, hv);
    }
    snprintf(req + rn, sizeof(req) - (size_t)rn, "\033\\");
    snprintf(want + wn, sizeof(want) - (size_t)wn, "\033\\");
    assert(strlen(req) > 256);

    struct wsap_vtcore *t = new_term(80, 24);
    write_seq(t, req);
    expect_response(want);
    response_len = 0;
    tt_write(t, req, strlen(req));
    expect_response(want);
    write_seq(t, "X");
    assert(row_is(t, 0, "X"));
    wsap_vtcore_free(t);
}

static void test_dcs_abort_c0(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    response_len = 0;
    write_seq(t, "\033P+q6b69\nX");
    expect_no_response();
    assert(row_is(t, 1, "X"));
    wsap_vtcore_free(t);
}

static void test_dcs_abort_esc(void)
{
    struct wsap_vtcore *t = new_term(80, 24);
    response_len = 0;
    write_seq(t, "\033P$qm\033[5n");
    expect_response("\033[0n");
    wsap_vtcore_free(t);
}

int main(void)
{
    test_decrqss_sgr();
    test_decrqss_margins_and_page();
    test_decrqss_cursor_style();
    test_decrqss_misc();
    test_decrqss_invalid();
    test_xtgettcap();
    test_xtgettcap_multi();
    test_xtgettcap_malformed();
    test_xtsettcap();
    test_xtgetxres();
    test_decrsps_cir();
    test_decrsps_tabsr();
    test_synchronized_output();
    test_ignored_dcs();
    test_xtversion();
    test_host_name();
    test_bel_terminator();
    test_dcs_long_query();
    test_dcs_abort_c0();
    test_dcs_abort_esc();
    return 0;
}
