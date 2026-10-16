#include "css.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws_and_comments(const char *p)
{
    while (*p && (isspace((unsigned char)*p) || *p == '/')) {
        if (*p == '/' && *(p + 1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p + 1) == '/')) p++;
            if (*p) p += 2;
        } else {
            p++;
        }
    }
    return p;
}

static bool parse_hex_color(const char *s, uint32_t *out)
{
    if (*s != '#') return false;
    s++;
    size_t len = 0;
    while (isxdigit((unsigned char)s[len])) len++;
    if (s[len] != '\0' || (len != 3 && len != 6 && len != 8)) return false;
    unsigned long v = strtoul(s, NULL, 16);
    if (len == 6) *out = 0xFF000000u | (uint32_t)v;
    else if (len == 8) {
        uint32_t r = (v >> 24) & 0xFF, g = (v >> 16) & 0xFF, b = (v >> 8) & 0xFF, a = v & 0xFF;
        *out = (a << 24) | (r << 16) | (g << 8) | b;
    } else {
        uint32_t r = ((v >> 8) & 0xF) * 0x11, g = ((v >> 4) & 0xF) * 0x11, b = (v & 0xF) * 0x11;
        *out = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    return true;
}

static bool parse_named_color(const char *s, uint32_t *out)
{
    switch (s[0]) {
    case 'b':
        if (!strcmp(s, "black")) { *out = 0xFF000000u; return true; }
        if (!strcmp(s, "blue"))  { *out = 0xFF0000FFu; return true; }
        break;
    case 'd':
        if (!strcmp(s, "darkgray") || !strcmp(s, "darkgrey")) { *out = 0xFFA9A9A9u; return true; }
        if (!strcmp(s, "dimgray")  || !strcmp(s, "dimgrey"))  { *out = 0xFF696969u; return true; }
        break;
    case 'g':
        if (!strcmp(s, "green")) { *out = 0xFF008000u; return true; }
        if (!strcmp(s, "gray") || !strcmp(s, "grey")) { *out = 0xFF808080u; return true; }
        break;
    case 'l':
        if (!strcmp(s, "lightgray") || !strcmp(s, "lightgrey")) { *out = 0xFFD3D3D3u; return true; }
        break;
    case 'r':
        if (!strcmp(s, "red")) { *out = 0xFFFF0000u; return true; }
        break;
    case 's':
        if (!strcmp(s, "silver")) { *out = 0xFFC0C0C0u; return true; }
        break;
    case 't':
        if (!strcmp(s, "transparent")) { *out = 0x00000000u; return true; }
        break;
    case 'w':
        if (!strcmp(s, "white")) { *out = 0xFFFFFFFFu; return true; }
        break;
    default:
        break;
    }
    return false;
}

static bool parse_color(const char *s, uint32_t *out)
{
    return parse_hex_color(s, out) || parse_named_color(s, out);
}

static struct css_value parse_value(const char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (!strncmp(s, "auto", 4) && (s[4] == '\0' || isspace((unsigned char)s[4]) || s[4] == ';'))
        return (struct css_value){ .unit = CSS_UNIT_AUTO };

    if (!strcmp(s, "grow"))
        return (struct css_value){ .unit = CSS_UNIT_GROW };
    if (!strcmp(s, "fit"))
        return (struct css_value){ .unit = CSS_UNIT_AUTO };

    char *end = NULL;
    float val = strtof(s, &end);
    if (!end || end == s || !isfinite(val))
        return (struct css_value){ .unit = CSS_UNIT_NONE };

    /* A space right after the number ends the token — unitless means px. */
    if (isspace((unsigned char)*end))
        return (struct css_value){ .val = val, .unit = CSS_UNIT_PX };

    static const struct {
        const char *name;
        uint8_t len;
        enum css_unit_type unit;
    } units[] = {
        { "%",   1, CSS_UNIT_PERCENT },
        { "rem", 3, CSS_UNIT_REM },
        { "em",  2, CSS_UNIT_EM },
        { "ch",  2, CSS_UNIT_CH },
        { "px",  2, CSS_UNIT_PX },
    };

    enum css_unit_type u = CSS_UNIT_NONE;
    if (*end == '\0' || *end == ';') {
        u = CSS_UNIT_PX;
    } else {
        for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
            if (!strncmp(end, units[i].name, units[i].len)) {
                char next = end[units[i].len];
                if (next == '\0' || next == ';' || isspace((unsigned char)next)) {
                    u = units[i].unit;
                    end += units[i].len;
                    break;
                }
            }
        }
    }

    if (u == CSS_UNIT_NONE)
        return (struct css_value){ .unit = CSS_UNIT_NONE };

    return (struct css_value){ .val = val, .unit = u };
}

float css_value_resolve(struct css_value v, float parent_dim, float auto_val,
                        const struct css_scale_context *ctx)
{
    if (!isfinite(v.val) || !isfinite(parent_dim) || !isfinite(auto_val))
        return isfinite(auto_val) ? auto_val : 0.0f;

    float dpi = (ctx && isfinite(ctx->dpi_scale) && ctx->dpi_scale > 0.0f)
        ? ctx->dpi_scale : 1.0f;
    float ch = (ctx && isfinite(ctx->cell_h) && ctx->cell_h > 0.0f)
        ? ctx->cell_h : 16.0f * dpi;
    float cw = (ctx && isfinite(ctx->cell_w) && ctx->cell_w > 0.0f)
        ? ctx->cell_w : 8.0f * dpi;
    float result;
    switch (v.unit) {
    case CSS_UNIT_PX:      result = v.val * dpi; break;
    case CSS_UNIT_PERCENT:
        /* Percentages only resolve against a definite basis; an
         * INT_MAX-ish offer (unbounded measure, scroll content height)
         * falls back to the auto value. */
        if (parent_dim >= 1e9f)
            return auto_val;
        result = (v.val / 100.0f) * parent_dim;
        break;
    case CSS_UNIT_EM:
    case CSS_UNIT_REM:     result = v.val * ch; break;
    case CSS_UNIT_CH:      result = v.val * cw; break;
    case CSS_UNIT_AUTO:
    case CSS_UNIT_NONE:
    default:               return auto_val;
    }
    return isfinite(result) ? result : auto_val;
}

