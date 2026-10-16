#include "escape.h"
#include "compat.h"
#include "core.h"
#include "kernels.h"
#include "pen.h"
#include "screen.h"
#include "text.h"
#include "osc/osc.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int sgr_color_param(const struct wsap_vtcore *t, char *buf, size_t sz, int base, uint32_t c)
{
    if (wsap_vtcore_color_is_slot(c) && (c & 0x1FFu) < 256) {
        int i = (int)(c & 0xFFu);
        /* The underline color has no short form. */
        if (i < 16 && base != 50)
            return snprintf(buf, sz, ";%d", i < 8 ? base + i : base + 60 + i - 8);
        return snprintf(buf, sz, ";%d:5:%d", base + 8, i);
    }
    if (wsap_vtcore_color_is_slot(c))
        c = t->palette.colors[c & WSAP_VTCORE_COLOR_SLOT_INDEX];
    return snprintf(buf, sz, ";%d:2:%d:%d:%d", base + 8,
                    (int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff), (int)(c & 0xff));
}

static int sgr_serialize(const struct wsap_vtcore *t, char *buf, size_t sz)
{
    int n = 0;
    if (!(t->bold || t->faint || t->italic || t->underline || t->strike ||
          t->overline || t->inverse || t->cur_fg != WSAP_VTCORE_COLOR_FG ||
          t->cur_bg != WSAP_VTCORE_COLOR_BG || t->cur_ul))
        return 0;
#define SGR_ADD(expr) \
    do { \
        n += (expr); \
        if ((size_t)n >= sz) return sz ? (int)sz - 1 : 0; \
    } while (0)
    SGR_ADD(snprintf(buf + n, sz - (size_t)n, "0"));
    if (t->bold) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";1"));
    if (t->faint) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";2"));
    if (t->italic) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";3"));
    if (t->underline == 1) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";4"));
    else if (t->underline) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";4:%d", t->underline));
    if (t->inverse) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";7"));
    if (t->strike) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";9"));
    if (t->overline) SGR_ADD(snprintf(buf + n, sz - (size_t)n, ";53"));
    if (t->cur_fg != WSAP_VTCORE_COLOR_FG) SGR_ADD(sgr_color_param(t, buf + n, sz - (size_t)n, 30, t->cur_fg));
    if (t->cur_bg != WSAP_VTCORE_COLOR_BG) SGR_ADD(sgr_color_param(t, buf + n, sz - (size_t)n, 40, t->cur_bg));
    if (t->cur_ul) SGR_ADD(sgr_color_param(t, buf + n, sz - (size_t)n, 50, t->cur_ul));
#undef SGR_ADD
    return n;
}

static const signed char hex_val[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static int dcs_decode_hex(const char *s, int len, char *out, int cap)
{
    if (len & 1) return -1;
    int n = 0;
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        int h0 = hex_val[(unsigned char)s[i]], l0 = hex_val[(unsigned char)s[i + 1]];
        int h1 = hex_val[(unsigned char)s[i + 2]], l1 = hex_val[(unsigned char)s[i + 3]];
        if ((h0 | l0 | h1 | l1) < 0 || n + 2 > cap) return -1;
        out[n] = (char)(h0 << 4 | l0);
        out[n + 1] = (char)(h1 << 4 | l1);
        n += 2;
    }
    if (i < len) {
        int hi = hex_val[(unsigned char)s[i]], lo = hex_val[(unsigned char)s[i + 1]];
        if ((hi | lo) < 0 || n >= cap) return -1;
        out[n++] = (char)(hi << 4 | lo);
    }
    return n;
}

struct tcap_entry {
    const char *name, *value;
};

static int tcap_cmp(const void *k, const void *e)
{
    const char *key = k;
    const struct tcap_entry *elem = e;
    return strcmp(key, elem->name);
}

