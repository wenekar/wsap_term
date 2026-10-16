#include "compat.h"
#include "core.h"
#include "kernels.h"
#include "text_run.h"
#include "screen.h"
#include "escape.h"
#include "palette.h"
#include "unicode/grapheme.h"
#include "unicode/unicode.h"
#include "text.h"
#include "unicode/utf8.h"
#include <stdio.h>
#include <string.h>

static inline void emit_codepoint(struct wsap_vtcore *t, uint32_t cp, int klass, uint16_t flags)
{
    static const uint32_t dec_graphics[] = {
        0x00a0,
        0x25c6,0x2592,0x2409,0x240c,0x240d,0x240a,0x00b0,0x00b1,
        0x2424,0x240b,0x2518,0x2510,0x250c,0x2514,0x253c,0x23ba,
        0x23bb,0x2500,0x23bc,0x23bd,0x251c,0x2524,0x2534,0x252c,
        0x2502,0x2264,0x2265,0x03c0,0x2260,0x00a3,0x00b7 };
    if (cp >= 0x80 && cp <= 0x9f) {
        static const char c1_map[32] = {
            [4] = 'D', [5] = 'E', [8] = 'H', [13] = 'M',
            [16] = 'P', [27] = '[', [28] = '\\', [29] = ']',
        };
        char ch = c1_map[cp - 0x80];
        if (ch) {
            t->state = ST_ESC;
            wsap_vtcore_escape_byte(t, (unsigned char)ch);
        }
        return;
    }
    if (t->graphics && cp >= '_' && cp <= '~') {
        cp = dec_graphics[cp - '_'];
        klass = -1;
    }
    sc_print_cell(t, cp, klass, flags);
}

static inline void emit_decoded(struct wsap_vtcore *t, uint32_t cp, uint16_t flags)
{
    int klass = wsap_vtcore_text_class(cp);
    if (!wsap_vtcore_sc_extend_cluster(t, cp, klass))
        emit_codepoint(t, cp, klass, flags);
}

/* A byte that cannot continue a UTF-8 sequence ends it: the unfinished
 * sequence prints U+FFFD (Unicode 3.9, maximal subparts). */
void wsap_vtcore_text_utf8_interrupt(struct wsap_vtcore *t)
{
    if (WSAP_VTCORE_EXPECT(t->utf8_state == WSAP_VTCORE_UTF8_ACCEPT, 1))
        return;
    t->utf8_state = WSAP_VTCORE_UTF8_ACCEPT;
    emit_decoded(t, 0xFFFD, wsap_vtcore_current_flags(t));
}

void wsap_vtcore_text_ground(struct wsap_vtcore *t, unsigned char ch)
{
    if (ch >= 0x20 && ch < 0x7f && t->utf8_state == WSAP_VTCORE_UTF8_ACCEPT) {
        emit_codepoint(t, ch, 1, wsap_vtcore_current_flags(t));
        return;
    }

    if (ch < 0x20 || ch == 0x7f)
        wsap_vtcore_text_utf8_interrupt(t);

    if (WSAP_VTCORE_EXPECT(ch == 0x1b, 0)) {
        wsap_vtcore_escape_begin(t);
    } else if (ch == '\n' || ch == 0x0b || ch == 0x0c) {
        /* VT and FF act as LF (xterm). */
        if (wsap_vtcore_lnm(t)) sc_cr(t);
        sc_linefeed(t, 1);
    } else if (ch == '\r') {
        sc_cr(t);
    } else if (ch == '\b') {
        t->pending_wrap = false;
        if (t->cursor_x > 0)
            t->cursor_x--;
        wsap_vtcore_row(t, t->cursor_y)->version++;
    } else if (ch == '\t') {
        wsap_vtcore_sc_tab(t, 1);
    } else if (ch == 0x07) {
        if (t->cb.bell)
            t->cb.bell(t->cb.ud);
    } else if (ch == 0x0e) {
        t->charset_active = 1;
        t->graphics = t->charsets[1] == '0';
    } else if (ch == 0x0f) {
        t->charset_active = 0;
        t->graphics = t->charsets[0] == '0';
    } else if (ch >= 0x20 && ch != 0x7f) {
        uint32_t before = t->utf8_state;
        uint32_t st = wsap_vtcore_utf8_decode(&t->utf8_state, &t->utf8_codep, ch);
        if (st == WSAP_VTCORE_UTF8_ACCEPT) {
            emit_decoded(t, t->utf8_codep, wsap_vtcore_current_flags(t));
        } else if (st == WSAP_VTCORE_UTF8_REJECT) {
            t->utf8_state = WSAP_VTCORE_UTF8_ACCEPT;
            emit_decoded(t, 0xFFFD, wsap_vtcore_current_flags(t));
            /* The byte that ended an unfinished sequence starts anew. */
            if (before != WSAP_VTCORE_UTF8_ACCEPT)
                wsap_vtcore_text_ground(t, ch);
        }
    }
}