static void parse_insets(const char *s, struct css_box *out)
{
    struct css_value vals[4] = {0};
    int count = 0;
    while (*s && count < 4) {
        s = skip_ws_and_comments(s);
        if (!*s || *s == ';') break;
        vals[count++] = parse_value(s);
        while (*s && !isspace((unsigned char)*s) && *s != ';') s++;
    }
    switch (count) {
    case 1:
        out->v[0] = vals[0];
        out->v[1] = vals[0];
        out->v[2] = vals[0];
        out->v[3] = vals[0];
        break;
    case 2:
        out->v[0] = vals[0];
        out->v[1] = vals[1];
        out->v[2] = vals[0];
        out->v[3] = vals[1];
        break;
    case 3:
        out->v[0] = vals[0];
        out->v[1] = vals[1];
        out->v[2] = vals[2];
        out->v[3] = vals[1];
        break;
    case 4:
        out->v[0] = vals[0];
        out->v[1] = vals[1];
        out->v[2] = vals[2];
        out->v[3] = vals[3];
        break;
    default:
        break;
    }
}

static bool parse_box_shadow(const char *s, struct css_style *out)
{
    if (!strcmp(s, "none")) {
        out->has_shadow = false;
        return true;
    }
    struct css_value lengths[3] = {
        { .unit = CSS_UNIT_NONE }, { .unit = CSS_UNIT_NONE },
        { .unit = CSS_UNIT_NONE },
    };
    uint32_t color = 0x80000000u;
    int n_len = 0;
    while (*s) {
        s = skip_ws_and_comments(s);
        if (!*s || *s == ';') break;
        const char *start = s;
        while (*s && !isspace((unsigned char)*s) && *s != ';') s++;
        char tok[32];
        size_t len = (size_t)(s - start);
        if (len >= sizeof(tok)) return false;
        memcpy(tok, start, len);
        tok[len] = '\0';
        uint32_t c;
        if (parse_color(tok, &c)) {
            color = c;
        } else if (n_len < 3) {
            struct css_value v = parse_value(tok);
            if (v.unit == CSS_UNIT_NONE) return false;
            lengths[n_len++] = v;
        } else {
            return false;
        }
    }
    if (n_len < 2)
        return false;
    out->shadow_x = lengths[0];
    out->shadow_y = lengths[1];
    out->shadow_blur = lengths[2];
    out->shadow_color = color;
    out->has_shadow = true;
    return true;
}

enum prop_kind { P_VAL, P_INSETS, P_COLOR, P_FLOAT, P_FLEX_DIR, P_ALIGN, P_JUSTIFY, P_CURSOR, P_DURATION, P_SHADOW };

struct prop_info {
    const char *name;
    uint16_t offset;
    uint16_t has_offset;
    uint8_t kind;
    uint8_t flag_idx;
    uint8_t pad_[2];
};

#define PROP(n, k, m, id) { .name = (n), .offset = offsetof(struct css_style, m), .has_offset = 0, .kind = (uint8_t)(k), .flag_idx = (id) }
#define PROP_COLOR(n, m, has_m, id) { .name = (n), .offset = offsetof(struct css_style, m), .has_offset = offsetof(struct css_style, has_m), .kind = P_COLOR, .flag_idx = (id) }
#define PROP_FLOAT(n, m, has_m, id) { .name = (n), .offset = offsetof(struct css_style, m), .has_offset = offsetof(struct css_style, has_m), .kind = P_FLOAT, .flag_idx = (id) }

static const struct prop_info props[] = {
    PROP("width",            P_VAL,      width,          1),
    PROP("height",           P_VAL,      height,         2),
    PROP("min-width",        P_VAL,      min_width,      3),
    PROP("min-height",       P_VAL,      min_height,     4),
    PROP("max-width",        P_VAL,      max_width,     22),
    PROP("max-height",       P_VAL,      max_height,    23),
    PROP("gap",              P_VAL,      gap,            5),
    PROP("border-radius",    P_VAL,      border_radius,  6),
    PROP("border-width",     P_VAL,      border_width,   7),
    PROP("border",           P_VAL,      border_width,   7),
    PROP("padding",          P_INSETS,   padding,        8),
    PROP("margin",           P_INSETS,   margin,         9),
    PROP_COLOR("background-color", bg_color, has_bg_color, 10),
    PROP_COLOR("background",       bg_color, has_bg_color, 10),
    PROP_COLOR("color",            text_color, has_text_color, 11),
    PROP_COLOR("border-color",     border_color, has_border_color, 12),
    PROP_FLOAT("opacity",    opacity,    has_opacity,   14),
    PROP("flex-direction",   P_FLEX_DIR, flex_direction,15),
    PROP("align-items",      P_ALIGN,    align_items,   16),
    PROP("justify-content",  P_JUSTIFY,  justify_content,17),
    PROP("cursor",           P_CURSOR,   cursor,         18),
    PROP("transition-duration", P_DURATION, transition_duration, 19),
    PROP("animation-duration",  P_DURATION, transition_duration, 19),
    PROP("transition",          P_DURATION, transition_duration, 19),
    PROP("box-shadow",       P_SHADOW,   shadow_x,      20),
    PROP_COLOR("ripple-color", ripple_color, has_ripple_color, 21),
    PROP_COLOR("ripple",       ripple_color, has_ripple_color, 21),
};

/* Map flag index to property info. */
static const struct prop_info *const prop_by_flag[32] = {
    [1]  = &props[0],
    [2]  = &props[1],
    [3]  = &props[2],
    [4]  = &props[3],
    [5]  = &props[6],
    [6]  = &props[7],
    [7]  = &props[8],
    [8]  = &props[10],
    [9]  = &props[11],
    [10] = &props[12],
    [11] = &props[14],
    [12] = &props[15],
    [14] = &props[16],
    [15] = &props[17],
    [16] = &props[18],
    [17] = &props[19],
    [18] = &props[20],
    [19] = &props[21],
    [20] = &props[24],
    [21] = &props[25],
    [22] = &props[4],
    [23] = &props[5],
};

