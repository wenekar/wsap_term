#ifndef WSAP_TERM_PEN_H
#define WSAP_TERM_PEN_H

#include "compat.h"
#include "wsap_vtcore.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct wsap_vtcore_pen {
    uint32_t fg;
    uint32_t bg;
    uint32_t ul;
    uint8_t underline;
    bool bold : 1;
    bool faint : 1;
    bool italic : 1;
    bool strike : 1;
    bool overline : 1;
    bool inverse : 1;
};

/* The pen of a stored cell. A cell has no underline color, so neither has
 * this. */
struct pen_attr {
    uint32_t fg;
    uint32_t bg;
    uint16_t flags;
    uint16_t link;
};

/* A pen as three words: fg, bg, and flags | link << 16 (the layout of
 * pen_attr.flags and .link). The hot path reads and writes whole words, so
 * a store is never read back through a wider load. */
struct text_pen {
    uint32_t fg;
    uint32_t bg;
    uint32_t sl;
};

static inline struct text_pen text_pen(uint32_t fg, uint32_t bg, uint32_t sl)
{
    return (struct text_pen){ fg, bg, sl };
}

static inline uint32_t pen_sl(const struct pen_attr *p)
{
    uint32_t sl;
    memcpy(&sl, &p->flags, 4);
    return sl;
}

static inline struct text_pen text_pen_from_attr(const struct pen_attr *p)
{
    return (struct text_pen){ p->fg, p->bg, pen_sl(p) };
}

static inline void pen_set(struct pen_attr *p, uint32_t fg, uint32_t bg, uint32_t sl)
{
    p->fg = fg;
    p->bg = bg;
    memcpy(&p->flags, &sl, 4);
}

static inline void pen_set_text(struct pen_attr *p, struct text_pen pen)
{
    p->fg = pen.fg;
    p->bg = pen.bg;
    memcpy(&p->flags, &pen.sl, 4);
}

static inline struct pen_attr pen_attr_default(void)
{
    return (struct pen_attr){ .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG };
}

static inline bool pen_attr_is_default(const struct pen_attr *a)
{
    return a->fg == WSAP_VTCORE_COLOR_FG && a->bg == WSAP_VTCORE_COLOR_BG && a->flags == 0 && a->link == 0;
}

static inline bool pen_attr_equal(const struct pen_attr *a, const struct pen_attr *b)
{
    return a->fg == b->fg && a->bg == b->bg && a->flags == b->flags && a->link == b->link;
}

static inline uint32_t wsap_vtcore_rgb(int r, int g, int b)
{
    return wsap_vtcore_color_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static inline void pen_reset(struct wsap_vtcore_pen *p)
{
    p->fg = WSAP_VTCORE_COLOR_FG;
    p->bg = WSAP_VTCORE_COLOR_BG;
    p->ul = 0;
    p->underline = 0;
    p->bold = false;
    p->faint = false;
    p->italic = false;
    p->strike = false;
    p->overline = false;
    p->inverse = false;
}

static inline int sgr_extended_color(const int *params, int nparams, uint16_t subparams, int i,
                                     uint32_t *color, bool *assigned)
{
    *assigned = false;
    if (i + 1 >= nparams)
        return 0;
    if (i + 2 < nparams && params[i + 1] == 5) {
        int index = params[i + 2];
        if (index >= 0 && index <= 255) {
            *color = wsap_vtcore_color_index(index);
            *assigned = true;
        }
        return 2;
    }
    if (params[i + 1] != 2)
        return 0;
    int colon = 0;
    int k = i + 2;
    if (k < 16 && k < nparams) {
        uint32_t shifted = (~(uint32_t)subparams) >> k;
        colon = wsap_vtcore_ctz32(shifted);
        int max_colon = (nparams < 16 ? nparams : 16) - k;
        if (colon > max_colon)
            colon = max_colon;
    }
    int skip = colon >= 4 ? 1 : 0;
    if (i + 4 + skip >= nparams)
        return 0;
    *color = wsap_vtcore_rgb(params[i + 2 + skip], params[i + 3 + skip], params[i + 4 + skip]);
    *assigned = true;
    return 4 + skip;
}

static inline bool pen_set_standard_color(struct wsap_vtcore_pen *p, int code)
{
    if ((unsigned)(code - 30) < 8)
        p->fg = wsap_vtcore_color_index(code - 30);
    else if ((unsigned)(code - 40) < 8)
        p->bg = wsap_vtcore_color_index(code - 40);
    else if ((unsigned)(code - 90) < 8)
        p->fg = wsap_vtcore_color_index(code - 90 + 8);
    else if ((unsigned)(code - 100) < 8)
        p->bg = wsap_vtcore_color_index(code - 100 + 8);
    else
        return false;
    return true;
}

static inline void sgr_apply(struct wsap_vtcore_pen *pen, const int *params, int nparams, uint16_t subparams)
{
    if (nparams == 0) {
        pen_reset(pen);
        return;
    }
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        switch (p) {
        case 0: pen_reset(pen); break;
        case 1: pen->bold = true; break;
        case 2: pen->faint = true; break;
        case 3: pen->italic = true; break;
        case 4: {
            int style = 1;
            if (i + 1 < nparams && (subparams & (1u << (i + 1))))
                style = params[++i];
            pen->underline = (uint8_t)(style <= 5 ? style : 1);
            break;
        }
        case 7: pen->inverse = true; break;
        case 9: pen->strike = true; break;
        case 21: pen->underline = 2; break;
        case 22: pen->bold = false; pen->faint = false; break;
        case 23: pen->italic = false; break;
        case 24: pen->underline = 0; break;
        case 27: pen->inverse = false; break;
        case 29: pen->strike = false; break;
        case 38: {
            uint32_t c; bool ok;
            i += sgr_extended_color(params, nparams, subparams, i, &c, &ok);
            if (ok) pen->fg = c;
            break;
        }
        case 39: pen->fg = WSAP_VTCORE_COLOR_FG; break;
        case 48: {
            uint32_t c; bool ok;
            i += sgr_extended_color(params, nparams, subparams, i, &c, &ok);
            if (ok) pen->bg = c;
            break;
        }
        case 49: pen->bg = WSAP_VTCORE_COLOR_BG; break;
        case 53: pen->overline = true; break;
        case 55: pen->overline = false; break;
        case 58: {
            uint32_t c; bool ok;
            i += sgr_extended_color(params, nparams, subparams, i, &c, &ok);
            if (ok) pen->ul = c;
            break;
        }
        case 59: pen->ul = 0; break;
        default:
            pen_set_standard_color(pen, p);
            break;
        }
    }
}

#endif