static inline bool can_stream_bottom(const struct wsap_vtcore *t)
{
    return t->state == ST_GROUND && !t->graphics && !t->insert_mode && t->autowrap &&
           !t->lr_margin_mode &&
           t->utf8_state == WSAP_VTCORE_UTF8_ACCEPT && !t->inverse &&
           t->scroll_top == 0 && t->scroll_bottom == t->rows - 1 &&
           t->cursor_y == t->rows - 1 &&
           !t->alternate && t->sb_capacity > 0 &&
           using_default_colors(t);
}

static inline bool try_text_run(struct wsap_vtcore *t, const char *buf, size_t len,
                                size_t *idx)
{
    size_t i = *idx;
    if (t->pending_wrap || i + 4 > len ||
        ((unsigned char)buf[i] >= 0x80 &&
         ((unsigned char)buf[i] < 0xc2 || (unsigned char)buf[i] > 0xf4)) ||
        t->last_char == 0x200D || wsap_vtcore_grapheme_is_handle(t->last_char))
        return false;
    if (t->cursor_x != 0 &&
        t->cursor_x != wsap_vtcore_row(t, t->cursor_y)->len)
        return false;
    if ((unsigned char)buf[i] >= 0x80) {
        uint32_t cp;
        int bytes = wsap_vtcore_utf8_decode_one(buf + i, len - i, &cp);
        if (!bytes || wsap_vtcore_text_class(cp) <= 0)
            return false;
    }
    struct text_pen pen = wsap_vtcore_text_pen(t);
    bool wrote = false;
    do {
        struct srow *text = t->insert_mode || t->lr_margin_mode ? NULL : text_append_row(t);
        if (text) {
            int x = t->cursor_x, avail = t->cols - x;
            struct text_run_bytes r = wsap_vtcore_text_run_bytes(text->text.text + text->text.text_len,
                                                          buf + i, (int)(len - i), avail,
                                                          t->last_char, &t->text_width);
            if (!r.bytes)
                break;
            wrote = true;
            text_commit(text, r.out, r.chars, r.cols,
                        (uint8_t)((r.narrow ? HLINE_NARROW : 0) | (r.wide ? HLINE_HAS_WIDE : 0)),
                        pen);
            t->last_char = r.last;
            sc_text_advance(t, x, r.cols, avail);
            i += (size_t)r.bytes;
        } else {
            struct span span = sc_span_begin(t, t->cols);
            if (span.avail < 2)
                break;
            struct text_run_result r = wsap_vtcore_text_run(span.cells, buf + i, (int)(len - i),
                                                     span.avail, pen.fg, pen.bg, pen.sl, t->last_char,
                                                     &t->text_width);
            if (!r.bytes)
                break;
            wrote = true;
            sc_span_end(t, &span, r.cols, 0, r.last);
            i += (size_t)r.bytes;
        }
        bool newline = false;
        if (i < len && buf[i] == '\r') {
            sc_cr(t);
            i++;
            if (i < len && buf[i] == '\n') {
                sc_linefeed(t, 1);
                i++;
                newline = true;
            }
        } else if (i < len && buf[i] == '\n') {
            if (wsap_vtcore_lnm(t)) sc_cr(t);
            sc_linefeed(t, 1);
            i++;
            newline = true;
        }
        if (!newline || i + 4 > len || t->pending_wrap ||
            t->last_char == 0x200D || wsap_vtcore_grapheme_is_handle(t->last_char) ||
            (t->cursor_x != 0 &&
             t->cursor_x != wsap_vtcore_row(t, t->cursor_y)->len))
            break;
    } while (true);
    if (wrote) *idx = i - 1;
    return wrote;
}

/* The feed flags this build knows. wsap_vtcore_feed() rejects any other bit. */
#define WSAP_VTCORE_FEED_KNOWN WSAP_VTCORE_FEED_ONLCR