static void apply_prop(struct css_rule *r, const char *key, const char *val)
{
    while (isspace((unsigned char)*key)) key++;
    while (isspace((unsigned char)*val)) val++;
    for (size_t i = 0; i < sizeof(props)/sizeof(props[0]); i++) {
        const struct prop_info *p = &props[i];
        if (strcmp(key, p->name) != 0) continue;
        char *dst = (char *)&r->style + p->offset;
        switch (p->kind) {
        case P_VAL: {
            struct css_value v = parse_value(val);
            if (v.unit == CSS_UNIT_NONE) return;
            *(struct css_value *)dst = v;
            break;
        }
        case P_INSETS:   parse_insets(val, (struct css_box *)dst); break;
        case P_COLOR: {
            uint32_t color;
            if (!parse_color(val, &color)) return;
            *(uint32_t *)dst = color;
            if (p->has_offset)
                *(bool *)((char *)&r->style + p->has_offset) = true;
            break;
        }
        case P_FLOAT: {
            char *end = NULL;
            float f = strtof(val, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (!end || end == val || *end != '\0' || !isfinite(f)) return;
            if (p->offset == offsetof(struct css_style, opacity)) {
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
            }
            memcpy(dst, &f, sizeof f);
            if (p->has_offset)
                *(bool *)((char *)&r->style + p->has_offset) = true;
            break;
        }
        case P_FLEX_DIR:
            if (!strcmp(val, "column"))
                *(enum css_flex_direction *)dst = CSS_FLEX_COLUMN;
            else if (!strcmp(val, "row"))
                *(enum css_flex_direction *)dst = CSS_FLEX_ROW;
            else
                return;
            break;
        case P_ALIGN: {
            static const struct { const char *name; enum css_align val; } align_map[] = {
                { "center",     CSS_ALIGN_CENTER },
                { "flex-start", CSS_ALIGN_START },
                { "start",      CSS_ALIGN_START },
                { "flex-end",   CSS_ALIGN_END },
                { "end",        CSS_ALIGN_END },
                { "stretch",    CSS_ALIGN_STRETCH },
            };
            enum css_align align = CSS_ALIGN_STRETCH;
            bool found = false;
            for (size_t k = 0; k < sizeof(align_map)/sizeof(align_map[0]); k++) {
                if (!strcmp(val, align_map[k].name)) {
                    align = align_map[k].val;
                    found = true;
                    break;
                }
            }
            if (!found) return;
            *(enum css_align *)dst = align;
            break;
        }
        case P_JUSTIFY: {
            static const struct { const char *name; enum css_justify val; } justify_map[] = {
                { "center",        CSS_JUSTIFY_CENTER },
                { "flex-end",      CSS_JUSTIFY_END },
                { "end",           CSS_JUSTIFY_END },
                { "flex-start",    CSS_JUSTIFY_START },
                { "start",         CSS_JUSTIFY_START },
            };
            enum css_justify just = CSS_JUSTIFY_START;
            bool found = false;
            for (size_t k = 0; k < sizeof(justify_map)/sizeof(justify_map[0]); k++) {
                if (!strcmp(val, justify_map[k].name)) {
                    just = justify_map[k].val;
                    found = true;
                    break;
                }
            }
            if (!found) return;
            *(enum css_justify *)dst = just;
            break;
        }
        case P_CURSOR: {
            static const struct { const char *name; enum ui_cursor val; } cursor_map[] = {
                { "auto",      UI_CURSOR_AUTO },
                { "default",   UI_CURSOR_DEFAULT },
                { "arrow",     UI_CURSOR_ARROW },
                { "text",      UI_CURSOR_TEXT },
                { "ibeam",     UI_CURSOR_TEXT },
                { "pointer",   UI_CURSOR_POINTER },
                { "clickable", UI_CURSOR_CLICKABLE },
                { "link",      UI_CURSOR_LINK },
                { "hand",      UI_CURSOR_POINTER },
                { "none",      UI_CURSOR_NONE },
            };
            enum ui_cursor cur = UI_CURSOR_AUTO;
            bool found = false;
            for (size_t k = 0; k < sizeof(cursor_map)/sizeof(cursor_map[0]); k++) {
                if (!strcmp(val, cursor_map[k].name)) {
                    cur = cursor_map[k].val;
                    found = true;
                    break;
                }
            }
            if (!found) return;
            *(enum ui_cursor *)dst = cur;
            break;
        }
        case P_SHADOW:
            if (!parse_box_shadow(val, &r->style)) return;
            r->set_mask |= (1ULL << p->flag_idx);
            return;
        case P_DURATION: {
            const char *s = val;
            while (*s && !isdigit((unsigned char)*s) && *s != '.')
                s++;
            char *end = NULL;
            float f = strtof(s, &end);
            if (end && end != s && isfinite(f)) {
                while (end && isspace((unsigned char)*end)) end++;
                if (end && !strncmp(end, "s", 1) && strncmp(end, "ms", 2) != 0) {
                    f *= 1000.0f;
                }
                if (f < 0.0f) f = 0.0f;
                if (f > 3600000.0f) f = 3600000.0f;
                memcpy(dst, &f, sizeof(f));
                r->set_mask |= (1ULL << p->flag_idx);
                r->style.has_transition_duration = true;
            }
            return;
        }
        }
        r->set_mask |= (1ULL << p->flag_idx);
        return;
    }
    fprintf(stderr, "css: unknown property '%s'\n", key);
}

static char *dup_range(const char *s, size_t len)
{
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static bool parse_single_selector(struct css_rule *r, const char *s, size_t len)
{
    while (len && isspace((unsigned char)*s)) { s++; len--; }
    while (len && isspace((unsigned char)s[len - 1])) len--;
    r->selector = dup_range(s, len);
    if (!r->selector)
        return false;

    const char *comp = r->selector;
    for (const char *p = r->selector; *p; p++)
        if (isspace((unsigned char)*p)) comp = p + 1;
    r->last_off = (size_t)(comp - r->selector);

    const char *pseudo = strchr(comp, ':');
    if (pseudo) {
        pseudo++;
        if (!strcmp(pseudo, "hover"))         r->pseudo = CSS_PSEUDO_HOVER;
        else if (!strcmp(pseudo, "active"))   r->pseudo = CSS_PSEUDO_ACTIVE;
        else if (!strcmp(pseudo, "focus"))    r->pseudo = CSS_PSEUDO_FOCUS;
        else if (!strcmp(pseudo, "disabled")) r->pseudo = CSS_PSEUDO_DISABLED;
        else if (!strcmp(pseudo, "checked"))  r->pseudo = CSS_PSEUDO_CHECKED;
        else fprintf(stderr, "css: unknown pseudo-class ':%s' in selector '%s'\n",
                     pseudo, r->selector);
    }
    if (*comp == '.') {
        r->is_class = true;
        const char *stop = pseudo ? pseudo - 1 : comp + strlen(comp);
        int n = 0;
        for (const char *c = comp; c < stop; c++)
            if (*c == '.') n++;
        r->class_rank = n;
    } else if (*comp == '#') {
        r->is_id = true;
    }
    return true;
}

static bool sheet_reserve(struct css_sheet *sheet, int extra)
{
    if (sheet->rule_count + extra <= sheet->rule_cap)
        return true;
    int cap = sheet->rule_cap ? sheet->rule_cap : 32;
    while (cap < sheet->rule_count + extra)
        cap *= 2;
    struct css_rule *rules = realloc(sheet->rules, (size_t)cap * sizeof(*rules));
    if (!rules)
        return false;
    sheet->rules = rules;
    sheet->rule_cap = cap;
    return true;
}

static bool sheet_push(struct css_sheet *sheet, const struct css_rule *proto,
                       const char *sel, size_t len)
{
    if (!sheet_reserve(sheet, 1))
        return false;
    struct css_rule *r = &sheet->rules[sheet->rule_count];
    *r = *proto;
    if (!parse_single_selector(r, sel, len))
        return false;
    sheet->rule_count++;
    return true;
}

/* Index over the rules. Each rule has a position in apply order: tier
 * (tag, class, id), then class rank, then source order. A rule with a
 * usable key sits in the group of that key. A lookup visits only the
 * groups its own tag, id, and class tokens name, plus the keyless group. */

enum key_kind {
    KEY_ANY = 0,
    KEY_TAG,
    KEY_CLASS,
    KEY_ID,
};

struct css_key {
    const char *text;
    uint32_t len;
    uint32_t start;
    uint32_t count;
    uint8_t kind;
};

struct css_index {
    uint32_t *order;
    uint32_t *items;
    struct css_key *keys;
    uint32_t key_count;
};

struct apply_slot {
    uint32_t tier;
    uint32_t rank;
    uint32_t rule;
};

struct key_slot {
    const char *text;
    uint32_t len;
    uint32_t pos;
    uint8_t kind;
};

static int cmp_u32(uint32_t a, uint32_t b)
{
    return (a > b) - (a < b);
}

static int cmp_apply_slot(const void *pa, const void *pb)
{
    const struct apply_slot *a = pa, *b = pb;
    if (a->tier != b->tier) return cmp_u32(a->tier, b->tier);
    if (a->rank != b->rank) return cmp_u32(a->rank, b->rank);
    return cmp_u32(a->rule, b->rule);
}

static int cmp_key_text(uint8_t ka, const char *ta, uint32_t la,
                        uint8_t kb, const char *tb, uint32_t lb)
{
    if (ka != kb) return cmp_u32(ka, kb);
    uint32_t m = la < lb ? la : lb;
    if (m) {
        int c = memcmp(ta, tb, m);
        if (c) return c < 0 ? -1 : 1;
    }
    return cmp_u32(la, lb);
}

static int cmp_key_slot(const void *pa, const void *pb)
{
    const struct key_slot *a = pa, *b = pb;
    int c = cmp_key_text(a->kind, a->text, a->len, b->kind, b->text, b->len);
    return c ? c : cmp_u32(a->pos, b->pos);
}

static uint32_t rule_tier(const struct css_rule *r)
{
    return r->is_id ? 2 : r->is_class ? 1 : 0;
}

static uint32_t rule_rank(const struct css_rule *r)
{
    return r->is_class ? (uint32_t)r->class_rank : 1;
}

/* The key comes from the last compound, cut at its pseudo-class. An empty
 * key stays KEY_ANY, so such a rule is checked on every lookup. */
static struct key_slot rule_key(const struct css_rule *r, uint32_t pos)
{
    struct key_slot k = { .text = NULL, .len = 0, .pos = pos, .kind = KEY_ANY };
    const char *comp = r->selector + r->last_off;
    const char *colon = strchr(comp, ':');
    size_t clen = colon ? (size_t)(colon - comp) : strlen(comp);
    if (!clen)
        return k;

    const char *text = comp;
    size_t len = clen;
    uint8_t kind = KEY_TAG;
    if (r->is_class) {
        const char *end = comp + clen;
        text = comp + 1;
        const char *dot = memchr(text, '.', (size_t)(end - text));
        len = (size_t)((dot ? dot : end) - text);
        kind = KEY_CLASS;
    } else if (r->is_id) {
        text = comp + 1;
        len = clen - 1;
        kind = KEY_ID;
    }
    if (!len)
        return k;
    k.text = text;
    k.len = (uint32_t)len;
    k.kind = kind;
    return k;
}

static void css_index_free(struct css_index *ix)
{
    if (!ix)
        return;
    free(ix->order);
    free(ix->items);
    free(ix->keys);
    free(ix);
}

/* Rebuilds the index of a sheet. An empty sheet has no index. A failed
 * build leaves the sheet without an index, and it resolves to empty
 * styles. */
static bool css_index_build(struct css_sheet *sheet)
{
    css_index_free(sheet->index);
    sheet->index = NULL;
    size_t n = (size_t)sheet->rule_count;
    if (n == 0)
        return true;

    struct css_index *ix = calloc(1, sizeof(*ix));
    if (!ix)
        return false;
    struct apply_slot *apply = malloc(n * sizeof(*apply));
    struct key_slot *slots = malloc(n * sizeof(*slots));
    ix->order = malloc(n * sizeof(*ix->order));
    ix->items = malloc(n * sizeof(*ix->items));
    ix->keys = malloc((n + 1) * sizeof(*ix->keys));
    if (!apply || !slots || !ix->order || !ix->items || !ix->keys) {
        free(apply);
        free(slots);
        css_index_free(ix);
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        const struct css_rule *r = &sheet->rules[i];
        apply[i] = (struct apply_slot){
            .tier = rule_tier(r), .rank = rule_rank(r), .rule = (uint32_t)i,
        };
    }
    qsort(apply, n, sizeof(*apply), cmp_apply_slot);
    for (size_t p = 0; p < n; p++) {
        ix->order[p] = apply[p].rule;
        slots[p] = rule_key(&sheet->rules[apply[p].rule], (uint32_t)p);
    }
    qsort(slots, n, sizeof(*slots), cmp_key_slot);
    for (size_t i = 0; i < n; i++)
        ix->items[i] = slots[i].pos;

    size_t i = 0;
    while (i < n && slots[i].kind == KEY_ANY)
        i++;
    ix->keys[0] = (struct css_key){ .kind = KEY_ANY, .start = 0, .count = (uint32_t)i };
    uint32_t nkeys = 1;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && cmp_key_text(slots[j].kind, slots[j].text, slots[j].len,
                                     slots[i].kind, slots[i].text, slots[i].len) == 0)
            j++;
        ix->keys[nkeys++] = (struct css_key){
            .text = slots[i].text, .len = slots[i].len, .start = (uint32_t)i,
            .count = (uint32_t)(j - i), .kind = slots[i].kind,
        };
        i = j;
    }
    ix->key_count = nkeys;

    free(apply);
    free(slots);
    sheet->index = ix;
    return true;
}

