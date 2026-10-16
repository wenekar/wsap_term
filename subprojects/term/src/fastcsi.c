#include "compat.h"
#include "core.h"
#include "kernels.h"
#include "pen.h"
#include "screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

WSAP_VTCORE_ALWAYS_INLINE
static inline const char *parse_byte_delim(const char *s, const char *end, char delim, uint32_t *val)
{
    if (s >= end) return NULL;
    unsigned char d0 = (unsigned char)(*s++ - '0');
    if (d0 > 9) return NULL;
    if (s < end && *s == delim) {
        *val = d0;
        return s + 1;
    }
    if (s >= end) return NULL;
    unsigned char d1 = (unsigned char)(*s++ - '0');
    if (d1 > 9) return NULL;
    if (s < end && *s == delim) {
        *val = d0 * 10 + d1;
        return s + 1;
    }
    if (s >= end) return NULL;
    unsigned char d2 = (unsigned char)(*s++ - '0');
    if (d2 > 9 || s >= end || *s++ != delim) return NULL;
    unsigned v = d0 * 100 + d1 * 10 + d2;
    if (v > 255) return NULL;
    *val = v;
    return s;
}

static inline const char *parse_color_sequence(const char *p, const char *end, uint32_t *color)
{
    const char *s = p + 5;
    uint32_t r, g, b;
    s = parse_byte_delim(s, end, ';', &r);
    if (!s) return NULL;
    s = parse_byte_delim(s, end, ';', &g);
    if (!s) return NULL;
    s = parse_byte_delim(s, end, 'm', &b);
    if (!s) return NULL;
    *color = wsap_vtcore_rgb((int)r, (int)g, (int)b);
    return s;
}

static const char *parse_256color(const char *p, const char *end, uint32_t *color)
{
    uint32_t v;
    const char *s = parse_byte_delim(p + 2, end, 'm', &v);
    if (!s) return NULL;
    *color = wsap_vtcore_color_index(v);
    return s;
}

