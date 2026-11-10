#include "compat.h"
#include "core.h"
#include "kernels.h"
#include "pen.h"
#include "screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Parse one color byte: one to three digits and the delimiter. One load holds
 * the digits and the delimiter of the common case. The value and the position
 * of the delimiter come from select instructions, so a component of two digits
 * costs the same as one of three and no branch can go wrong. */
WSAP_VTCORE_ALWAYS_INLINE
static inline const char *parse_byte_delim(const char *s, const char *end, char delim, uint32_t *val)
{
    if (WSAP_VTCORE_EXPECT(end - s >= 4, 1)) {
        uint32_t w;
        memcpy(&w, s, 4);
        uint32_t d0 = (w & 0xffu) - '0';
        uint32_t d1 = ((w >> 8) & 0xffu) - '0';
        uint32_t d2 = ((w >> 16) & 0xffu) - '0';
        uint32_t p0 = d0 < 10;
        uint32_t p1 = p0 & (d1 < 10);
        uint32_t p2 = p1 & (d2 < 10);
        uint32_t v = p1 ? d0 * 10 + d1 : d0;
        v = p2 ? v * 10 + d2 : v;
        uint32_t digits = p0 + p1 + p2;
        uint32_t next = (w >> (8 * digits)) & 0xffu;
        if (WSAP_VTCORE_EXPECT(digits == 0 || next != (uint32_t)(unsigned char)delim ||
                               (digits == 3 && v > 255), 0))
            return NULL;
        *val = v;
        return s + digits;
    }
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

/* Parse one color byte that two delimiters can end. Set *found_delim to the
 * byte that ends it. */
WSAP_VTCORE_ALWAYS_INLINE
static inline const char *parse_byte_delim2(const char *s, const char *end, char delim1, char delim2, uint32_t *val, char *found_delim)
{
    if (WSAP_VTCORE_EXPECT(end - s >= 4, 1)) {
        uint32_t w;
        memcpy(&w, s, 4);
        uint32_t d0 = (w & 0xffu) - '0';
        uint32_t d1 = ((w >> 8) & 0xffu) - '0';
        uint32_t d2 = ((w >> 16) & 0xffu) - '0';
        uint32_t p0 = d0 < 10;
        uint32_t p1 = p0 & (d1 < 10);
        uint32_t p2 = p1 & (d2 < 10);
        uint32_t v = p1 ? d0 * 10 + d1 : d0;
        v = p2 ? v * 10 + d2 : v;
        uint32_t digits = p0 + p1 + p2;
        uint32_t next = (w >> (8 * digits)) & 0xffu;
        if (WSAP_VTCORE_EXPECT(digits == 0 || (next != (uint32_t)(unsigned char)delim1 &&
                               next != (uint32_t)(unsigned char)delim2) ||
                               (digits == 3 && v > 255), 0))
            return NULL;
        *val = v;
        if (found_delim) *found_delim = (char)next;
        return s + digits;
    }
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

/* Parse the red, green, and blue bytes of "38;2;" or "48;2;". */
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

/* Parse the extended color at p and write it to fg or bg. Then parse a second
 * color that the same sequence or the next one holds. Return the byte after
 * the last color, or NULL when the bytes are not complete. */
static const char *parse_color_pair(const char *p, const char *end, uint32_t *fg, uint32_t *bg)
{
    bool is_fg = p[0] == '3';
    uint32_t *slot = is_fg ? fg : bg;
    uint32_t *other = is_fg ? bg : fg;
    const char *next = p[3] == '2' ? parse_color_sequence(p, end, slot)
                                   : parse_256color(p + 3, end, slot);
    /* Two colors in one sequence, such as 38;2;R;G;B;48;5;N. */
    if (WSAP_VTCORE_EXPECT(!next, 0))
        return parse_sgr_colors(p, end, fg, bg);
    /* A second color in the next sequence, such as 48;2;R;G;Bm ESC[38;5;Nm. */
    if (next + 7 <= end && next[0] == '\033' && next[1] == '[' &&
        next[2] == (is_fg ? '4' : '3') && next[3] == '8' &&
        next[4] == ';' && (next[5] == '2' || next[5] == '5') && next[6] == ';')
        return next[5] == '2' ? parse_color_sequence(next + 2, end, other)
                              : parse_256color(next + 5, end, other);
    return next;
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

/* Read up to three digits of a count and return the byte after them. A
 * straight line of tests is cheaper than a loop, because the exit of a short
 * loop costs a branch misprediction. */
WSAP_VTCORE_ALWAYS_INLINE
static inline const char *scan_count3(const char *p, const char *end, int *count)
{
    int v = 0;
    if (p < end) {
        unsigned d = (unsigned)((unsigned char)*p - '0');
        if (d <= 9) {
            v = (int)d;
            p++;
            if (p < end) {
                d = (unsigned)((unsigned char)*p - '0');
                if (d <= 9) {
                    v = v * 10 + (int)d;
                    p++;
                    if (p < end) {
                        d = (unsigned)((unsigned char)*p - '0');
                        if (d <= 9) {
                            v = v * 10 + (int)d;
                            p++;
                        }
                    }
                }
            }
        }
    }
    *count = v;
    return p;
}

/* Read a count of any length and return the byte after it. The value stops at
 * 100000, which is more than any screen holds. */
static inline const char *scan_count(const char *p, const char *end, int *count)
{
    int v = 0;
    while (p < end) {
        unsigned d = (unsigned)((unsigned char)*p - '0');
        if (d > 9) break;
        if (v < 100000) v = v * 10 + (int)d;
        p++;
    }
    *count = v;
    return p;
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
        unsigned char lead = (unsigned char)*p;

        /* 38;2;R;G;B, 38;5;N, 48;2;R;G;B, and 48;5;N. A color change before
         * each character is the common case in colored output. */
        if (WSAP_VTCORE_EXPECT((unsigned)(lead - '3') < 2 && p + 5 <= end &&
                               p[1] == '8' && p[2] == ';' &&
                               (p[3] == '2' || p[3] == '5') && p[4] == ';', 1)) {
            const char *next = parse_color_pair(p, end, &fg, &bg);
            /* A sequence that is not complete goes to the general parser
             * unchanged. A cancel byte can then discard it. */
            if (WSAP_VTCORE_EXPECT(!next, 0)) {
                p = esc;
                goto done;
            }
            p = next;
            /* Write the one character that follows a color sequence here. */
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
            goto tail;
        }

        if ((unsigned)(lead - '0') < 10 || lead == ';') {
            /* The short SGR forms come first. They are the most common
             * sequences in colored output. */
            if (p + 1 < end && p[1] == 'm') {
                /* One digit and the final byte. */
                if (lead == '0') {
                    reset_sgr(t);
                    fg = t->cur_fg;
                    bg = t->cur_bg;
                    flags_pad = wsap_vtcore_flags_pad(t);
                    flags_ok = true;
                    p += 2;
                    goto tail;
                }
                if (sgr_attr_on(t, (char)lead)) {
                    flags_ok = false;
                    p += 2;
                    goto tail;
                }
            }
            if ((lead == '0' || lead == '1') && p + 6 <= end && p[1] == ';' &&
                is_ext_color(p + 2, end, 0)) {
                /* "0;" or "1;" before two colors, such as 0;38;2;1;2;3m. */
                uint32_t nfg = lead == '0' ? WSAP_VTCORE_COLOR_FG : fg;
                uint32_t nbg = lead == '0' ? WSAP_VTCORE_COLOR_BG : bg;
                const char *next = parse_sgr_colors(p + 2, end, &nfg, &nbg);
                if (WSAP_VTCORE_EXPECT(!next, 0)) {
                    p = esc;
                    goto done;
                }
                if (lead == '0')
                    reset_sgr(t);
                else
                    t->bold = true;
                fg = nfg;
                bg = nbg;
                flags_ok = false;
                p = next;
                goto tail;
            }
            if (p + 2 < end && p[2] == 'm') {
                /* Two digits and the final byte. */
                if (sgr_color2((char)lead, p[1], &fg, &bg)) {
                    p += 3;
                    goto tail;
                }
                if (lead == '2' && p[1] == '2') {
                    t->bold = false;
                    t->faint = false;
                    flags_ok = false;
                    p += 3;
                    goto tail;
                }
            } else if (p + 3 < end && p[3] == 'm' && lead == '1' && p[1] == '0' &&
                       (unsigned)(p[2] - '0') < 8) {
                /* "100m" to "107m": the bright background colors. */
                bg = wsap_vtcore_color_index(p[2] - '0' + 8);
                p += 4;
                goto tail;
            }

            /* A count of up to three digits, then a final byte. */
            int count;
            const char *s = scan_count3(p, end, &count);
            if (WSAP_VTCORE_EXPECT(s >= end, 0)) {
                p = esc;
                goto done;
            }
            unsigned char fin = (unsigned char)*s;
            if (WSAP_VTCORE_EXPECT((unsigned)(fin - '0') < 10, 0))
                goto params;
            const int n = count > 0 ? count : 1;
            if (WSAP_VTCORE_EXPECT(fin == ';', 0)) {
                /* CSI row ; col H: the common two-parameter form. */
                int col;
                const char *after = scan_count3(s + 1, end, &col);
                if (after < end && (*after == 'H' || *after == 'f') &&
                    !t->origin_mode && !t->lr_margin_mode) {
                    sc_move(t, col > 0 ? col - 1 : 0, count > 0 ? count - 1 : 0);
                    p = after + 1;
                    goto tail;
                }
                goto params;
            }
            if (WSAP_VTCORE_EXPECT(fin == 'm', 1)) {
                /* One SGR code. Colors change the pen only, so the flags of
                 * the pen stay valid. */
                if ((unsigned)(count - 30) < 8) {
                    fg = wsap_vtcore_color_index((uint8_t)(count - 30));
                } else if ((unsigned)(count - 40) < 8) {
                    bg = wsap_vtcore_color_index((uint8_t)(count - 40));
                } else if ((unsigned)(count - 90) < 8) {
                    fg = wsap_vtcore_color_index((uint8_t)(count - 90 + 8));
                } else if ((unsigned)(count - 100) < 8) {
                    bg = wsap_vtcore_color_index((uint8_t)(count - 100 + 8));
                } else if (count == 0) {
                    reset_sgr(t);
                    fg = t->cur_fg;
                    bg = t->cur_bg;
                    flags_pad = wsap_vtcore_flags_pad(t);
                    flags_ok = true;
                } else if (count == 39) {
                    fg = WSAP_VTCORE_COLOR_FG;
                } else if (count == 49) {
                    bg = WSAP_VTCORE_COLOR_BG;
                } else if (count == 1) {
                    t->bold = true;
                    flags_ok = false;
                } else if (count == 22) {
                    t->bold = false;
                    t->faint = false;
                    flags_ok = false;
                } else if (count == 2) {
                    t->faint = true;
                    flags_ok = false;
                } else if (count == 3) {
                    t->italic = true;
                    flags_ok = false;
                } else if (count == 4) {
                    t->underline = 1;
                    flags_ok = false;
                } else if (count == 24) {
                    t->underline = 0;
                    flags_ok = false;
                } else if (count == 7) {
                    t->inverse = true;
                    flags_ok = false;
                } else if (count == 27) {
                    t->inverse = false;
                    flags_ok = false;
                } else if (count == 9) {
                    t->strike = true;
                    flags_ok = false;
                } else if (count == 29) {
                    t->strike = false;
                    flags_ok = false;
                } else if (count == 21) {
                    t->underline = 2;
                    flags_ok = false;
                } else if (count == 23) {
                    t->italic = false;
                    flags_ok = false;
                } else if (count == 53) {
                    t->overline = true;
                    flags_ok = false;
                } else if (count == 55) {
                    t->overline = false;
                    flags_ok = false;
                } else {
                    /* The general parser applies this code. */
                    p = esc;
                    goto done;
                }
                p = s + 1;
                goto tail;
            }

            /* The other final bytes need no origin mode and no left and right
             * margins. The general parser takes them when either is set. */
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0)) {
                p = esc;
                goto done;
            }
            if (fin == 'H' || fin == 'f') {
                sc_move(t, 0, n - 1);
                p = s + 1;
                goto tail;
            }
            if (fin == 'C' || fin == 'a') {
                sc_move(t, t->cursor_x + n, t->cursor_y);
                p = s + 1;
                goto tail;
            }
            if (fin == 'D') {
                sc_move(t, t->cursor_x - n, t->cursor_y);
                p = s + 1;
                goto tail;
            }
            if (fin == 'A' || fin == 'B' || fin == 'e' || fin == 'E' || fin == 'F') {
                /* Relative motion stops at a margin when the cursor starts inside it. */
                if (WSAP_VTCORE_EXPECT(t->scroll_top != 0 || t->scroll_bottom != rows - 1, 0))
                    goto params;
                bool up = fin == 'A' || fin == 'F';
                bool col0 = fin == 'E' || fin == 'F';
                sc_move(t, col0 ? 0 : t->cursor_x, up ? t->cursor_y - n : t->cursor_y + n);
                p = s + 1;
                goto tail;
            }
            if (fin == 'G' || fin == '`') {
                sc_move(t, n - 1, t->cursor_y);
                p = s + 1;
                goto tail;
            }
            if (fin == 'd') {
                sc_move(t, t->cursor_x, n - 1);
                p = s + 1;
                goto tail;
            }
            if (fin == 'J') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_erase(t, count);
                p = s + 1;
                goto tail;
            }
            if (fin == 'K') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_erase_line(t, count);
                p = s + 1;
                goto tail;
            }
            if (fin == 'X') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                sc_erase_span(t, t->cursor_y, t->cursor_x,
                              n > cols - t->cursor_x ? cols : t->cursor_x + n);
                p = s + 1;
                goto tail;
            }
            if (fin == 'P') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_delete_chars(t, n);
                p = s + 1;
                goto tail;
            }
            if (fin == '@') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                wsap_vtcore_screen_insert_chars(t, n);
                p = s + 1;
                goto tail;
            }
            if (fin == 's') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                t->saved = wsap_vtcore_cursor_save(t);
                p = s + 1;
                goto tail;
            }
            if (fin == 'u') {
                t->cur_fg = fg;
                t->cur_bg = bg;
                sc_restore_cursor(t, &t->saved);
                fg = t->cur_fg;
                bg = t->cur_bg;
                flags_ok = false;
                p = s + 1;
                goto tail;
            }
            goto params;
        }

        /* The sequences without a parameter. */
        if (lead == 'm') {
            reset_sgr(t);
            fg = t->cur_fg;
            bg = t->cur_bg;
            flags_pad = wsap_vtcore_flags_pad(t);
            flags_ok = true;
            p++;
            goto tail;
        }
        if (lead == 'H') {
            /* Origin mode and left and right margins move the home position. */
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0)) {
                p = esc;
                goto done;
            }
            sc_move(t, 0, 0);
            p++;
            goto tail;
        }
        if (lead == 'C') {
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0)) {
                p = esc;
                goto done;
            }
            sc_move(t, t->cursor_x < cols - 1 ? t->cursor_x + 1 : cols - 1, t->cursor_y);
            p++;
            goto tail;
        }
        if (lead == 'D') {
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0)) {
                p = esc;
                goto done;
            }
            sc_move(t, t->cursor_x > 0 ? t->cursor_x - 1 : 0, t->cursor_y);
            p++;
            goto tail;
        }
        if (lead == 'B') {
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0) ||
                t->scroll_top != 0 || t->scroll_bottom != rows - 1) {
                p = esc;
                goto done;
            }
            sc_move(t, t->cursor_x, t->cursor_y < rows - 1 ? t->cursor_y + 1 : rows - 1);
            p++;
            goto tail;
        }
        if (lead == 'A') {
            if (WSAP_VTCORE_EXPECT(t->origin_mode || t->lr_margin_mode, 0) ||
                t->scroll_top != 0 || t->scroll_bottom != rows - 1) {
                p = esc;
                goto done;
            }
            sc_move(t, t->cursor_x, t->cursor_y > 0 ? t->cursor_y - 1 : 0);
            p++;
            goto tail;
        }
        if (lead == 'J') {
            t->cur_fg = fg;
            t->cur_bg = bg;
            wsap_vtcore_screen_erase(t, 0);
            p++;
            goto tail;
        }
        if (lead == 'K') {
            t->cur_fg = fg;
            t->cur_bg = bg;
            wsap_vtcore_screen_erase_line(t, 0);
            p++;
            goto tail;
        }
        if (lead == '?') {
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
                    goto done;
                }
                p = q + 1;
                goto tail;
            }
        }
        p = esc;
        goto done;

    params: {
        /* The full parameter reader. It takes long counts, three or more
         * parameters, and the final bytes that the fast forms above do not. */
        if (t->origin_mode || t->lr_margin_mode ||
            !((*p >= '0' && *p <= '9') || *p == ';' ||
              *p == 'A' || *p == 'B' || *p == 'C' || *p == 'D' ||
              *p == 'E' || *p == 'F' || *p == 'G' || *p == '`' ||
              *p == 'a' || *p == 'd' || *p == 'e' ||
              *p == 'H' || *p == 'f' || *p == 'm' ||
              *p == 'J' || *p == 'K' || *p == 'X' ||
              *p == 'P' || *p == '@' || *p == 's' || *p == 'u')) {
            p = esc;
            goto done;
        }
        int pv[12];
        int v;
        const char *q = scan_count(p, end, &v);
        if (WSAP_VTCORE_EXPECT(q >= end, 0)) {
            p = esc;
            goto done;
        }
        pv[0] = v;
        int nparams = 1;
        while (*q == ';') {
            if (WSAP_VTCORE_EXPECT(nparams == 11, 0))
                break;
            const char *next = scan_count(q + 1, end, &v);
            if (WSAP_VTCORE_EXPECT(next >= end, 0)) {
                p = esc;
                goto done;
            }
            pv[nparams++] = v;
            q = next;
        }
        int n = pv[0] > 0 ? pv[0] : 1;
        int nx = t->cursor_x, ny = t->cursor_y;
        switch (*q) {
        case 'A': ny -= n; goto vmove;
        case 'B': case 'e': ny += n; goto vmove;
        case 'E': ny += n; nx = 0; goto vmove;
        case 'F': ny -= n; nx = 0; goto vmove;
        vmove:
            /* Relative motion stops at a margin when the cursor starts inside it. */
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
            nx = nparams > 1 && pv[1] > 0 ? pv[1] - 1 : 0;
            goto move;
        move:
            sc_move(t, nx, ny);
            p = q + 1;
            break;
        case 'm':
            for (int j = 0; j < nparams; j++) {
                int code = pv[j];
                switch (code) {
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
                    uint32_t *slot = code == 38 ? &fg : &bg;
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
                    fg = wsap_vtcore_color_index((uint8_t)(code - 30));
                    break;
                case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                    bg = wsap_vtcore_color_index((uint8_t)(code - 40));
                    break;
                case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                    fg = wsap_vtcore_color_index((uint8_t)(code - 90 + 8));
                    break;
                case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                    bg = wsap_vtcore_color_index((uint8_t)(code - 100 + 8));
                    break;
                default:
                    break;
                }
            }
            flags_ok = false;
            p = q + 1;
            break;
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
            break;
        default:
            p = esc;
            goto done;
        }
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