static bool parse_rules(struct css_sheet *sheet, const char *p)
{
    while (*p) {
        p = skip_ws_and_comments(p);
        if (!*p) break;
        const char *s_start = p;
        while (*p && *p != '{') p++;
        if (!*p) break;
        const char *s_end = p;

        struct css_rule proto = {0};
        p++;
        while (*p && *p != '}') {
            p = skip_ws_and_comments(p);
            if (!*p || *p == '}') break;
            const char *k_start = p;
            while (*p && *p != ':' && *p != '}') p++;
            if (*p != ':') break;
            const char *k_end = p++;
            const char *v_start = p;
            while (*p && *p != ';' && *p != '}') p++;
            const char *v_end = p;
            if (*p == ';') p++;

            while (k_end > k_start && isspace((unsigned char)k_end[-1])) k_end--;
            while (v_end > v_start && isspace((unsigned char)v_end[-1])) v_end--;
            char *key = dup_range(k_start, (size_t)(k_end - k_start));
            char *val = dup_range(v_start, (size_t)(v_end - v_start));
            if (!key || !val) {
                free(key);
                free(val);
                return false;
            }
            apply_prop(&proto, key, val);
            free(key);
            free(val);
        }
        if (*p == '}') p++;

        const char *sel_ptr = s_start;
        while (sel_ptr < s_end) {
            sel_ptr = skip_ws_and_comments(sel_ptr);
            if (sel_ptr > s_end) break;
            const char *comma = memchr(sel_ptr, ',', (size_t)(s_end - sel_ptr));
            const char *chunk_end = comma ? comma : s_end;
            if (chunk_end > sel_ptr &&
                !sheet_push(sheet, &proto, sel_ptr, (size_t)(chunk_end - sel_ptr)))
                return false;
            if (!comma) break;
            sel_ptr = comma + 1;
        }
    }
    return true;
}