static const char *wsap_vtcore_tcap(const struct wsap_vtcore *t, const char *name)
{
    static const struct tcap_entry caps[] = {
        { "Co",    "16777216" }, { "RGB",   "8/8/8" },
        { "colors", "16777216" }, { "kDC",   "\x1b[3;2~" },
        { "kDC3",  "\x1b[3;3~" }, { "kDC4",  "\x1b[3;4~" },
        { "kDC5",  "\x1b[3;5~" }, { "kDC6",  "\x1b[3;6~" },
        { "kDC7",  "\x1b[3;7~" }, { "kDN",   "\x1b[1;2B" },
        { "kDN3",  "\x1b[1;3B" }, { "kDN4",  "\x1b[1;4B" },
        { "kDN5",  "\x1b[1;5B" }, { "kDN6",  "\x1b[1;6B" },
        { "kDN7",  "\x1b[1;7B" }, { "kEND",  "\x1b[1;2F" },
        { "kEND3", "\x1b[1;3F" }, { "kEND4", "\x1b[1;4F" },
        { "kEND5", "\x1b[1;5F" }, { "kEND6", "\x1b[1;6F" },
        { "kEND7", "\x1b[1;7F" }, { "kHOM",  "\x1b[1;2H" },
        { "kHOM3", "\x1b[1;3H" }, { "kHOM4", "\x1b[1;4H" },
        { "kHOM5", "\x1b[1;5H" }, { "kHOM6", "\x1b[1;6H" },
        { "kHOM7", "\x1b[1;7H" }, { "kIC",   "\x1b[2;2~" },
        { "kIC3",  "\x1b[2;3~" }, { "kIC4",  "\x1b[2;4~" },
        { "kIC5",  "\x1b[2;5~" }, { "kIC6",  "\x1b[2;6~" },
        { "kIC7",  "\x1b[2;7~" }, { "kLFT",  "\x1b[1;2D" },
        { "kLFT3", "\x1b[1;3D" }, { "kLFT4", "\x1b[1;4D" },
        { "kLFT5", "\x1b[1;5D" }, { "kLFT6", "\x1b[1;6D" },
        { "kLFT7", "\x1b[1;7D" }, { "kNXT",  "\x1b[6;2~" },
        { "kNXT3", "\x1b[6;3~" }, { "kNXT4", "\x1b[6;4~" },
        { "kNXT5", "\x1b[6;5~" }, { "kNXT6", "\x1b[6;6~" },
        { "kNXT7", "\x1b[6;7~" }, { "kPRV",  "\x1b[5;2~" },
        { "kPRV3", "\x1b[5;3~" }, { "kPRV4", "\x1b[5;4~" },
        { "kPRV5", "\x1b[5;5~" }, { "kPRV6", "\x1b[5;6~" },
        { "kPRV7", "\x1b[5;7~" }, { "kRIT",  "\x1b[1;2C" },
        { "kRIT3", "\x1b[1;3C" }, { "kRIT4", "\x1b[1;4C" },
        { "kRIT5", "\x1b[1;5C" }, { "kRIT6", "\x1b[1;6C" },
        { "kRIT7", "\x1b[1;7C" }, { "kUP",   "\x1b[1;2A" },
        { "kUP3",  "\x1b[1;3A" }, { "kUP4",  "\x1b[1;4A" },
        { "kUP5",  "\x1b[1;5A" }, { "kUP6",  "\x1b[1;6A" },
        { "kUP7",  "\x1b[1;7A" }, { "kbs",   "\x7f" },
        { "kcbt",  "\x1b[Z" },    { "kcub1", "\x1bOD" },
        { "kcud1", "\x1bOB" },    { "kcuf1", "\x1bOC" },
        { "kcuu1", "\x1bOA" },    { "kdch1", "\x1b[3~" },
        { "kend",  "\x1bOF" },    { "kent",  "\x1bOM" },
        { "kf1",   "\x1bOP" },    { "kf10",  "\x1b[21~" },
        { "kf11",  "\x1b[23~" },  { "kf12",  "\x1b[24~" },
        { "kf13",  "\x1b[1;2P" }, { "kf14",  "\x1b[1;2Q" },
        { "kf15",  "\x1b[1;2R" }, { "kf16",  "\x1b[1;2S" },
        { "kf17",  "\x1b[15;2~" }, { "kf18",  "\x1b[17;2~" },
        { "kf19",  "\x1b[18;2~" }, { "kf2",   "\x1bOQ" },
        { "kf20",  "\x1b[19;2~" }, { "kf21",  "\x1b[20;2~" },
        { "kf22",  "\x1b[21;2~" }, { "kf23",  "\x1b[23;2~" },
        { "kf24",  "\x1b[24;2~" }, { "kf25",  "\x1b[1;5P" },
        { "kf26",  "\x1b[1;5Q" }, { "kf27",  "\x1b[1;5R" },
        { "kf28",  "\x1b[1;5S" }, { "kf29",  "\x1b[15;5~" },
        { "kf3",   "\x1bOR" },    { "kf30",  "\x1b[17;5~" },
        { "kf31",  "\x1b[18;5~" }, { "kf32",  "\x1b[19;5~" },
        { "kf33",  "\x1b[20;5~" }, { "kf34",  "\x1b[21;5~" },
        { "kf35",  "\x1b[23;5~" }, { "kf36",  "\x1b[24;5~" },
        { "kf37",  "\x1b[1;6P" }, { "kf38",  "\x1b[1;6Q" },
        { "kf39",  "\x1b[1;6R" }, { "kf4",   "\x1bOS" },
        { "kf40",  "\x1b[1;6S" }, { "kf41",  "\x1b[15;6~" },
        { "kf42",  "\x1b[17;6~" }, { "kf43",  "\x1b[18;6~" },
        { "kf44",  "\x1b[19;6~" }, { "kf45",  "\x1b[20;6~" },
        { "kf46",  "\x1b[21;6~" }, { "kf47",  "\x1b[23;6~" },
        { "kf48",  "\x1b[24;6~" }, { "kf5",   "\x1b[15~" },
        { "kf6",   "\x1b[17~" },  { "kf7",   "\x1b[18~" },
        { "kf8",   "\x1b[19~" },  { "kf9",   "\x1b[20~" },
        { "khome", "\x1bOH" },    { "kich1", "\x1b[2~" },
        { "kind",  "\x1b[1;2B" }, { "kmous", "\x1b[<" },
        { "knp",   "\x1b[6~" },   { "kpp",   "\x1b[5~" },
        { "kri",   "\x1b[1;2A" }, { "kxIN",  "\x1b[I" },
        { "kxOUT", "\x1b[O" },
    };
    if (!strcmp(name, "TN") || !strcmp(name, "name"))
        return t->tcap_name;
    const struct tcap_entry *found = bsearch(name, caps,
                                             sizeof(caps) / sizeof(caps[0]),
                                             sizeof(caps[0]), tcap_cmp);
    return found ? found->value : NULL;
}

struct resp_buf {
    char *s;
    size_t len, cap;
    bool failed;
};

static void resp_put(struct resp_buf *r, const char *s, size_t n)
{
    if (r->failed)
        return;
    if (r->len + n + 1 > r->cap) {
        size_t cap = wsap_vtcore_grow_cap(r->cap ? r->cap : 256, r->len + n + 1);
        char *p = realloc(r->s, cap);
        if (!p) {
            r->failed = true;
            return;
        }
        r->s = p;
        r->cap = cap;
    }
    memcpy(r->s + r->len, s, n);
    r->len += n;
    r->s[r->len] = '\0';
}

static void dcs_xtgettcap(struct wsap_vtcore *t, const char *buf, int len)
{
    static const char hex[] = "0123456789abcdef";
    struct resp_buf r = {0};
    resp_put(&r, "\033P0+r", 5);
    char *name = malloc((size_t)len / 2 + 1);
    if (!name) {
        free(r.s);
        return;
    }
    int i = 0;
    bool first = true;
    while (i < len) {
        int span = 0;
        while (i + span < len && buf[i + span] != ';') span++;
        int n = dcs_decode_hex(buf + i, span, name, len / 2);
        if (n < 0)
            break;
        name[n] = 0;
        const char *cap = wsap_vtcore_tcap(t, name);
        if (first && r.s) { r.s[2] = cap ? '1' : '0'; first = false; }
        resp_put(&r, buf + i, (size_t)span);
        if (!cap) break;
        resp_put(&r, "=", 1);
        for (const char *c = cap; *c; c++) {
            unsigned char b = (unsigned char)*c;
            char pair[2] = { hex[b >> 4], hex[b & 15] };
            resp_put(&r, pair, 2);
        }
        i += span;
        if (i < len) resp_put(&r, &buf[i++], 1);
    }
    free(name);
    resp_put(&r, "\033\\", 2);
    if (!r.failed)
        send_response(t, r.s);
    free(r.s);
}