WSAP_VTCORE_NOINLINE
int wsap_vtcore_try_fast_csi(struct wsap_vtcore *t, const char *buf, size_t rem)
{
    const char *end = buf + rem;
    const char *p = buf;
    const char *esc = buf;
    const int cols = t->cols;
    const int rows = t->rows;
    uint32_t fg = t->cur_fg;
    uint32_t bg = t->cur_bg;
    uint32_t flags_pad = 0;
    bool flags_ok = false;

    while (p < end) {
        if (p + 5 <= end && (p[0] == '3' || p[0] == '4') && p[1] == '8' &&
            p[2] == ';' && (p[3] == '2' || p[3] == '5') && p[4] == ';') {
            bool is_fg = p[0] == '3';
            uint32_t *slot = is_fg ? &fg : &bg;
            uint32_t *other = is_fg ? &bg : &fg;
            const char *next = p[3] == '2' ? parse_color_sequence(p, end, slot)
                                           : parse_256color(p + 3, end, slot);
            if (!next) {
                p = esc;
                break;
            }
            p = next;
            if (p + 7 <= end && p[0] == '\033' && p[1] == '[' &&
                p[2] == (is_fg ? '4' : '3') && p[3] == '8' &&
                p[4] == ';' && (p[5] == '2' || p[5] == '5') && p[6] == ';') {
                next = p[5] == '2' ? parse_color_sequence(p + 2, end, other)
                                   : parse_256color(p + 5, end, other);
                if (!next) break;
                p = next;
            }
            if (p < end && (unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) {
                char ch = *p++;
                if (WSAP_VTCORE_EXPECT(!flags_ok, 0)) {
                    flags_pad = wsap_vtcore_flags_pad(t);
                    flags_ok = true;
                }
                if (WSAP_VTCORE_EXPECT(!sc_put1_fast(t, ch, (struct text_pen){ fg, bg, flags_pad }), 0)) {
                    t->cur_fg = fg;
                    t->cur_bg = bg;
                    sc_print_ascii(t, &ch, 1);
                }
                if (p + 2 <= end && p[0] == '\033' && p[1] == '[') {
                    esc = p;
                    p += 2;
                    continue;
                }
            }
        } else if (p[0] == 'm' || (p[0] == '0' && p + 1 < end && p[1] == 'm')) {
            reset_sgr(t);
            fg = t->cur_fg;
            bg = t->cur_bg;
            /* Recompute instead of zeroing. The cached flags carry the link
             * id, and reset_sgr leaves cur_link set, so zeroing here would
             * drop the hyperlink from every cell written after the reset. */
            flags_pad = wsap_vtcore_flags_pad(t);
            flags_ok = true;
            p += p[0] == 'm' ? 1 : 2;
        } else if (p[0] == 'J' ||
                   (p[0] >= '0' && p[0] <= '3' && p + 1 < end && p[1] == 'J')) {
            t->cur_fg = fg;
            t->cur_bg = bg;
            wsap_vtcore_screen_erase(t, p[0] == 'J' ? 0 : p[0] - '0');
            p += p[0] == 'J' ? 1 : 2;
        } else if (p[0] == 'K' ||
                   (p[0] >= '0' && p[0] <= '2' && p + 1 < end && p[1] == 'K')) {
            t->cur_fg = fg;
            t->cur_bg = bg;
            wsap_vtcore_screen_erase_line(t, p[0] == 'K' ? 0 : p[0] - '0');
            p += p[0] == 'K' ? 1 : 2;
        } else if (!t->origin_mode && !t->lr_margin_mode &&
                   ((*p >= '0' && *p <= '9') || *p == ';' ||
                    *p == 'A' || *p == 'B' || *p == 'C' || *p == 'D' ||
                    *p == 'E' || *p == 'F' || *p == 'G' || *p == '`' ||
                    *p == 'a' || *p == 'd' || *p == 'e' ||
                    *p == 'H' || *p == 'f' || *p == 'm' ||
                    *p == 'J' || *p == 'K' || *p == 'X' ||
                    *p == 'P' || *p == '@' || *p == 's' || *p == 'u')) {
            const char *q = p;
            int pv[12] = { 0 };
            int np = 0;
            while (q < end && ((*q >= '0' && *q <= '9') || *q == ';')) {
                if (*q == ';') {
                    if (np == 11) { q = NULL; break; }
                    np++;
                    q++;
                    continue;
                }
                if (pv[np] < 100000)
                    pv[np] = pv[np] * 10 + (*q - '0');
                q++;
            }
            if (!q || q >= end) {
                p = esc;
                break;
            }
            int nparams = q > p ? np + 1 : 0;
            int n = pv[0] > 0 ? pv[0] : 1;
            int nx = t->cursor_x, ny = t->cursor_y;
            switch (*q) {
            case 'A': ny -= n; goto vmove;
            case 'B': case 'e': ny += n; goto vmove;
            case 'E': ny += n; nx = 0; goto vmove;
            case 'F': ny -= n; nx = 0; goto vmove;
            vmove:
                if (WSAP_VTCORE_EXPECT(t->scroll_top != 0 || t->scroll_bottom != rows - 1, 0)) {
                    p = esc;
                    goto done;
                }
                goto move;
            case 'C': case 'a': nx += n; goto move;
            case 'D': nx -= n; goto move;
            case 'G': case '`': nx = n - 1; goto move;
            case 'd': ny = n - 1; goto move;
            case 'H': case 'f':
                ny = n - 1;
                nx = np && pv[1] > 0 ? pv[1] - 1 : 0;
                goto move;
            move:
                sc_move(t, nx, ny);
                p = q + 1;
                break;
            case 'm': {
                if (nparams == 1) {
                    int v = pv[0];
                    switch (v) {
                    case 0: reset_sgr(t); fg = t->cur_fg; bg = t->cur_bg; break;
                    case 1: t->bold = true; break;
                    case 2: t->faint = true; break;
                    case 3: t->italic = true; break;
                    case 4: t->underline = 1; break;
                    case 7: t->inverse = true; break;
                    case 9: t->strike = true; break;
                    case 21: t->underline = 2; break;
                    case 22: t->bold = false; t->faint = false; break;
                    case 23: t->italic = false; break;
                    case 24: t->underline = 0; break;
                    case 27: t->inverse = false; break;
                    case 29: t->strike = false; break;
                    case 39: fg = WSAP_VTCORE_COLOR_FG; break;
                    case 49: bg = WSAP_VTCORE_COLOR_BG; break;
                    case 53: t->overline = true; break;
                    case 55: t->overline = false; break;
                    case 59: t->cur_ul = 0; break;
                    default:
                        if ((unsigned)(v - 30) < 8) fg = wsap_vtcore_color_index(v - 30);
                        else if ((unsigned)(v - 40) < 8) bg = wsap_vtcore_color_index(v - 40);
                        else if ((unsigned)(v - 90) < 8) fg = wsap_vtcore_color_index(v - 90 + 8);
                        else if ((unsigned)(v - 100) < 8) bg = wsap_vtcore_color_index(v - 100 + 8);
                        break;
                    }
                    flags_ok = false;
                    p = q + 1;
                    break;
                }
                for (int j = 0; j < nparams; j++) {
                    int v = pv[j];
                    switch (v) {
                    case 0: reset_sgr(t); fg = t->cur_fg; bg = t->cur_bg; break;
                    case 1: t->bold = true; break;
                    case 2: t->faint = true; break;
                    case 3: t->italic = true; break;
                    case 4: t->underline = 1; break;
                    case 7: t->inverse = true; break;
                    case 9: t->strike = true; break;
                    case 21: t->underline = 2; break;
                    case 22: t->bold = false; t->faint = false; break;
                    case 23: t->italic = false; break;
                    case 24: t->underline = 0; break;
                    case 27: t->inverse = false; break;
                    case 29: t->strike = false; break;
                    case 38: case 48: case 58: {
                        uint32_t *slot = v == 38 ? &fg : v == 48 ? &bg : &t->cur_ul;
                        if (j + 2 < nparams && pv[j + 1] == 5) {
                            if (pv[j + 2] >= 0 && pv[j + 2] <= 255)
                                *slot = wsap_vtcore_color_index(pv[j + 2]);
                            j += 2;
                        } else if (j + 1 < nparams && pv[j + 1] == 2 &&
                                   j + 4 < nparams) {
                            *slot = wsap_vtcore_rgb(pv[j + 2], pv[j + 3], pv[j + 4]);
                            j += 4;
                        }
                        break;
                    }
                    case 39: fg = WSAP_VTCORE_COLOR_FG; break;
                    case 49: bg = WSAP_VTCORE_COLOR_BG; break;
                    case 53: t->overline = true; break;
                    case 55: t->overline = false; break;
                    case 59: t->cur_ul = 0; break;
                    default:
                        if ((unsigned)(v - 30) < 8) fg = wsap_vtcore_color_index(v - 30);
                        else if ((unsigned)(v - 40) < 8) bg = wsap_vtcore_color_index(v - 40);
                        else if ((unsigned)(v - 90) < 8) fg = wsap_vtcore_color_index(v - 90 + 8);
                        else if ((unsigned)(v - 100) < 8) bg = wsap_vtcore_color_index(v - 100 + 8);
                        break;
                    }
                }
                flags_ok = false;
                p = q + 1;
                break;
            }
            case 'J':
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_erase(t, pv[0]);
                p = q + 1;
                break;
            case 'K':
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_erase_line(t, pv[0]);
                p = q + 1;
                break;
            case 'X':
                t->cur_fg = fg;
                t->cur_bg = bg;
                sc_erase_span(t, t->cursor_y, t->cursor_x,
                              n > cols - t->cursor_x ? cols : t->cursor_x + n);
                p = q + 1;
                break;
            case 'P':
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_delete_chars(t, n);
                p = q + 1;
                break;
            case '@':
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_insert_chars(t, n);
                p = q + 1;
                break;
            case 's':
                t->cur_fg = fg;
                t->cur_bg = bg;
                t->saved = wsap_vtcore_cursor_save(t);
                p = q + 1;
                break;
            case 'u':
                t->cur_fg = fg;
                t->cur_bg = bg;
                sc_restore_cursor(t, &t->saved);
                fg = t->cur_fg;
                bg = t->cur_bg;
                flags_ok = false;
                p = q + 1;
                if (t->origin_mode || t->graphics)
                    goto done;
                break;
            default:
                p = esc;
                goto done;
            }
        } else {
            p = esc;
            break;
        }
tail:
        if (p < end && (unsigned char)(*p - 0x20) < 0x5f && !t->pending_wrap) {
            t->cur_fg = fg;
            t->cur_bg = bg;
            int avail = cols - t->cursor_x;
            int limit = end - p < (ptrdiff_t)avail ? (int)(end - p) : avail;
            int run = scan_ascii_run(p, limit);
            if (WSAP_VTCORE_EXPECT(!flags_ok, 0)) {
                flags_pad = wsap_vtcore_flags_pad(t);
                flags_ok = true;
            }
            if (!sc_ascii_fast(t, p, run, (struct text_pen){ fg, bg, flags_pad }))
                sc_print_ascii(t, p, run);
            p += run;
        }
        if (p + 2 <= end && p[0] == '\033' && p[1] == '[') {
            esc = p;
            p += 2;
            continue;
        }
        if (p < end && *p == '\r') {
            sc_cr(t);
            p++;
            goto tail;
        }
        if (p < end && *p == '\n') {
            t->cur_fg = fg;
            t->cur_bg = bg;
            if (wsap_vtcore_lnm(t)) sc_cr(t);
            sc_linefeed(t, 1);
            p++;
            goto tail;
        }
        break;
    }
done:
    if (p == end && esc != buf && esc == p - 2)
        p = esc;
    t->cur_fg = fg;
    t->cur_bg = bg;
    if (p > buf)
        return (int)(p - buf);
    return 0;
}