bool css_parse(struct css_sheet *sheet, const char *p)
{
    if (!sheet || !p) return false;
    bool ok = parse_rules(sheet, p);
    return css_index_build(sheet) && ok;
}

void css_sheet_clear(struct css_sheet *sheet)
{
    if (!sheet)
        return;
    css_index_free(sheet->index);
    for (int i = 0; i < sheet->rule_count; i++)
        free(sheet->rules[i].selector);
    free(sheet->rules);
    *sheet = (struct css_sheet){0};
}

static bool append_rules(struct css_sheet *dst, const struct css_sheet *src)
{
    if (!sheet_reserve(dst, src->rule_count))
        return false;
    for (int i = 0; i < src->rule_count; i++) {
        struct css_rule *r = &dst->rules[dst->rule_count];
        *r = src->rules[i];
        r->selector = strdup(src->rules[i].selector);
        if (!r->selector)
            return false;
        dst->rule_count++;
    }
    return true;
}

bool css_sheet_append(struct css_sheet *dst, const struct css_sheet *src)
{
    /* Appending a sheet to itself cannot work: the loop bound and the write
     * index are the same field, so it grows without bound. */
    if (!dst || !src || dst == src)
        return false;
    bool ok = append_rules(dst, src);
    return css_index_build(dst) && ok;
}