static void wsap_vtcore_feed_inner(struct wsap_vtcore *t, const char *buf, size_t len, unsigned flags)
{
    t->transport_newline = (flags & WSAP_VTCORE_FEED_ONLCR) != 0;
    uint16_t cur_flags = wsap_vtcore_current_flags(t);

    for (size_t i = 0; i < len; i++) {
        while (t->state != ST_GROUND) {
            if (t->state == ST_CSI)
                i += wsap_vtcore_escape_csi(t, buf + i, len - i);
            else if (t->state == ST_OSC || t->state == ST_DCS || t->state == ST_IGNORE)
                i += wsap_vtcore_escape_str(t, buf + i, len - i);
            else
                wsap_vtcore_escape_byte(t, (unsigned char)buf[i++]);
            cur_flags = wsap_vtcore_current_flags(t);
            if (i == len) return;
        }

        unsigned char ch = (unsigned char)buf[i];
        if (WSAP_VTCORE_EXPECT(ch >= 0x20 && ch < 0x7f && !t->insert_mode && !t->lr_margin_mode &&
                             t->state == ST_GROUND && !t->graphics &&
                             t->utf8_state == WSAP_VTCORE_UTF8_ACCEPT, 1)) {
            int avail = t->pending_wrap ? t->cols : t->cols - t->cursor_x;
            int rem = (int)(len - i);
            if (rem > avail) rem = avail;
            int run = 1;
            if (rem > 1 && (unsigned char)buf[i + 1] >= 0x20 &&
                (unsigned char)buf[i + 1] < 0x7f)
                run += scan_ascii_run(buf + i + 1, rem - 1);
            if (WSAP_VTCORE_EXPECT(can_stream_bottom(t), 1) &&
                (run == t->cols ||
                 (i + (size_t)run < len &&
                  (buf[i + run] == '\r' || buf[i + run] == '\n')))) {
                struct srow *row = wsap_vtcore_row(t, t->cursor_y);
                if (t->pending_wrap ||
                    (t->cursor_x == 0 && row && row->len == 0)) {
                    size_t done = wsap_vtcore_sc_bulk_ascii_lines(t, buf + i, len - i);
                    if (done > 0) {
                        i += done - 1;
                        continue;
                    }
                }
            }
            if (run == 1) {
                if (i + 1 < len && (unsigned char)buf[i + 1] >= 0xc2 &&
                    (unsigned char)buf[i + 1] <= 0xf4 &&
                    (i + 2 == len || ((unsigned char)buf[i + 2] & 0xc0) == 0x80)) {
                    uint32_t next_cp;
                    if (wsap_vtcore_utf8_decode_one(buf + i + 1, len - i - 1, &next_cp) &&
                        wsap_vtcore_text_class(next_cp) > 0 &&
                        try_text_run(t, buf, len, &i))
                        continue;
                }
                sc_print_cell(t, ch, 1, cur_flags);
                continue;
            }
            if (i + (size_t)run < len && (unsigned char)buf[i + run] >= 0x80) {
                uint32_t next_cp;
                if (wsap_vtcore_utf8_decode_one(buf + i + run, len - i - (size_t)run, &next_cp) &&
                    wsap_vtcore_text_class(next_cp) > 0 &&
                    try_text_run(t, buf, len, &i))
                    continue;
            }
            sc_print_ascii(t, buf + i, run);
            i += (size_t)run;
            if (i < len && buf[i] == '\r') {
                sc_cr(t);
                i++;
                if (i < len && buf[i] == '\n') {
                    sc_linefeed(t, 1);
                    i++;
                }
            } else if (i < len && buf[i] == '\n') {
                if (wsap_vtcore_lnm(t)) sc_cr(t);
                sc_linefeed(t, 1);
                i++;
            }
            i--;
            continue;
        }

        if (WSAP_VTCORE_EXPECT(ch >= 0x80, 1)) {
            if (WSAP_VTCORE_EXPECT(t->state == ST_GROUND &&
                            t->utf8_state == WSAP_VTCORE_UTF8_ACCEPT && !t->graphics, 1)) {
                if (ch >= 0xc2 && ch <= 0xf4 &&
                    (i + 1 == len || ((unsigned char)buf[i + 1] & 0xc0) == 0x80)) {
                    if (!t->insert_mode && !t->lr_margin_mode &&
                        try_text_run(t, buf, len, &i))
                        continue;
                } else {
                    if (WSAP_VTCORE_EXPECT(t->last_char != 0x200D && !wsap_vtcore_grapheme_is_handle(t->last_char), 1))
                        sc_print_cell(t, 0xFFFD, 1, cur_flags);
                    else
                        emit_decoded(t, 0xFFFD, cur_flags);
                    continue;
                }
                uint32_t cp;
                int cp_len = wsap_vtcore_utf8_decode_ground(&t->utf8_state, &t->utf8_codep, buf + i, len - i, &cp);
                if (cp)
                    emit_decoded(t, cp, cur_flags);
                i += (size_t)(cp_len - 1);
                continue;
            }
            if (WSAP_VTCORE_EXPECT(t->state == ST_GROUND, 1))
                wsap_vtcore_text_ground(t, ch);
            else
                wsap_vtcore_escape_byte(t, ch);
            continue;
        }

        if (WSAP_VTCORE_EXPECT(ch < 0x20 || ch == 0x7f, 0)) {
            if (t->state == ST_GROUND) {
                wsap_vtcore_text_utf8_interrupt(t);
                if (ch == 0x7f || ((1u << ch) & 0x0800FF80u) == 0)
                    continue;

                switch (ch) {
                case '\n':
                case 0x0b:
                case 0x0c: {
                    size_t end = i + 1 + scan_byte_run(buf + i + 1, len - i - 1, '\n');
                    if (wsap_vtcore_lnm(t)) sc_cr(t);
                    sc_linefeed(t, end - i);
                    i = end - 1;
                    continue;
                }
                case '\r':
                    if (i + 1 < len && buf[i + 1] == '\n') {
                        size_t end = i + 2 + scan_crlf_pairs(buf + i + 2, len - i - 2);
                        sc_cr(t);
                        sc_linefeed(t, (end - i) / 2);
                        i = end - 1;
                    } else {
                        sc_cr(t);
                    }
                    continue;
                case 0x1b:
                    if (t->utf8_state == WSAP_VTCORE_UTF8_ACCEPT && i + 1 < len &&
                        buf[i + 1] == '[' && !t->insert_mode &&
                        !t->lr_margin_mode && !t->graphics) {
                        int fast = wsap_vtcore_try_fast_csi(t, buf + i + 2, len - i - 2);
                        if (fast > 0) {
                            cur_flags = wsap_vtcore_current_flags(t);
                            i += 1 + (size_t)fast;
                            continue;
                        }
                        t->state = ST_CSI;
                        csi_parser_reset(&t->csi_parser);
                        i += 1 + wsap_vtcore_escape_csi(t, buf + i + 2, len - i - 2);
                        cur_flags = wsap_vtcore_current_flags(t);
                        continue;
                    }
                    wsap_vtcore_text_ground(t, ch);
                    cur_flags = wsap_vtcore_current_flags(t);
                    continue;
                default:
                    wsap_vtcore_text_ground(t, ch);
                    cur_flags = wsap_vtcore_current_flags(t);
                    continue;
                }
            } else {
                wsap_vtcore_escape_byte(t, ch);
                cur_flags = wsap_vtcore_current_flags(t);
                continue;
            }
        }

        if (WSAP_VTCORE_EXPECT(t->state == ST_GROUND, 1))
            wsap_vtcore_text_ground(t, ch);
        else
            wsap_vtcore_escape_byte(t, ch);
        cur_flags = wsap_vtcore_current_flags(t);
    }
}

bool wsap_vtcore_feed(struct wsap_vtcore *t, const char *buf, size_t len, unsigned flags)
{
    if (!t || (!buf && len > 0) || (flags & ~WSAP_VTCORE_FEED_KNOWN))
        return false;
    if (len == 0)
        return true;
    wsap_vtcore_write_begin(t);
    wsap_vtcore_feed_inner(t, buf, len, flags);
    wsap_vtcore_write_end(t);
    return true;
}

void wsap_vtcore_text_repeat(struct wsap_vtcore *t, int count)
{
    int screen = t->cols * t->rows;
    if (count > screen) count = screen;
    if (count <= 0 || !t->last_char)
        return;
    if (count == 1) {
        wsap_vtcore_sc_print_glyph(t, t->last_char, -1, wsap_vtcore_current_flags(t));
        return;
    }
    for (int j = 0; j < count && t->last_char; j++)
        wsap_vtcore_sc_print_glyph(t, t->last_char, -1, wsap_vtcore_current_flags(t));
}
