#ifndef WSAP_TERM_PEN_H
#define WSAP_TERM_PEN_H

#include "compat.h"
#include "wsap_vtcore.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PEN_ATTR_BOLD     (1u << 0)
#define PEN_ATTR_FAINT    (1u << 1)
#define PEN_ATTR_ITALIC   (1u << 2)
#define PEN_ATTR_STRIKE   (1u << 3)
#define PEN_ATTR_OVERLINE (1u << 4)
#define PEN_ATTR_INVERSE  (1u << 5)

struct wsap_vtcore_pen {
    uint32_t fg;
    uint32_t bg;
    uint32_t ul;
    uint8_t underline;
    union {
        struct {
            bool bold : 1;
            bool faint : 1;
            bool italic : 1;
            bool strike : 1;
            bool overline : 1;
            bool inverse : 1;
        };
        uint8_t attrs;
    };
};

struct pen_attr {
    uint32_t fg;
    uint32_t bg;
    uint16_t flags;
    uint16_t link;
};

struct text_pen {
    uint32_t fg;
    uint32_t bg;
    uint32_t sl;
};

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
    p->attrs = 0;
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

enum wsap_vtcore_sgr_act {
    SGR_ACT_NOP = 0,
    SGR_ACT_RESET,
    SGR_ACT_ATTR,
    SGR_ACT_UL_STYLE,
    SGR_ACT_UL_VAL,
    SGR_ACT_FG_IDX,
    SGR_ACT_BG_IDX,
    SGR_ACT_FG_DEF,
    SGR_ACT_BG_DEF,
    SGR_ACT_UL_DEF,
    SGR_ACT_EXT_FG,
    SGR_ACT_EXT_BG,
    SGR_ACT_EXT_UL,
};

struct wsap_vtcore_sgr_entry {
    uint8_t act;
    uint8_t clr_mask;
    uint8_t set_mask;
    uint8_t val;
};

/* Table maps SGR codes 0 through 109 to actions.
 * It provides O(1) dispatch and avoids branch ladders. */
static const struct wsap_vtcore_sgr_entry wsap_vtcore_sgr_actions[110] = {
    [0] = { SGR_ACT_RESET, 0, 0, 0 },
    [1] = { SGR_ACT_ATTR, 0, PEN_ATTR_BOLD, 0 },
    [2] = { SGR_ACT_ATTR, 0, PEN_ATTR_FAINT, 0 },
    [3] = { SGR_ACT_ATTR, 0, PEN_ATTR_ITALIC, 0 },
    [4] = { SGR_ACT_UL_STYLE, 0, 0, 1 },
    [7] = { SGR_ACT_ATTR, 0, PEN_ATTR_INVERSE, 0 },
    [9] = { SGR_ACT_ATTR, 0, PEN_ATTR_STRIKE, 0 },
    [21] = { SGR_ACT_UL_VAL, 0, 0, 2 },
    [22] = { SGR_ACT_ATTR, PEN_ATTR_BOLD | PEN_ATTR_FAINT, 0, 0 },
    [23] = { SGR_ACT_ATTR, PEN_ATTR_ITALIC, 0, 0 },
    [24] = { SGR_ACT_UL_VAL, 0, 0, 0 },
    [27] = { SGR_ACT_ATTR, PEN_ATTR_INVERSE, 0, 0 },
    [29] = { SGR_ACT_ATTR, PEN_ATTR_STRIKE, 0, 0 },

    [30] = { SGR_ACT_FG_IDX, 0, 0, 0 },
    [31] = { SGR_ACT_FG_IDX, 0, 0, 1 },
    [32] = { SGR_ACT_FG_IDX, 0, 0, 2 },
    [33] = { SGR_ACT_FG_IDX, 0, 0, 3 },
    [34] = { SGR_ACT_FG_IDX, 0, 0, 4 },
    [35] = { SGR_ACT_FG_IDX, 0, 0, 5 },
    [36] = { SGR_ACT_FG_IDX, 0, 0, 6 },
    [37] = { SGR_ACT_FG_IDX, 0, 0, 7 },
    [38] = { SGR_ACT_EXT_FG, 0, 0, 0 },
    [39] = { SGR_ACT_FG_DEF, 0, 0, 0 },

    [40] = { SGR_ACT_BG_IDX, 0, 0, 0 },
    [41] = { SGR_ACT_BG_IDX, 0, 0, 1 },
    [42] = { SGR_ACT_BG_IDX, 0, 0, 2 },
    [43] = { SGR_ACT_BG_IDX, 0, 0, 3 },
    [44] = { SGR_ACT_BG_IDX, 0, 0, 4 },
    [45] = { SGR_ACT_BG_IDX, 0, 0, 5 },
    [46] = { SGR_ACT_BG_IDX, 0, 0, 6 },
    [47] = { SGR_ACT_BG_IDX, 0, 0, 7 },
    [48] = { SGR_ACT_EXT_BG, 0, 0, 0 },
    [49] = { SGR_ACT_BG_DEF, 0, 0, 0 },

    [53] = { SGR_ACT_ATTR, 0, PEN_ATTR_OVERLINE, 0 },
    [55] = { SGR_ACT_ATTR, PEN_ATTR_OVERLINE, 0, 0 },
    [58] = { SGR_ACT_EXT_UL, 0, 0, 0 },
    [59] = { SGR_ACT_UL_DEF, 0, 0, 0 },

    [90] = { SGR_ACT_FG_IDX, 0, 0, 8 },
    [91] = { SGR_ACT_FG_IDX, 0, 0, 9 },
    [92] = { SGR_ACT_FG_IDX, 0, 0, 10 },
    [93] = { SGR_ACT_FG_IDX, 0, 0, 11 },
    [94] = { SGR_ACT_FG_IDX, 0, 0, 12 },
    [95] = { SGR_ACT_FG_IDX, 0, 0, 13 },
    [96] = { SGR_ACT_FG_IDX, 0, 0, 14 },
    [97] = { SGR_ACT_FG_IDX, 0, 0, 15 },

    [100] = { SGR_ACT_BG_IDX, 0, 0, 8 },
    [101] = { SGR_ACT_BG_IDX, 0, 0, 9 },
    [102] = { SGR_ACT_BG_IDX, 0, 0, 10 },
    [103] = { SGR_ACT_BG_IDX, 0, 0, 11 },
    [104] = { SGR_ACT_BG_IDX, 0, 0, 12 },
    [105] = { SGR_ACT_BG_IDX, 0, 0, 13 },
    [106] = { SGR_ACT_BG_IDX, 0, 0, 14 },
    [107] = { SGR_ACT_BG_IDX, 0, 0, 15 },
};