void css_init_default(struct css_sheet *sheet)
{
    if (!sheet) return;
    css_sheet_clear(sheet);
    css_parse(sheet,
        ".label-selection { background: #45475a; color: #ffffff; }"
        ".label-link { color: #89b4fa; border-width: 1px; }"
        ".label-link:hover, .label-link:focus { color: #b4befe; }"
        ".label-link:disabled { color: #585b70; }"
        ".label-focus { border-color: #89b4fa; border-width: 1px; }"
        ".tab-bar { height: 2.2em; flex-direction: row; align-items: center; padding: 0.25em 0.5em; gap: 0.4em; }"
        ".tab { height: 1.6em; background: #1e1e2e; color: #a6adc8; border-radius: 4px; padding: 0 0.8em; flex-direction: row; align-items: center; gap: 0.5em; cursor: pointer; }"
        ".tab:hover { background: #313244; color: #cdd6f4; }"
        ".tab:focus { border-color: #89b4fa; border-width: 2px; }"
        ".tab.active { background: #45475a; color: #ffffff; }"
        ".tab.drop-preview { background: #89b4fa; opacity: 0.45; box-shadow: 0 2px 5px #00000066; }"
        ".tab-title { cursor: pointer; }"
        ".tab-close { width: 1em; height: 1em; color: #a6adc8; border-radius: 0.5em; cursor: pointer; }"
        ".tab-close:hover { background: #f38ba8; color: #11111b; }"
        ".tab-new { color: #a6adc8; border-radius: 4px; padding: 0 0.4em; cursor: pointer; }"
        ".tab-new:hover { background: #313244; color: #cdd6f4; }"
        ".scrollbar { width: 8px; padding: 0 1px; }"
        ".scrollbar:hover, .scrollbar:active { width: 12px; background: #bac2de26; }"
        ".scrollbar-thumb { width: 3px; min-height: 24px; background: #a6adc8; }"
        ".scrollbar-thumb:hover { width: 8px; background: #bac2de; }"
        ".scrollbar-thumb:active { width: 8px; background: #89b4fa; }"
        ".context-menu { background: #1e1e2e; border-color: #45475a; border-radius: 6px; box-shadow: 0 8px 24px #00000073; }"
        ".menu-item { color: #cdd6f4; cursor: pointer; }"
        ".menu-item:hover { background: #313244; color: #cdd6f4; }"
        ".menu-item.disabled { color: #585b70; cursor: default; }"
        ".menu-item:disabled { color: #585b70; cursor: default; }"
        ".menu-item:checked { color: #cdd6f4; }"
        ".menu-item.submenu { color: #cdd6f4; }"
        ".menu-label { cursor: pointer; }"
        ".menu-label:disabled { cursor: default; }"
        ".menu-shortcut { color: #7f849c; }"
        ".menu-separator { background: #45475a; }"
        ".menu-list { flex-direction: column; padding: 4px; }"
        ".menu-list .menu-item { min-height: 1.25em; padding: 0.15em 0.8em; gap: 2ch; border-radius: 4px; cursor: pointer; }"
        ".menu-list .menu-item:focus { background: #313244; }"
        ".menu-list .menu-item:disabled { color: #585b70; cursor: default; }"
        ".menu-list .menu-shortcut { color: #7f849c; }"
        ".menu-list .menu-separator { height: 1px; margin: 4px 6px; background: #45475a; }"
        ".text-input { background: #1e1e2e; border-color: #45475a; border-width: 1px; border-radius: 4px; padding: 2px 1ch; min-width: 20ch; cursor: text; }"
        ".text-input:focus { border-color: #89b4fa; }"
        ".button:focus { background: #45475a; }"
        ".command-palette { background: #1e1e2e; border-color: #45475a; border-width: 1px; border-radius: 6px; }"
        ".command-palette .text-input { background: transparent; border-width: 0; border-radius: 0; min-width: 0; padding: 6px 0; }"
        ".palette-empty { color: #7f849c; }"
        ".tab-preview { width: 400px; padding: 8px; gap: 6px; background: #1e1e2e; border-color: #45475a; border-width: 1px; border-radius: 8px; box-shadow: 0 6px 18px #00000073; }"
        ".tab-preview.dragging { width: 280px; }"
        ".tab-preview-title { color: #cdd6f4; }"
        ".dropdown-button { padding: 0.4em 1ch; background: #313244; color: #cdd6f4; border-radius: 4px; cursor: pointer; }"
        ".dropdown-button:hover, .dropdown-button:focus { background: #45475a; }"
        ".dropdown-popup { width: 46ch; height: 17em; padding: 0.6em 1ch; gap: 0.5em; background: #1e1e2e; color: #cdd6f4; border-radius: 6px; }"
        ".dropdown-scroll { height: grow; }"
        ".terminal-drop-target { background: #89b4fa33; border-color: #89b4fa; border-width: 2px; }"
        ".settings-form { flex-direction: column; padding: 1.2em 2ch; gap: 0.8em; color: #cdd6f4; }"
        ".settings-header { align-items: center; gap: 2ch; }"
        ".settings-title { width: grow; }"
        ".settings-search { padding: 0.45em 1ch; }"
        ".settings-body { height: grow; gap: 2ch; align-items: stretch; }"
        ".settings-sidebar { width: 17ch; gap: 0.4em; }"
        ".settings-scroll { width: grow; }"
        ".settings-fields { gap: 0.55em; }"
        ".settings-section { padding: 0.4em 0; color: #89b4fa; }"
        ".settings-row { padding: 0.65em 1ch; gap: 2ch; align-items: center; background: #181825; border-radius: 6px; }"
        ".settings-description { width: grow; min-width: 0; gap: 0.25em; }"
        ".settings-help { color: #a6adc8; }"
        ".settings-editor { width: grow; min-width: 0; align-items: stretch; gap: 0.4em; }"
        ".settings-editor .text-input { min-width: 0; padding: 0.4em 1ch; }"
        ".settings-error { color: #f38ba8; }"
        ".settings-footer { align-items: center; gap: 1ch; }"
        ".settings-status { width: grow; }"
        ".settings-button { padding: 0.4em 1ch; background: #313244; border-radius: 4px; cursor: pointer; }"
        ".settings-button:hover, .settings-button:focus { background: #45475a; }"
        ".settings-button:disabled { color: #585b70; cursor: default; }"
        ".settings-primary { background: #89b4fa; color: #11111b; }"
        ".settings-primary:hover, .settings-primary:focus { background: #b4befe; }"
        ".settings-primary:disabled { background: #313244; color: #585b70; }"
        ".settings-nav.active { background: #45475a; color: #89b4fa; }"
        ".settings-link { color: #89b4fa; background: transparent; cursor: pointer; }"
    );
}

const struct css_sheet *css_default_sheet(void)
{
    static struct css_sheet sheet;
    static bool ready;
    if (!ready) {
        css_init_default(&sheet);
        ready = true;
    }
    return &sheet;
}

static bool class_list_has(const char *list, size_t list_len,
                           const char *tok, size_t tlen)
{
    const char *end = list + list_len;
    for (const char *p = list; p < end;) {
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (p < end && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - start) == tlen && !memcmp(start, tok, tlen))
            return true;
    }
    return false;
}

static bool range_equals(const char *a, size_t alen, const char *b)
{
    return b && strlen(b) == alen && !memcmp(a, b, alen);
}