static int dcs_param_int(const char **p, const char *end)
{
    int v = 0, any = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        v = v < 214748364 ? v * 10 + (**p - '0') : INT_MAX;
        (*p)++;
        any = 1;
    }
    if (*p < end && **p == ';') (*p)++;
    return any ? v : -1;
}

static int dcs_param_chr(const char **p, const char *end)
{
    if (*p >= end) return -1;
    int v = (unsigned char)*(*p)++;
    if (*p < end && **p == ';') (*p)++;
    return v;
}

static void restore_deccir(struct wsap_vtcore *t, const char *p, const char *end)
{
    int row = dcs_param_int(&p, end);
    if (row <= 0 || row > t->rows) return;
    int col = dcs_param_int(&p, end);
    if (col <= 0 || col > t->cols) return;
    if (dcs_param_int(&p, end) != 1) return;
    sc_move(t, col - 1, row - 1);
    int rend = dcs_param_chr(&p, end);
    if ((rend & 0xf0) != 0x40) return;
    t->bold = rend & 1;
    t->underline = (rend & 2) ? 1 : 0;
    t->inverse = (rend & 8) != 0;
    int attr = dcs_param_chr(&p, end);
    if ((attr & 0xf0) != 0x40) return;
    int flag = dcs_param_chr(&p, end);
    if ((flag & 0xf0) != 0x40) return;
    t->autowrap = (flag & 8) != 0;
    t->origin_mode = (flag & 1) != 0;
    int curss = (flag & 4) ? 3 : (flag & 2) ? 2 : 0;
    int gl = dcs_param_chr(&p, end) - '0';
    if (gl < 0 || gl > 3) return;
    int gr = dcs_param_chr(&p, end) - '0';
    if (gr < 0 || gr > 3) return;
    t->charset_active = gl < 2 ? gl : (curss ? 1 : 0);
    if (dcs_param_chr(&p, end) < 0) return;
    for (int i = 0; i < 4 && p < end; i++) {
        char d = *p++;
        if ((d == '%' || d == '&' || d == '"') && p < end) d = *p++;
        if (i < 2) t->charsets[i] = d;
    }
    t->graphics = t->charsets[t->charset_active] == '0';
}

static void restore_dectabsr(struct wsap_vtcore *t, const char *p, const char *end)
{
    wsap_vtcore_tabs_clear(t);
    t->custom_tabs = true;
    int stop = 0, any = 0;
    for (; p < end; p++) {
        if (*p >= '0' && *p <= '9') {
            stop = stop < 214748364 ? stop * 10 + (*p - '0') : INT_MAX;
            any = 1;
        } else if (*p == '/') {
            if (any && --stop >= 0 && stop < t->tabs_cap) t->tabs[stop] = 1;
            stop = any = 0;
        } else {
            return;
        }
    }
    if (any && --stop >= 0 && stop < t->tabs_cap) t->tabs[stop] = 1;
}

/* Grows the DCS string to hold extra more bytes, up to WSAP_VTCORE_OSC_MAX.
 * False when nothing more fits. */
static bool dcs_reserve(struct wsap_vtcore *t, size_t extra)
{
    size_t need = (size_t)t->dcs_len + extra;
    if (need <= t->dcs_cap)
        return true;
    if (t->dcs_cap >= WSAP_VTCORE_OSC_MAX)
        return false;
    size_t cap = wsap_vtcore_grow_cap(t->dcs_cap ? t->dcs_cap : 256, need);
    if (cap > WSAP_VTCORE_OSC_MAX)
        cap = WSAP_VTCORE_OSC_MAX;
    char *p = realloc(t->dcs_buf, cap);
    if (!p)
        return false;
    t->dcs_buf = p;
    t->dcs_cap = cap;
    return need <= cap;
}

static void dcs_dispatch(struct wsap_vtcore *t)
{
    if (wsap_vtcore_str_was_truncated(t))
        return;
    const char *buf = t->dcs_buf;
    int len = t->dcs_len;
    char resp[512];
    if (len < 2) return;
    if (buf[0] == '+' && buf[1] == 'q') {
        dcs_xtgettcap(t, buf + 2, len - 2);
    } else if (buf[0] == '+' && buf[1] == 'p') {
        int n = dcs_decode_hex(buf + 2, len - 2, t->tcap_name, sizeof(t->tcap_name) - 1);
        if (n >= 0) t->tcap_name[n] = 0;
    } else if (buf[0] == '+' && buf[1] == 'Q') {
        int span = 0;
        while (span < len - 2 && buf[2 + span] != ';') span++;
        char *name = malloc((size_t)span / 2 + 1);
        if (name && span && dcs_decode_hex(buf + 2, span, name, span / 2) >= 0) {
            struct resp_buf r = {0};
            resp_put(&r, "\033P0+R", 5);
            resp_put(&r, buf + 2, (size_t)span);
            resp_put(&r, "\033\\", 2);
            if (!r.failed)
                send_response(t, r.s);
            free(r.s);
        }
        free(name);
    } else if (buf[0] == '$' && buf[1] == 'q') {
        const char *spec = buf + 2;
        int slen = len - 2, p;
        if (slen == 2 && spec[0] == ' ' && spec[1] == 'q') {
            int shape = t->cursor_style == WSAP_VTCORE_CURSOR_BLOCK ? (t->cursor_blink ? 1 : 2)
                  : t->cursor_style == WSAP_VTCORE_CURSOR_UNDERLINE ? (t->cursor_blink ? 3 : 4)
                  : (t->cursor_blink ? 5 : 6);
            p = snprintf(resp, sizeof(resp), "\033P1$r%d q\033\\", shape);
        } else if (slen == 1 && spec[0] == 'm') {
            char sgr[256] = "";
            sgr_serialize(t, sgr, sizeof(sgr));
            p = snprintf(resp, sizeof(resp), "\033P1$r%sm\033\\", sgr);
        } else if (slen == 1 && spec[0] == 'r') {
            p = snprintf(resp, sizeof(resp), "\033P1$r%d;%dr\033\\",
                         t->scroll_top + 1, t->scroll_bottom + 1);
        } else if (slen == 1 && spec[0] == 's') {
            p = snprintf(resp, sizeof(resp), "\033P1$r%d;%ds\033\\",
                         t->left_margin + 1, t->right_margin + 1);
        } else if (slen == 1 && spec[0] == 't') {
            p = snprintf(resp, sizeof(resp), "\033P1$r%dt\033\\", t->rows);
        } else if (slen == 2 && spec[0] == '"' && spec[1] == 'p') {
            p = snprintf(resp, sizeof(resp), "\033P1$r64;1\"p\033\\");
        } else if (slen == 2 && spec[0] == '"' && spec[1] == 'q') {
            p = snprintf(resp, sizeof(resp), "\033P1$r0\"q\033\\");
        } else if (slen == 2 && spec[0] == '$' && spec[1] == '|') {
            p = snprintf(resp, sizeof(resp), "\033P1$r%d$|\033\\", t->cols);
        } else if (slen == 2 && spec[0] == '$' && spec[1] == '}') {
            p = snprintf(resp, sizeof(resp), "\033P1$r0$}\033\\");
        } else if (slen == 2 && spec[0] == '$' && spec[1] == '~') {
            p = snprintf(resp, sizeof(resp), "\033P1$r0$~\033\\");
        } else if (slen == 2 && spec[0] == '*' && spec[1] == 'x') {
            p = snprintf(resp, sizeof(resp), "\033P1$r%d*x\033\\", t->decsace);
        } else if (slen == 2 && spec[0] == '*' && spec[1] == '|') {
            p = snprintf(resp, sizeof(resp), "\033P1$r%d*|\033\\", t->rows);
        } else {
            p = snprintf(resp, sizeof(resp), "\033P0$r\033\\");
        }
        (void)p;
        send_response(t, resp);
    } else if (buf[0] == '=' && len >= 3 && buf[len - 1] == 's') {
        const char *p = buf + 1, *end = buf + len;
        int mode = dcs_param_int(&p, end);
        if (mode == 1) t->synchronized_output = true;
        else if (mode == 2) t->synchronized_output = false;
    } else if (buf[0] >= '0' && buf[0] <= '9') {
        const char *p = buf, *end = buf + len;
        int ps = dcs_param_int(&p, end);
        while (p < end && *p == ';') { p++; dcs_param_int(&p, end); }
        if (end - p >= 2 && p[0] == '$' && p[1] == 't') {
            if (ps == 1) restore_deccir(t, p + 2, end);
            else if (ps == 2) restore_dectabsr(t, p + 2, end);
        }
    }
}