static inline void sgr_apply(struct wsap_vtcore_pen *pen, const int *params, int nparams, uint16_t subparams)
{
    if (nparams == 0) {
        pen_reset(pen);
        return;
    }
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if ((unsigned)p >= 110)
            continue;
        const struct wsap_vtcore_sgr_entry *entry = &wsap_vtcore_sgr_actions[p];
        switch (entry->act) {
        case SGR_ACT_NOP: break;
        case SGR_ACT_ATTR:
            pen->attrs = (uint8_t)((pen->attrs & ~entry->clr_mask) | entry->set_mask);
            break;
        case SGR_ACT_FG_IDX:
            pen->fg = wsap_vtcore_color_index(entry->val);
            break;
        case SGR_ACT_BG_IDX:
            pen->bg = wsap_vtcore_color_index(entry->val);
            break;
        case SGR_ACT_RESET:
            pen_reset(pen);
            break;
        case SGR_ACT_UL_VAL:
            pen->underline = entry->val;
            break;
        case SGR_ACT_UL_STYLE: {
            int style = 1;
            if (i + 1 < nparams && (subparams & (1u << (i + 1))))
                style = params[++i];
            pen->underline = (uint8_t)(style <= 5 ? style : 1);
            break;
        }
        case SGR_ACT_FG_DEF: pen->fg = WSAP_VTCORE_COLOR_FG; break;
        case SGR_ACT_BG_DEF: pen->bg = WSAP_VTCORE_COLOR_BG; break;
        case SGR_ACT_UL_DEF: pen->ul = 0; break;
        case SGR_ACT_EXT_FG: {
            uint32_t c; bool ok;
            i += sgr_extended_color(params, nparams, subparams, i, &c, &ok);
            if (ok) pen->fg = c;
            break;
        }
        case SGR_ACT_EXT_BG: {
            uint32_t c; bool ok;
            i += sgr_extended_color(params, nparams, subparams, i, &c, &ok);
            if (ok) pen->bg = c;
            break;
        }
        case SGR_ACT_EXT_UL: {
            uint32_t c; bool ok;
            i += sgr_extended_color(params, nparams, subparams, i, &c, &ok);
            if (ok) pen->ul = c;
            break;
        }
        }
    }
}

#endif