static bool compound_selector_matches(const char *comp, size_t len,
                                      const char *class_name, size_t class_len,
                                      const char *id_name, const char *tag_name)
{
    const char *pc = memchr(comp, ':', len);
    if (pc) len = (size_t)(pc - comp);
    if (!len) return false;
    if (comp[0] == '.') {
        if (!class_name) return false;
        const char *end = comp + len;
        for (const char *sub = comp + 1; sub < end;) {
            const char *dot = memchr(sub, '.', (size_t)(end - sub));
            const char *stop = dot ? dot : end;
            if (!class_list_has(class_name, class_len, sub, (size_t)(stop - sub)))
                return false;
            sub = stop + 1;
        }
        return true;
    }
    if (comp[0] == '#') return range_equals(comp + 1, len - 1, id_name);
    return range_equals(comp, len, tag_name);
}

static bool selector_matches(const struct css_rule *r, const char *class_name,
                             const char *id_name, const char *tag_name,
                             unsigned pseudo_flags, const char *ancestor_path)
{
    if (r->pseudo != CSS_PSEUDO_NONE && !(pseudo_flags & r->pseudo)) return false;
    const char *last = r->selector + r->last_off;
    if (!compound_selector_matches(last, strlen(last), class_name,
                                   class_name ? strlen(class_name) : 0,
                                   id_name, tag_name))
        return false;
    if (r->last_off == 0) return true;
    if (!ancestor_path) return false;
    const char *cursor = ancestor_path;
    const char *p = r->selector;
    while (p < last) {
        while (p < last && isspace((unsigned char)*p)) p++;
        if (p >= last) break;
        const char *e = p;
        while (e < last && !isspace((unsigned char)*e)) e++;
        bool found = false;
        for (const char *a = cursor; *a;) {
            const char *nl = strchr(a, '\n');
            size_t alen = nl ? (size_t)(nl - a) : strlen(a);
            if (compound_selector_matches(p, (size_t)(e - p), a, alen, NULL, NULL)) {
                cursor = nl ? nl + 1 : a + alen;
                found = true;
                break;
            }
            if (!nl) break;
            a = nl + 1;
        }
        if (!found) return false;
        p = e;
    }
    return true;
}

static void apply_rule_props(const struct css_rule *r, struct css_style *out)
{
    uint64_t mask = r->set_mask;
    while (mask) {
        int bit = __builtin_ctzll(mask);
        uint64_t bit_mask = 1ULL << bit;
        mask &= ~bit_mask;

        if (bit >= 32)
            continue;
        const struct prop_info *p = prop_by_flag[bit];
        if (!p)
            continue;
        char *dst = (char *)out + p->offset;
        const char *src = (const char *)&r->style + p->offset;
        if (p->kind == P_INSETS) {
            /* Merge per edge: edges that failed to parse (CSS_UNIT_NONE)
             * must not erase a lower-precedence rule's value. */
            const struct css_value *srcv = (const struct css_value *)src;
            struct css_value *dstv = (struct css_value *)dst;
            for (int e = 0; e < 4; e++)
                if (srcv[e].unit != CSS_UNIT_NONE)
                    dstv[e] = srcv[e];
            continue;
        }
        size_t sz = (p->kind == P_SHADOW)
                        ? offsetof(struct css_style, shadow_color) +
                          sizeof(uint32_t) -
                          offsetof(struct css_style, shadow_x) :
                    (p->kind == P_COLOR)  ? sizeof(uint32_t) :
                    (p->kind == P_VAL)    ? sizeof(struct css_value) :
                    (p->kind == P_FLOAT)  ? sizeof(float) : sizeof(int);
        memcpy(dst, src, sz);
        if (p->kind == P_COLOR) {
            if (p->offset == offsetof(struct css_style, bg_color)) out->has_bg_color = true;
            else if (p->offset == offsetof(struct css_style, text_color)) out->has_text_color = true;
            else if (p->offset == offsetof(struct css_style, border_color)) out->has_border_color = true;
            else if (p->offset == offsetof(struct css_style, ripple_color)) out->has_ripple_color = true;
        } else if (p->offset == offsetof(struct css_style, opacity)) {
            out->has_opacity = true;
        } else if (p->offset == offsetof(struct css_style, cursor)) {
            out->has_cursor = true;
        } else if (p->offset == offsetof(struct css_style, transition_duration)) {
            out->has_transition_duration = true;
        } else if (p->offset == offsetof(struct css_style, flex_direction)) {
            out->has_flex_direction = true;
        } else if (p->offset == offsetof(struct css_style, align_items)) {
            out->has_align_items = true;
        } else if (p->offset == offsetof(struct css_style, justify_content)) {
            out->has_justify_content = true;
        } else if (p->kind == P_SHADOW) {
            out->has_shadow = r->style.has_shadow;
        }
    }
}

/* Size of the candidate buffer. A lookup with more candidates finds each
 * next rule by a search instead. */
#define CSS_CANDIDATE_CAP 64

struct lookup {
    const char *class_name;
    size_t class_len;
    const char *id_name;
    const char *tag_name;
    unsigned pseudo_flags;
    const char *ancestor_path;
};

struct token_cursor {
    const char *p;
    const char *end;
};

static bool next_class_token(struct token_cursor *c, const char **tok, uint32_t *len)
{
    while (c->p < c->end && isspace((unsigned char)*c->p)) c->p++;
    if (c->p == c->end)
        return false;
    const char *s = c->p;
    while (c->p < c->end && !isspace((unsigned char)*c->p)) c->p++;
    *tok = s;
    *len = (uint32_t)(c->p - s);
    return true;
}