static void soft_reset(struct wsap_vtcore *t)
{
    t->application_cursor = t->application_keypad = t->backarrow_sends_bs = false;
    t->insert_mode = t->origin_mode = t->reverse_video = t->lnm = false;
    t->focus_events = t->synchronized_output = t->lr_margin_mode = false;
    t->mouse_mode = t->mouse_encoding = 0;
    t->bracketed_paste = false;
    t->autowrap = t->cursor_visible = true;
    t->cursor_blink = t->default_cursor_blink;
    t->cursor_style = t->default_cursor_style;
    t->scroll_top = 0; t->scroll_bottom = t->rows - 1;
    t->left_margin = 0; t->right_margin = t->cols - 1;
    t->decsace = 0;
    t->cur_link = 0;
    memset(t->keyboard, 0, sizeof(t->keyboard));
    reset_sgr(t);
}

static inline void sgr_set_slot(struct wsap_vtcore *t, int code, uint32_t c)
{
    if (code == 38) t->cur_fg = c;
    else if (code == 48) t->cur_bg = c;
    else t->cur_ul = c;
}

static inline void handle_sgr(struct wsap_vtcore *t, const struct csi *csi)
{
    if (csi->nparams == 0) {
        reset_sgr(t);
        return;
    }

    if (csi->nparams == 1 && !csi->subparams) {
        int p = csi->params[0];
        if (p == 0) { reset_sgr(t); return; }
        if ((unsigned)(p - 30) < 8) { t->cur_fg = wsap_vtcore_color_index(p - 30); return; }
        if ((unsigned)(p - 40) < 8) { t->cur_bg = wsap_vtcore_color_index(p - 40); return; }
        if ((unsigned)(p - 90) < 8) { t->cur_fg = wsap_vtcore_color_index(p - 90 + 8); return; }
        if ((unsigned)(p - 100) < 8) { t->cur_bg = wsap_vtcore_color_index(p - 100 + 8); return; }
        if (p == 1) { t->bold = true; return; }
        if (p == 39) { t->cur_fg = WSAP_VTCORE_COLOR_FG; return; }
        if (p == 49) { t->cur_bg = WSAP_VTCORE_COLOR_BG; return; }
    }

    if (!csi->subparams) {
        if (csi->nparams == 5 && csi->params[1] == 2 &&
            (csi->params[0] == 38 || csi->params[0] == 48)) {
            sgr_set_slot(t, csi->params[0],
                         wsap_vtcore_rgb(csi->params[2], csi->params[3], csi->params[4]));
            return;
        }

        if (csi->nparams == 10 && csi->params[1] == 2 && csi->params[6] == 2 &&
            (csi->params[0] == 38 || csi->params[0] == 48) &&
            csi->params[5] == (csi->params[0] == 38 ? 48 : 38)) {
            sgr_set_slot(t, csi->params[0],
                         wsap_vtcore_rgb(csi->params[2], csi->params[3], csi->params[4]));
            sgr_set_slot(t, csi->params[5],
                         wsap_vtcore_rgb(csi->params[7], csi->params[8], csi->params[9]));
            return;
        }

        if (csi->nparams == 3 && csi->params[1] == 5 &&
            (csi->params[0] == 38 || csi->params[0] == 48) &&
            (unsigned)csi->params[2] < 256) {
            sgr_set_slot(t, csi->params[0], wsap_vtcore_color_index(csi->params[2]));
            return;
        }

        if (csi->nparams == 6 && csi->params[1] == 5 && csi->params[4] == 5 &&
            (csi->params[0] == 38 || csi->params[0] == 48) &&
            csi->params[3] == (csi->params[0] == 38 ? 48 : 38) &&
            (unsigned)csi->params[2] < 256 && (unsigned)csi->params[5] < 256) {
            sgr_set_slot(t, csi->params[0], wsap_vtcore_color_index(csi->params[2]));
            sgr_set_slot(t, csi->params[3], wsap_vtcore_color_index(csi->params[5]));
            return;
        }
    }

    struct wsap_vtcore_pen pen = {
        .fg = t->cur_fg, .bg = t->cur_bg, .ul = t->cur_ul,
        .underline = t->underline,
        .bold = t->bold, .faint = t->faint, .italic = t->italic,
        .strike = t->strike, .overline = t->overline, .inverse = t->inverse,
    };
    sgr_apply(&pen, csi->params, csi->nparams, csi->subparams);
    t->cur_fg = pen.fg;
    t->cur_bg = pen.bg;
    t->cur_ul = pen.ul;
    t->underline = pen.underline;
    t->bold = pen.bold;
    t->faint = pen.faint;
    t->italic = pen.italic;
    t->strike = pen.strike;
    t->overline = pen.overline;
    t->inverse = pen.inverse;
}

