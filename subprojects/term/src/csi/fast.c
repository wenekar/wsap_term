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

WSAP_VTCORE_ALWAYS_INLINE
static inline const char *parse_byte_delim2(const char *s, const char *end, char delim1, char delim2, uint32_t *val, char *found_delim)
{
    if (s >= end) return NULL;
    unsigned char d0 = (unsigned char)(*s++ - '0');
    if (d0 > 9) return NULL;
    if (s < end && (*s == delim1 || *s == delim2)) {
        *val = d0;
        if (found_delim) *found_delim = *s;
        return s + 1;
    }
    if (s >= end) return NULL;
    unsigned char d1 = (unsigned char)(*s++ - '0');
    if (d1 > 9) return NULL;
    if (s < end && (*s == delim1 || *s == delim2)) {
        *val = d0 * 10 + d1;
        if (found_delim) *found_delim = *s;
        return s + 1;
    }
    if (s >= end) return NULL;
    unsigned char d2 = (unsigned char)(*s++ - '0');
    if (d2 > 9 || s >= end || (*s != delim1 && *s != delim2)) return NULL;
    unsigned v = d0 * 100 + d1 * 10 + d2;
    if (v > 255) return NULL;
    *val = v;
    if (found_delim) *found_delim = *s;
    return s + 1;
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

/* Test for "38;2;", "38;5;", "48;2;", or "48;5;" at p. lead is '3', '4', or 0 for either. */
static inline bool is_ext_color(const char *p, const char *end, char lead)
{
    return p + 5 <= end && (lead ? p[0] == lead : (p[0] == '3' || p[0] == '4')) &&
           p[1] == '8' && p[2] == ';' && (p[3] == '2' || p[3] == '5') && p[4] == ';';
}

/* Parse one color that is_ext_color found. The color can end with 'm' or ';'.
 * Set *delim to the byte that ends it. */
static const char *parse_ext_color(const char *p, const char *end, uint32_t *color, char *delim)
{
    uint32_t r = 0, g = 0, v;
    const char *s = p + 5;
    if (p[3] == '2') {
        s = parse_byte_delim(s, end, ';', &r);
        if (s) s = parse_byte_delim(s, end, ';', &g);
        if (!s) return NULL;
    }
    s = parse_byte_delim2(s, end, 'm', ';', &v, delim);
    if (!s) return NULL;
    *color = p[3] == '2' ? wsap_vtcore_rgb((int)r, (int)g, (int)v) : wsap_vtcore_color_index(v);
    return s;
}

/* Parse the colors at p up to the 'm' that ends the sequence: one color, or one
 * foreground and one background color. Write them only if the parse succeeds. */
WSAP_VTCORE_NOINLINE
static const char *parse_sgr_colors(const char *p, const char *end, uint32_t *fg, uint32_t *bg)
{
    bool is_fg = p[0] == '3';
    uint32_t first, second;
    char delim = 0;
    const char *s = parse_ext_color(p, end, &first, &delim);
    bool both = s && delim == ';';
    if (both) {
        s = is_ext_color(s, end, is_fg ? '4' : '3') ? parse_ext_color(s, end, &second, &delim) : NULL;
        if (delim != 'm')
            s = NULL;
    }
    if (!s)
        return NULL;
    *(is_fg ? fg : bg) = first;
    if (both)
        *(is_fg ? bg : fg) = second;
    return s;
}

/* Apply SGR 1, 2, 3, 4, or 7. Return false for other codes. */
static inline bool sgr_attr_on(struct wsap_vtcore *t, char code)
{
    switch (code) {
    case '1': t->bold = true; return true;
    case '2': t->faint = true; return true;
    case '3': t->italic = true; return true;
    case '4': t->underline = 1; return true;
    case '7': t->inverse = true; return true;
    default: return false;
    }
}

/* Apply SGR 30 to 37, 39, 40 to 47, 49, or 90 to 97. Return false for other codes. */
static inline bool sgr_color2(char a, char b, uint32_t *fg, uint32_t *bg)
{
    unsigned d = (unsigned)(b - '0');
    if (d < 8) {
        if (a == '3') { *fg = wsap_vtcore_color_index((int)d); return true; }
        if (a == '4') { *bg = wsap_vtcore_color_index((int)d); return true; }
        if (a == '9') { *fg = wsap_vtcore_color_index((int)d + 8); return true; }
    } else if (d == 9) {
        if (a == '3') { *fg = WSAP_VTCORE_COLOR_FG; return true; }
        if (a == '4') { *bg = WSAP_VTCORE_COLOR_BG; return true; }
    }
    return false;
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
            // Two colors in one sequence, such as 38;2;R;G;B;48;5;N.
            if (WSAP_VTCORE_EXPECT(!next, 0))
                next = parse_sgr_colors(p, end, &fg, &bg);
            // A sequence that is not complete goes to the general parser unchanged.
            // A cancel byte can then discard it.
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
            flags_pad = wsap_vtcore_flags_pad(t);
            flags_ok = true;
            p += p[0] == 'm' ? 1 : 2;
        } else if (p + 2 <= end && (p[0] == '0' || p[0] == '1') && p[1] == ';' &&
                   is_ext_color(p + 2, end, 0)) {
            // "0;" or "1;" before the colors. Parse all colors before a change.
            uint32_t nfg = p[0] == '0' ? WSAP_VTCORE_COLOR_FG : fg;
            uint32_t nbg = p[0] == '0' ? WSAP_VTCORE_COLOR_BG : bg;
            const char *next = parse_sgr_colors(p + 2, end, &nfg, &nbg);
            if (!next) {
                p = esc;
                break;
            }
            if (p[0] == '0')
                reset_sgr(t);
            else
                t->bold = true;
            fg = nfg;
            bg = nbg;
            flags_ok = false;
            p = next;
        } else if (p + 1 < end && p[1] == 'm' && sgr_attr_on(t, p[0])) {
            flags_ok = false;
            p += 2;
        } else if (p + 2 < end && p[2] == 'm' && sgr_color2(p[0], p[1], &fg, &bg)) {
            p += 3;
        } else if (p + 2 < end && p[2] == 'm' && p[0] == '2' && p[1] == '2') {
            t->bold = false;
            t->faint = false;
            flags_ok = false;
            p += 3;
        } else if (p + 3 < end && p[3] == 'm' && p[0] == '1' && p[1] == '0' &&
                   (unsigned)(p[2] - '0') < 8) {
            bg = wsap_vtcore_color_index(p[2] - '0' + 8);
            p += 4;
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
        } else if (p[0] == 'H' || (p + 4 <= end && p[0] == '1' && p[1] == ';' && p[2] == '1' && p[3] == 'H')) {
            // Origin mode and margins move the home position. The general parser handles them.
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0)) {
                p = esc;
                break;
            }
            sc_move(t, 0, 0);
            p += p[0] == 'H' ? 1 : 4;
        } else if (p[0] == 'C' && !t->origin_mode && !t->lr_margin_mode) {
            sc_move(t, t->cursor_x < cols - 1 ? t->cursor_x + 1 : cols - 1, t->cursor_y);
            p++;
        } else if (p[0] == 'D' && !t->origin_mode && !t->lr_margin_mode) {
            sc_move(t, t->cursor_x > 0 ? t->cursor_x - 1 : 0, t->cursor_y);
            p++;
        } else if (p[0] == 'B' && !t->origin_mode && !t->lr_margin_mode &&
                   t->scroll_top == 0 && t->scroll_bottom == rows - 1) {
            sc_move(t, t->cursor_x, t->cursor_y < rows - 1 ? t->cursor_y + 1 : rows - 1);
            p++;
        } else if (p[0] == 'A' && !t->origin_mode && !t->lr_margin_mode &&
                   t->scroll_top == 0 && t->scroll_bottom == rows - 1) {
            sc_move(t, t->cursor_x, t->cursor_y > 0 ? t->cursor_y - 1 : 0);
            p++;
        } else if (p < end && p[0] == '?') {
            const char *q = p + 1;
            int mode = 0;
            while (q < end && *q >= '0' && *q <= '9') {
                if (mode < 100000)
                    mode = mode * 10 + (*q - '0');
                q++;
            }
            if (q < end && (*q == 'h' || *q == 'l')) {
                bool enabled = (*q == 'h');
                if (mode == 25) {
                    t->cursor_visible = enabled;
                } else if (mode == 2004) {
                    t->bracketed_paste = enabled;
                } else if (mode == 1) {
                    t->application_cursor = enabled;
                } else if (mode == 12) {
                    t->cursor_blink = enabled;
                } else if (mode == 1004) {
                    t->focus_events = enabled;
                } else if (mode == 2026) {
                    t->synchronized_output = enabled;
                } else if (mode == 7) {
                    t->autowrap = enabled;
                    t->pending_wrap = false;
                } else {
                    p = esc;
                    break;
                }
                p = q + 1;
            } else {
                p = esc;
                break;
            }
        } else if (!t->origin_mode && !t->lr_margin_mode &&
                   ((*p >= '0' && *p <= '9') || *p == ';' ||
                    *p == 'A' || *p == 'B' || *p == 'C' || *p == 'D' ||
                    *p == 'E' || *p == 'F' || *p == 'G' || *p == '`' ||
                    *p == 'a' || *p == 'd' || *p == 'e' ||
                    *p == 'H' || *p == 'f' || *p == 'm' ||
                    *p == 'J' || *p == 'K' || *p == 'X' ||
                    *p == 'P' || *p == '@' || *p == 's' || *p == 'u')) {
            const char *q = p;
            int pv[12];
            pv[0] = 0;
            int np = 0;
            while (q < end && ((*q >= '0' && *q <= '9') || *q == ';')) {
                if (*q == ';') {
                    if (np == 11) { q = NULL; break; }
                    np++;
                    pv[np] = 0;
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
                    case 5: case 6: case 8: case 25: case 28: case 58: case 59:
                        p = esc;
                        goto done;
                    case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
                        fg = wsap_vtcore_color_index((uint8_t)(v - 30));
                        break;
                    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                        bg = wsap_vtcore_color_index((uint8_t)(v - 40));
                        break;
                    case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                        fg = wsap_vtcore_color_index((uint8_t)(v - 90 + 8));
                        break;
                    case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                        bg = wsap_vtcore_color_index((uint8_t)(v - 100 + 8));
                        break;
                    default:
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
                    case 38: case 48: {
                        uint32_t *slot = v == 38 ? &fg : &bg;
                        if (j + 2 < nparams && pv[j + 1] == 5) {
                            if (pv[j + 2] >= 0 && pv[j + 2] <= 255)
                                *slot = wsap_vtcore_color_index(pv[j + 2]);
                            j += 2;
                        } else if (j + 1 < nparams && pv[j + 1] == 2 &&
                                   j + 4 < nparams) {
                            /* Parameters stop at six digits here, but the base parser keeps
                             * all digits and the color keeps the low byte. Let it decide. */
                            if ((pv[j + 2] | pv[j + 3] | pv[j + 4]) >= 100000) {
                                p = esc;
                                goto done;
                            }
                            *slot = wsap_vtcore_rgb(pv[j + 2], pv[j + 3], pv[j + 4]);
                            j += 4;
                        }
                        break;
                    }
                    case 39: fg = WSAP_VTCORE_COLOR_FG; break;
                    case 49: bg = WSAP_VTCORE_COLOR_BG; break;
                    case 53: t->overline = true; break;
                    case 55: t->overline = false; break;
                    /* SGR parameters set absolute values, so the general
                     * parser can apply the whole sequence again. */
                    case 5: case 6: case 8: case 25: case 28: case 58: case 59:
                        p = esc;
                        goto done;
                    case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
                        fg = wsap_vtcore_color_index((uint8_t)(v - 30));
                        break;
                    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                        bg = wsap_vtcore_color_index((uint8_t)(v - 40));
                        break;
                    case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                        fg = wsap_vtcore_color_index((uint8_t)(v - 90 + 8));
                        break;
                    case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                        bg = wsap_vtcore_color_index((uint8_t)(v - 100 + 8));
                        break;
                    default:
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