static const struct css_key *find_key(const struct css_index *ix, uint8_t kind,
                                      const char *text, uint32_t len)
{
    uint32_t lo = 1, hi = ix->key_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const struct css_key *k = &ix->keys[mid];
        int c = cmp_key_text(k->kind, k->text, k->len, kind, text, len);
        if (c == 0)
            return k;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static bool take_group(const struct css_index *ix, const struct css_key *k,
                       uint32_t *buf, uint32_t *n)
{
    if (!k)
        return true;
    if (*n + k->count > CSS_CANDIDATE_CAP)
        return false;
    memcpy(buf + *n, ix->items + k->start, k->count * sizeof(*buf));
    *n += k->count;
    return true;
}

/* Collects the positions of every group the lookup names. False when
 * they do not fit in buf. */
static bool gather_candidates(const struct css_index *ix, const struct lookup *q,
                              uint32_t *buf, uint32_t *n)
{
    if (!take_group(ix, &ix->keys[0], buf, n))
        return false;
    if (q->tag_name &&
        !take_group(ix, find_key(ix, KEY_TAG, q->tag_name, (uint32_t)strlen(q->tag_name)), buf, n))
        return false;
    if (q->id_name &&
        !take_group(ix, find_key(ix, KEY_ID, q->id_name, (uint32_t)strlen(q->id_name)), buf, n))
        return false;
    if (!q->class_name)
        return true;
    struct token_cursor c = { q->class_name, q->class_name + q->class_len };
    const char *tok;
    uint32_t tlen;
    while (next_class_token(&c, &tok, &tlen))
        if (!take_group(ix, find_key(ix, KEY_CLASS, tok, tlen), buf, n))
            return false;
    return true;
}

static int64_t min_after(const struct css_index *ix, const struct css_key *k,
                         int64_t last, int64_t best)
{
    if (!k || !k->count)
        return best;
    const uint32_t *items = ix->items + k->start;
    uint32_t a = 0, b = k->count;
    while (a < b) {
        uint32_t mid = a + (b - a) / 2;
        if ((int64_t)items[mid] > last)
            b = mid;
        else
            a = mid + 1;
    }
    if (a == k->count)
        return best;
    int64_t v = items[a];
    return (best < 0 || v < best) ? v : best;
}

/* Smallest apply position above last, over the groups the lookup names. */
static int64_t next_candidate(const struct css_index *ix, const struct lookup *q, int64_t last)
{
    int64_t best = min_after(ix, &ix->keys[0], last, -1);
    if (q->tag_name)
        best = min_after(ix, find_key(ix, KEY_TAG, q->tag_name,
                                      (uint32_t)strlen(q->tag_name)), last, best);
    if (q->id_name)
        best = min_after(ix, find_key(ix, KEY_ID, q->id_name,
                                      (uint32_t)strlen(q->id_name)), last, best);
    if (!q->class_name)
        return best;
    struct token_cursor c = { q->class_name, q->class_name + q->class_len };
    const char *tok;
    uint32_t tlen;
    while (next_class_token(&c, &tok, &tlen))
        best = min_after(ix, find_key(ix, KEY_CLASS, tok, tlen), last, best);
    return best;
}

static void apply_position(const struct css_sheet *sheet, const struct css_index *ix,
                           uint32_t pos, const struct lookup *q, struct css_style *out)
{
    const struct css_rule *r = &sheet->rules[ix->order[pos]];
    if (selector_matches(r, q->class_name, q->id_name, q->tag_name,
                         q->pseudo_flags, q->ancestor_path))
        apply_rule_props(r, out);
}

static void resolve_style(const struct css_sheet *sheet, const char *class_name,
                          const char *id_name, const char *tag_name,
                          unsigned pseudo_flags, const char *ancestor_path,
                          struct css_style *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->opacity = 1.0f;
    if (!sheet || !sheet->index) return;

    const struct css_index *ix = sheet->index;
    struct lookup q = {
        .class_name = class_name,
        .class_len = class_name ? strlen(class_name) : 0,
        .id_name = id_name,
        .tag_name = tag_name,
        .pseudo_flags = pseudo_flags,
        .ancestor_path = ancestor_path,
    };

    uint32_t buf[CSS_CANDIDATE_CAP];
    uint32_t n = 0;
    if (gather_candidates(ix, &q, buf, &n)) {
        for (uint32_t i = 1; i < n; i++) {
            uint32_t v = buf[i];
            uint32_t j = i;
            while (j > 0 && buf[j - 1] > v) {
                buf[j] = buf[j - 1];
                j--;
            }
            buf[j] = v;
        }
        for (uint32_t i = 0; i < n; i++)
            if (i == 0 || buf[i] != buf[i - 1])
                apply_position(sheet, ix, buf[i], &q, out);
        return;
    }

    for (int64_t last = next_candidate(ix, &q, -1); last >= 0;
         last = next_candidate(ix, &q, last))
        apply_position(sheet, ix, (uint32_t)last, &q, out);
}

void css_style_resolve_ex(const struct css_sheet *sheet, const char *class_name,
                          const char *id_name, const char *tag_name,
                          unsigned pseudo_flags, const char *ancestor_path,
                          struct css_style *out)
{
    resolve_style(sheet, class_name, id_name, tag_name, pseudo_flags, ancestor_path, out);
}

void css_style_resolve(const struct css_sheet *sheet, const char *class_name,
                       const char *id_name, const char *tag_name, unsigned pseudo_flags,
                       struct css_style *out)
{
    css_style_resolve_ex(sheet, class_name, id_name, tag_name, pseudo_flags,
                         NULL, out);
}

static bool value_eq(struct css_value a, struct css_value b)
{
    return a.val == b.val && a.unit == b.unit;
}

static bool box_eq(const struct css_box *a, const struct css_box *b)
{
    for (int i = 0; i < 4; i++)
        if (!value_eq(a->v[i], b->v[i]))
            return false;
    return true;
}

bool css_style_layout_eq(const struct css_style *a, const struct css_style *b)
{
    for (int i = 0; i < 2; i++)
        if (!value_eq(a->size[i], b->size[i]) ||
            !value_eq(a->min_size[i], b->min_size[i]) ||
            !value_eq(a->max_size[i], b->max_size[i]))
            return false;
    return box_eq(&a->padding, &b->padding) &&
           box_eq(&a->margin, &b->margin) &&
           value_eq(a->gap, b->gap) &&
           value_eq(a->border_width, b->border_width) &&
           a->has_flex_direction == b->has_flex_direction &&
           a->flex_direction == b->flex_direction &&
           a->has_align_items == b->has_align_items &&
           a->align_items == b->align_items &&
           a->has_justify_content == b->has_justify_content &&
           a->justify_content == b->justify_content;
}