#define WSAP_VTCORE_PRIV_MODE_FIELDS(X) \
    X(1,    application_cursor) \
    X(12,   cursor_blink) \
    X(25,   cursor_visible) \
    X(67,   backarrow_sends_bs) \
    X(1004, focus_events) \
    X(2004, bracketed_paste) \
    X(2026, synchronized_output)

#define WSAP_VTCORE_ANSI_MODE_FIELDS(X) \
    X(4,  insert_mode) \
    X(20, lnm)

static int mode_get(const struct wsap_vtcore *t, int mode, bool priv)
{
    if (priv) {
        switch (mode) {
#define MODE_FIELD_GET(n, f) case n: return t->f;
        WSAP_VTCORE_PRIV_MODE_FIELDS(MODE_FIELD_GET)
#undef MODE_FIELD_GET
        case 5: return t->reverse_video;
        case 6: return t->origin_mode;
        case 7: return t->autowrap;
        case 47: case 1047: case 1049: return t->alternate;
        case 69: return t->lr_margin_mode;
        default: break;
        }
        if (mode_is_mouse(mode)) return t->mouse_mode == mode;
        if (mode_is_mouse_enc(mode)) return t->mouse_encoding == mode;
        return -1;
    }
    switch (mode) {
#define MODE_FIELD_GET(n, f) case n: return t->f;
    WSAP_VTCORE_ANSI_MODE_FIELDS(MODE_FIELD_GET)
#undef MODE_FIELD_GET
    }
    return -1;
}

static int cursor_top(const struct wsap_vtcore *t) { return t->origin_mode ? t->scroll_top : 0; }
static int cursor_bottom(const struct wsap_vtcore *t) { return t->origin_mode ? t->scroll_bottom : t->rows - 1; }
static int cursor_left(const struct wsap_vtcore *t) { return t->origin_mode && t->lr_margin_mode ? t->left_margin : 0; }
static int cursor_right(const struct wsap_vtcore *t) { return t->origin_mode && t->lr_margin_mode ? t->right_margin : t->cols - 1; }

static void mode_set(struct wsap_vtcore *t, int mode, bool enabled, bool priv)
{
    if (!priv) {
        switch (mode) {
#define MODE_FIELD_SET(n, f) case n: t->f = enabled; break;
        WSAP_VTCORE_ANSI_MODE_FIELDS(MODE_FIELD_SET)
#undef MODE_FIELD_SET
        }
        return;
    }
    switch (mode) {
#define MODE_FIELD_SET(n, f) case n: t->f = enabled; return;
    WSAP_VTCORE_PRIV_MODE_FIELDS(MODE_FIELD_SET)
#undef MODE_FIELD_SET
    default: break;
    }
    if (mode == 5) {
        if (t->reverse_video != enabled) {
            t->reverse_video = enabled;
            t->full_gen++;
        }
    } else if (mode == 6) {
        t->origin_mode = enabled;
        sc_move(t, 0, enabled ? t->scroll_top : 0);
    } else if (mode == 7) {
        t->autowrap = enabled;
        t->pending_wrap = false;
    } else if (mode == 1048) {
        if (enabled) t->saved = wsap_vtcore_cursor_save(t);
        else sc_restore_cursor(t, &t->saved);
    } else if (mode == 47 || mode == 1047 || mode == 1049) {
        wsap_vtcore_screen_set_alternate(t, enabled);
    } else if (mode_is_mouse(mode)) {
        t->mouse_mode = enabled ? mode : 0;
    } else if (mode == 69) {
        t->lr_margin_mode = enabled;
        t->left_margin = 0; t->right_margin = t->cols - 1;
        sc_move(t, 0, cursor_top(t));
    } else if (mode_is_mouse_enc(mode)) {
        t->mouse_encoding = enabled ? mode : 0;
    }
}

/* Vertical moves stop at a margin when they start inside the scroll
 * region (xterm, DEC STD 070). */
static inline void move_v(struct wsap_vtcore *t, int n, bool col0)
{
    int top = t->cursor_y >= t->scroll_top ? t->scroll_top : 0;
    int bottom = t->cursor_y <= t->scroll_bottom ? t->scroll_bottom : t->rows - 1;
    int y = n < 0 ? (t->cursor_y - top >= -n ? t->cursor_y + n : top)
                 : (n < bottom - t->cursor_y ? t->cursor_y + n : bottom);
    sc_move(t, col0 ? cursor_left(t) : t->cursor_x, y);
}
static inline void move_h(struct wsap_vtcore *t, int n, bool margin_aware)
{
    int right = margin_aware ? cursor_right(t) : t->cols - 1;
    int x = n < 0 ? (t->cursor_x >= -n ? t->cursor_x + n : 0)
                 : (n < right - t->cursor_x ? t->cursor_x + n : right);
    sc_move(t, x, t->cursor_y);
}

static inline WSAP_VTCORE_ALWAYS_INLINE void dispatch_csi(struct wsap_vtcore *t, const struct csi *csi)
{
    if (WSAP_VTCORE_EXPECT(csi->final == 'm', 1)) {
        if (WSAP_VTCORE_EXPECT(!csi->prefix && !csi->intermediate, 1))
            handle_sgr(t, csi);
        return;
    }
    if (WSAP_VTCORE_EXPECT(csi->final == 'H' || csi->final == 'f', 0)) {
        if (WSAP_VTCORE_EXPECT(!csi->prefix && !csi->intermediate && !csi->subparams, 1)) {
            int64_t r = (int64_t)(csi->params[0] > 0 ? csi->params[0] - 1 : 0) + cursor_top(t);
            if (r > cursor_bottom(t)) r = cursor_bottom(t);
            int64_t c = (int64_t)(csi->nparams > 1 && csi->params[1] > 0 ? csi->params[1] - 1 : 0) + cursor_left(t);
            if (c > cursor_right(t)) c = cursor_right(t);
            sc_move(t, (int)c, (int)r);
        }
        return;
    }

    if (csi->intermediate) {
        bool plain = (csi->final == 'q' && csi->intermediate == ' ') ||
                     (csi->final == 'p' && csi->intermediate == '!') ||
                     (csi->final == 'x' && csi->intermediate == '*');
        if (plain) {
            if (csi->prefix || csi->subparams) return;
        } else if (csi->final == 'p' && csi->intermediate == '$') {
            if (csi->subparams) return;
        } else if (csi->final == 'r' && csi->intermediate == '$') {
            if (csi->prefix) return;
        } else {
            return;
        }
    }
    if (csi->prefix) {
        if (csi->final != 'u' &&
            !(csi->final == 'c' && (csi->prefix == '>' || csi->prefix == '=')) &&
            !(csi->final == 'q' && csi->prefix == '>' && !csi->intermediate) &&
            !(csi->final == 'p' && csi->intermediate == '$' && csi->prefix == '?') &&
            !(csi->final == 'W' && csi->prefix == '?') &&
            !((csi->final == 'h' || csi->final == 'l') && csi->prefix == '?')) return;
    }
    if (csi->subparams && !(csi->intermediate == '$' && csi->final == 'r')) return;
    switch (csi->final) {
    case 'r': {
        if (csi->intermediate == '$') {
            wsap_vtcore_sc_deccara(t, csi);
            break;
        }
        if (csi->prefix || csi->intermediate) break;
        int top = csi_count(csi) - 1;
        int bottom = csi->nparams > 1 && csi->params[1] ? csi->params[1] - 1 : t->rows - 1;
        if (top < bottom && bottom < t->rows) {
            t->scroll_top = top;
            t->scroll_bottom = bottom;
            sc_move(t, 0, t->origin_mode ? top : 0);
        }
        break;
    }
    case 'L':
    case 'M':
        if (csi->prefix || t->cursor_y < t->scroll_top || t->cursor_y > t->scroll_bottom) break;
        wsap_vtcore_screen_scroll_region(t, t->cursor_y, t->scroll_bottom,
                                  csi_count(csi), csi->final == 'L', false);
        /* IL and DL move the cursor to the left margin (xterm). */
        sc_move(t, t->lr_margin_mode ? t->left_margin : 0, t->cursor_y);
        break;
    case 'S':
    case 'T':
        if (csi->prefix || csi->nparams > 1) break;
        wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom,
                                  csi_count(csi), csi->final == 'T', true);
        break;
    case 'd': {
        int64_t r = (int64_t)csi_count(csi) - 1 + cursor_top(t);
        if (r > cursor_bottom(t)) r = cursor_bottom(t);
        sc_move(t, t->cursor_x, (int)r);
        break;
    }
    case 'b':
        wsap_vtcore_text_repeat(t, csi_count(csi));
        break;
    case 'A': move_v(t, -csi_count(csi), false); break;
    case 'B': move_v(t,  csi_count(csi), false); break;
    case 'E': move_v(t,  csi_count(csi), true);  break;
    case 'F': move_v(t, -csi_count(csi), true);  break;
    case 'e': move_v(t,  csi_count(csi), false); break;
    case 'a': move_h(t,  csi_count(csi), true);  break;
    case 'C': move_h(t,  csi_count(csi), false); break;
    case 'D': move_h(t, -csi_count(csi), false); break;
    case 'G':
    case '`': {
        int64_t x = (int64_t)csi_count(csi) - 1 + cursor_left(t);
        sc_move(t, (int)(x > cursor_right(t) ? cursor_right(t) : x), t->cursor_y);
        break;
    }
    case 's':
        if (!csi->prefix && t->lr_margin_mode && csi->nparams > 1) {
            int left = csi_count(csi) - 1;
            int right = csi->params[1] > 0 ? csi->params[1] - 1 : t->cols - 1;
            if (left < right && right < t->cols) {
                t->left_margin = left; t->right_margin = right;
                sc_move(t, t->origin_mode ? left : 0, cursor_top(t));
            }
        } else if (!csi->prefix) {
            t->saved = wsap_vtcore_cursor_save(t);
        }
        break;
    case 'I':
    case 'Z':
        wsap_vtcore_sc_tab(t, csi->final == 'I' ? csi_count(csi) : -csi_count(csi));
        break;
    case 'g':
        if (csi->params[0] == 0) {
            t->tabs[t->cursor_x] = 0;
            t->custom_tabs = true;
        } else if (csi->params[0] == 3) {
            wsap_vtcore_tabs_clear(t);
        }
        break;
    case 'W':
        if (csi->prefix == '?' && csi->params[0] == 5) {
            wsap_vtcore_tabs_clear(t);
            wsap_vtcore_tabs_default(t);
        }
        break;
    case 'n':
        if (!csi->prefix && csi->params[0] == 5) send_response(t, "\033[0n");
        else if (!csi->prefix && csi->params[0] == 6) {
            /* Report the cursor cell, also with a pending wrap (xterm). */
            char reply[48];
            snprintf(reply, sizeof(reply), "\033[%d;%dR", t->cursor_y - cursor_top(t) + 1,
                     t->cursor_x - cursor_left(t) + 1);
            send_response(t, reply);
        }
        break;
    case 'J':
        wsap_vtcore_screen_erase(t, csi->params[0]);
        break;
    case 'X': {
        int n = csi_count(csi);
        sc_erase_span(t, t->cursor_y, t->cursor_x,
                      n > t->cols - t->cursor_x ? t->cols : t->cursor_x + n);
        break;
    }
    case 'x':
        if (csi->intermediate == '*')
            t->decsace = (uint8_t)csi->params[0];
        break;
    case 'K':
        wsap_vtcore_screen_erase_line(t, csi->params[0]);
        break;
    case 'P':
        wsap_vtcore_screen_delete_chars(t, csi_count(csi));
        break;
    case '@':
        wsap_vtcore_screen_insert_chars(t, csi_count(csi));
        break;
    case 'c': {
        if (csi->prefix == '>')
            send_response(t, "\x1b[>0;10;0c");
        else if (!csi->prefix && csi->params[0] == 0)
            send_response(t, "\x1b[?62;22c");
        break;
    }
    case 't':
        if (!csi->prefix && csi->params[0] == 14) {
            char reply[32];
            snprintf(reply, sizeof(reply), "\033[4;%d;%dt",
                     t->rows * 20, t->cols * 10);
            send_response(t, reply);
        }
        break;
    case 'u': {
        if (!csi->prefix) { sc_restore_cursor(t, &t->saved); break; }
        if (csi->nparams > (csi->prefix == '=' ? 2 : 1)) break;
        struct keyboard_mode *k = &t->keyboard[t->alternate];
        unsigned flags = (unsigned)csi->params[0] & 31;
        if (csi->prefix == '?') {
            char reply[32];
            snprintf(reply, sizeof(reply), "\033[?%uu", k->flags);
            send_response(t, reply);
        } else if (csi->prefix == '>') {
            if (k->depth == KEYBOARD_STACK_SIZE) {
                memmove(k->saved, k->saved + 1, sizeof(k->saved) - sizeof(k->saved[0]));
                k->depth--;
            }
            k->saved[k->depth++] = k->flags;
            k->flags = flags;
        } else if (csi->prefix == '<') {
            unsigned count = (unsigned)csi_count(csi);
            if (count > k->depth) { k->depth = 0; k->flags = 0; }
            else { k->depth -= count; k->flags = k->saved[k->depth]; }
        } else if (csi->prefix == '=') {
            int mode = csi->nparams > 1 && csi->params[1] ? csi->params[1] : 1;
            if (mode == 1) k->flags = flags;
            else if (mode == 2) k->flags |= flags;
            else if (mode == 3) k->flags &= ~flags;
        }
        break;
    }
    case 'h':
    case 'l': {
        bool enabled = csi->final == 'h';
        bool priv = csi->prefix == '?';
        for (int p = 0; p < csi->nparams; p++)
            mode_set(t, csi->params[p], enabled, priv);
        break;
    }
    case 'q': {
        if (csi->prefix == '>' && !csi->intermediate &&
            (!csi->nparams || !csi->params[0])) {
            char resp[96];
            snprintf(resp, sizeof(resp), "\033P>|%s\033\\", t->name);
            send_response(t, resp);
        } else if (!csi->prefix && csi->intermediate == ' ') {
            wsap_vtcore_sc_set_cursor_style(t, csi->nparams > 0 ? csi->params[0] : 0);
        }
        break;
    }
    case 'p':
        if (csi->intermediate == '!') {
            soft_reset(t);
            sc_move(t, 0, 0);
        } else if (csi->intermediate == '$' && csi->nparams >= 1) {
            char reply[32];
            int set = mode_get(t, csi->params[0], csi->prefix == '?');
            snprintf(reply, sizeof(reply), "\033[%s%d;%d$y",
                     csi->prefix == '?' ? "?" : "", csi->params[0],
                     set < 0 ? 0 : set ? 1 : 2);
            send_response(t, reply);
        }
        break;
    default:
        break;
    }
}

/* Formats one trace line and hands it to the host's trace callback. */
static void esc_tracef(const struct wsap_vtcore *t, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    t->cb.trace(t->cb.ud, line);
}

/* The guard keeps formatting off the parser path when tracing is off. */
#define ESC_TRACE(t, ...) \
    do { if ((t)->cb.trace) esc_tracef((t), __VA_ARGS__); } while (0)

static void dispatch_esc(struct wsap_vtcore *t, unsigned char ch)
{
    t->state = ST_GROUND;
    switch (ch) {
    case '=':
        t->application_keypad = true;
        break;
    case '>':
        t->application_keypad = false;
        break;
    case 'H':
        t->tabs[t->cursor_x] = 1;
        t->custom_tabs = true;
        break;
    case 'c':
        if (t->alternate) wsap_vtcore_screen_set_alternate(t, false);
        soft_reset(t);
        wsap_vtcore_sc_reset(t);
        break;
    case '[':
        ESC_TRACE(t, "ESC[ → ST_CSI");
        t->state = ST_CSI;
        csi_parser_reset(&t->csi_parser);
        break;
    case ']':
        ESC_TRACE(t, "ESC] → ST_OSC");
        t->state = ST_OSC;
        t->osc_len = 0;
        t->str_truncated = false;
        break;
    case 'P':
        ESC_TRACE(t, "ESCP → ST_DCS");
        t->state = ST_DCS;
        t->dcs_len = 0;
        t->str_truncated = false;
        break;
    case '_':
    case '^':
    case 'X':
        t->state = ST_IGNORE;
        break;
    case '#':
        t->state = ST_DECALN;
        break;
    case '%':
        t->state = ST_DOCS;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
        t->charset_designation = (char)ch;
        t->state = ST_CHARSET;
        break;
    case 'D':
    case 'E':
        if (ch == 'E') sc_cr(t);
        sc_linefeed(t, 1);
        break;
    case 'M':
        if (t->cursor_y == t->scroll_top)
            wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom, 1, true, false);
        else
            sc_move(t, t->cursor_x, t->cursor_y > 0 ? t->cursor_y - 1 : 0);
        break;
    case '7':
        t->saved = wsap_vtcore_cursor_save(t);
        break;
    case '8':
        sc_restore_cursor(t, &t->saved);
        break;
    default:
        break;
    }
}

void wsap_vtcore_escape_byte(struct wsap_vtcore *t, unsigned char ch)
{
    if (ch >= 0x80 && t->state != ST_OSC && t->state != ST_DCS &&
        t->state != ST_IGNORE) {
        ESC_TRACE(t, "  0x%02x in state %d → abort → ST_GROUND", ch, t->state);
        t->osc_len = 0;
        t->dcs_len = 0;
        t->state = ST_GROUND;
        wsap_vtcore_text_ground(t, ch);
        return;
    }
    switch (t->state) {
    case ST_ESC:
        ESC_TRACE(t, "  ESC+0x%02x('%c')", ch, ch >= 0x20 && ch < 0x7f ? ch : '.');
        if (ch < 0x20) {
            t->state = ST_GROUND;
            wsap_vtcore_text_ground(t, ch);
        } else {
            dispatch_esc(t, ch);
        }
        break;
    case ST_CSI:
        if (ch == 0x1b) {
            ESC_TRACE(t, "  CSI interrupted by ESC");
            t->state = ST_ESC;
        } else if (ch == 0x18 || ch == 0x1a) {
            ESC_TRACE(t, "  CSI cancelled by 0x%02x", ch);
            t->state = ST_GROUND;
        } else if (ch < 0x20) {
            /* Other C0 controls run inside the sequence (ECMA-48 5.5,
             * xterm). */
            wsap_vtcore_text_ground(t, ch);
        }
        break;
    case ST_CHARSET:
        if (t->charset_designation == '(' || t->charset_designation == ')') {
            t->charsets[t->charset_designation == ')'] = (char)ch;
            t->graphics = t->charsets[t->charset_active] == '0';
        }
        t->state = ST_GROUND;
        break;
    case ST_DOCS:
        if (ch != 'G' && ch != '@' && ch != '/')
            wsap_vtcore_text_ground(t, '%');
        t->state = ST_GROUND;
        if (ch != 'G' && ch != '@' && ch != '/')
            wsap_vtcore_text_ground(t, ch);
        break;
    case ST_DECALN:
        t->state = ST_GROUND;
        if (ch == '8')
            wsap_vtcore_sc_decaln(t);
        break;
    case ST_IGNORE:
        if (ch == 0x07 || ch == 0x18 || ch == 0x1a)
            t->state = ST_GROUND;
        else if (ch == 0x1b)
            t->state = ST_IGNORE_ESC;
        break;
    case ST_IGNORE_ESC:
        if (ch == '\\' || ch == 0x07 || ch == 0x18 || ch == 0x1a)
            t->state = ST_GROUND;
        else {
            t->state = ST_ESC;
            wsap_vtcore_escape_byte(t, ch);
        }
        break;
    case ST_OSC:
        if (ch == 0x07) {
            wsap_vtcore_osc_finish(t);
            t->state = ST_GROUND;
        } else if (ch == 0x1b) {
            t->state = ST_STR_ESC;
        } else if (ch < 0x20) {
            t->osc_len = 0;
            t->state = ST_GROUND;
            wsap_vtcore_text_ground(t, ch);
        } else if (wsap_vtcore_osc_room(t) || wsap_vtcore_osc_grow(t)) {
            *wsap_vtcore_osc_tail(t) = (char)ch;
            t->osc_len++;
        } else {
            t->str_truncated = true;
        }
        break;
    case ST_DCS:
        if (ch == 0x07) {
            dcs_dispatch(t);
            t->dcs_len = 0;
            t->state = ST_GROUND;
        } else if (ch == 0x1b) {
            t->state = ST_DCS_ESC;
        } else if (ch < 0x20) {
            t->dcs_len = 0;
            t->state = ST_GROUND;
            wsap_vtcore_text_ground(t, ch);
        } else if (dcs_reserve(t, 1)) {
            t->dcs_buf[t->dcs_len++] = (char)ch;
        } else {
            t->str_truncated = true;
        }
        break;
    case ST_DCS_ESC:
        if (ch == '\\') {
            dcs_dispatch(t);
            t->dcs_len = 0;
            t->state = ST_GROUND;
        } else {
            t->dcs_len = 0;
            t->state = ST_ESC;
            wsap_vtcore_escape_byte(t, ch);
        }
        break;
    case ST_STR_ESC:
        if (ch == '\\') {
            wsap_vtcore_osc_finish(t);
            t->state = ST_GROUND;
        } else {
            t->osc_len = 0;
            t->dcs_len = 0;
            t->state = ST_ESC;
            wsap_vtcore_escape_byte(t, ch);
        }
        break;
    default:
        break;
    }
}

size_t wsap_vtcore_escape_str(struct wsap_vtcore *t, const char *buf, size_t len)
{
    if (t->state == ST_IGNORE) {
        size_t take = scan_printable_span(buf, len, 0x80);
        if (take < len)
            wsap_vtcore_escape_byte(t, (unsigned char)buf[take++]);
        return take;
    }
    int is_osc = t->state == ST_OSC;
    size_t take = scan_printable_span(buf, len, 0x80);
    if (is_osc) {
        size_t room = wsap_vtcore_osc_room(t);
        if (take > room && wsap_vtcore_osc_grow(t))
            room = wsap_vtcore_osc_room(t);
        if (room) {
            if (take > room)
                take = room;
            memcpy(wsap_vtcore_osc_tail(t), buf, take);
            t->osc_len += (int)take;
        } else {
            t->str_truncated = true;
        }
    } else {
        dcs_reserve(t, take);
        size_t room = t->dcs_cap - (size_t)t->dcs_len;
        if (room) {
            if (take > room)
                take = room;
            memcpy(t->dcs_buf + t->dcs_len, buf, take);
            t->dcs_len += (int)take;
        } else {
            t->str_truncated = true;
        }
    }
    if (take < len)
        wsap_vtcore_escape_byte(t, (unsigned char)buf[take++]);
    return take;
}

size_t wsap_vtcore_escape_csi(struct wsap_vtcore *t, const char *buf, size_t len)
{
    size_t consumed;
    enum csi_result result = csi_read(&t->csi_parser, buf, len, &consumed);
    if (result == CSI_CONTROL) {
        wsap_vtcore_escape_byte(t, (unsigned char)buf[consumed++]);
    } else if (result == CSI_DISPATCH || result == CSI_DISCARD) {
        if (t->cb.trace) {
            const struct csi *c = &t->csi_parser.csi;
            char params[256] = "-1";
            if (c->nparams) {
                size_t n = 0;
                for (int j = 0; j < c->nparams && n < sizeof params; j++)
                    n += (size_t)snprintf(params + n, sizeof params - n,
                                          j ? ";%d" : "%d", c->params[j]);
            }
            esc_tracef(t, "  CSI complete: %s%c p=[%s] prefix='%c' inter='%c'",
                       result == CSI_DISCARD ? "INVALID " : "", c->final, params,
                       c->prefix ? c->prefix : ' ', c->intermediate ? c->intermediate : ' ');
        }
        if (result == CSI_DISPATCH)
            dispatch_csi(t, &t->csi_parser.csi);
        t->state = ST_GROUND;
    }
    return consumed;
}

void wsap_vtcore_escape_begin(struct wsap_vtcore *t)
{
    ESC_TRACE(t, "0x1b → ST_ESC");
    t->state = ST_ESC;
}
