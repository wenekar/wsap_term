#include "css.h"
#include "ui/metrics.h"
#include "ui_app_css.h"
#include "ui_core_css.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool sheet_build_index(struct css_sheet *sheet);

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

/* Parses rgb(r, g, b) and rgba(r, g, b, a). Each part is a whole number from
   0 to 255. The alpha of rgb() is opaque. */
static bool parse_rgb_color(const char *s, uint32_t *out)
{
    bool alpha = !strncmp(s, "rgba(", 5);
    if (!alpha && strncmp(s, "rgb(", 4) != 0)
        return false;
    const char *p = s + (alpha ? 5 : 4);
    int part[4] = { 0, 0, 0, 255 };
    int count = alpha ? 4 : 3;
    for (int i = 0; i < count; i++) {
        while (isspace((unsigned char)*p)) p++;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255)
            return false;
        part[i] = (int)v;
        p = end;
        while (isspace((unsigned char)*p)) p++;
        if (i + 1 < count) {
            if (*p != ',')
                return false;
            p++;
        }
    }
    if (*p != ')')
        return false;
    p++;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '\0')
        return false;
    *out = ((uint32_t)part[3] << 24) | ((uint32_t)part[0] << 16) |
           ((uint32_t)part[1] << 8) | (uint32_t)part[2];
    return true;
}

static bool parse_color(const char *s, uint32_t *out)
{
    return parse_hex_color(s, out) || parse_named_color(s, out) ||
           parse_rgb_color(s, out);
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
        ? ctx->cell_h : (float)UI_DEFAULT_CELL_H * dpi;
    float cw = (ctx && isfinite(ctx->cell_w) && ctx->cell_w > 0.0f)
        ? ctx->cell_w : (float)UI_DEFAULT_CELL_W * dpi;
    float result;
    switch (v.unit) {
    case CSS_UNIT_PX:      result = v.val * dpi; break;
    case CSS_UNIT_PERCENT:
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

static bool parse_insets(const char *s, struct css_box *out)
{
    struct css_value vals[4] = {0};
    int count = 0;
    while (*s && count < 4) {
        s = skip_ws_and_comments(s);
        if (!*s || *s == ';') break;
        vals[count++] = parse_value(s);
        if (vals[count - 1].unit == CSS_UNIT_NONE)
            return false;
        while (*s && !isspace((unsigned char)*s) && *s != ';') s++;
    }
    s = skip_ws_and_comments(s);
    if (*s && *s != ';')
        return false;
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
        return false;
    }
    return true;
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

/* The style keywords that `border` accepts and ignores. */
static bool is_border_style_keyword(const char *tok)
{
    static const char *const keywords[] = {
        "solid", "dashed", "dotted", "double",
        "groove", "ridge", "inset", "outset", "hidden",
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
        if (!strcmp(tok, keywords[i]))
            return true;
    return false;
}

/* Parses `border`. The value holds a width and a color in any order. A style
   keyword is accepted and ignored, and `none` sets the width to zero.
   `out_has_width` reports whether the value named a width at all. */
static bool parse_border(const char *s, struct css_style *out, bool *out_has_width)
{
    bool width_set = false, color_set = false;
    while (*s) {
        s = skip_ws_and_comments(s);
        if (!*s) break;
        const char *start = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        char tok[32];
        size_t len = (size_t)(s - start);
        if (len >= sizeof(tok)) return false;
        memcpy(tok, start, len);
        tok[len] = '\0';
        if (!strcmp(tok, "none")) {
            out->border_width = (struct css_value){ .unit = CSS_UNIT_PX };
            width_set = true;
            continue;
        }
        if (is_border_style_keyword(tok))
            continue;
        uint32_t color;
        if (!color_set && parse_color(tok, &color)) {
            out->border_color = color;
            out->has_border_color = true;
            color_set = true;
            continue;
        }
        if (width_set)
            return false;
        struct css_value v = parse_value(tok);
        if (v.unit == CSS_UNIT_NONE)
            return false;
        out->border_width = v;
        width_set = true;
    }
    *out_has_width = width_set;
    return width_set || color_set;
}

enum prop_kind {
    P_VAL,
    P_INSETS,
    P_COLOR,
    P_FLOAT,
    P_FLEX_DIR,
    P_ALIGN,
    P_JUSTIFY,
    P_CURSOR,
    P_DURATION,
    P_SHADOW,
    P_BORDER, /* A width and an optional color. */
    P_WRAP,
    P_ALIGN_SELF,
    P_FAMILY,
    P_FONT_WEIGHT,
    P_FONT_STYLE,
    P_LINE_HEIGHT,
    P_OVERFLOW,
    P_POSITION,
    P_INT,
    P_PAD_EDGE,    /* `size` is the edge. */
    P_MARGIN_EDGE,
    P_BORDER_EDGE,
    P_INSET,       /* `size` is top, right, bottom, or left. */
};

enum style_has_bit {
    HAS_NONE = 0,
    HAS_has_bg_color,
    HAS_has_text_color,
    HAS_has_border_color,
    HAS_has_shadow,
    HAS_has_opacity,
    HAS_has_transition_duration,
    HAS_has_cursor,
    HAS_has_ripple_color,
    HAS_has_flex_direction,
    HAS_has_align_items,
    HAS_has_justify_content,
    HAS_has_flex_wrap,
    HAS_has_flex_grow,
    HAS_has_flex_shrink,
    HAS_has_align_self,
    HAS_has_font_family,
    HAS_has_font_size,
    HAS_has_font_weight,
    HAS_has_font_style,
    HAS_has_line_height,
    HAS_has_overflow,
    HAS_has_position,
    HAS_has_z_index,
};

struct prop_info {
    const char *name;
    uint16_t offset;
    uint8_t kind;
    uint8_t size;
    uint8_t has_bit;
    uint8_t pad_[3];
};

#define PROP_FIELD_SIZE(m) ((uint8_t)sizeof(((struct css_style *)0)->m))
#define CSS_SHADOW_SIZE \
    (offsetof(struct css_style, shadow_color) + sizeof(uint32_t) - \
     offsetof(struct css_style, shadow_x))

/* A rule records the properties it sets in `set_mask`. Bit `i` belongs to
 * entry `i` of this table, so a set bit names its own entry. */
#define PROP(n, k, m) { \
    .name = (n), .offset = offsetof(struct css_style, m), \
    .kind = (uint8_t)(k), .size = PROP_FIELD_SIZE(m), \
}
#define PROP_HAS(n, k, m, has_m) { \
    .name = (n), .offset = offsetof(struct css_style, m), \
    .kind = (uint8_t)(k), .size = PROP_FIELD_SIZE(m), \
    .has_bit = HAS_##has_m, \
}
#define PROP_ENUM(n, k, has_m) { \
    .name = (n), .kind = (uint8_t)(k), .has_bit = HAS_##has_m, \
}
#define PROP_COLOR(n, m, has_m) PROP_HAS(n, P_COLOR, m, has_m)
#define PROP_FLOAT(n, m, has_m) PROP_HAS(n, P_FLOAT, m, has_m)
#define PROP_SHADOW(n, m, has_m) { \
    .name = (n), .offset = offsetof(struct css_style, m), \
    .kind = P_SHADOW, .size = (uint8_t)CSS_SHADOW_SIZE, \
    .has_bit = HAS_##has_m, \
}
#define PROP_BORDER(n, m) { \
    .name = (n), .offset = offsetof(struct css_style, m), \
    .kind = P_BORDER, .size = PROP_FIELD_SIZE(m), \
}
/* A per-edge property. `edge` is its index in the box. */
#define PROP_EDGE(n, k, m, edge) { \
    .name = (n), .offset = offsetof(struct css_style, m), \
    .kind = (uint8_t)(k), .size = (uint8_t)(edge), \
}

static const struct prop_info props[] = {
    PROP("width",            P_VAL,      width),
    PROP("height",           P_VAL,      height),
    PROP("min-width",        P_VAL,      min_width),
    PROP("min-height",       P_VAL,      min_height),
    PROP("max-width",        P_VAL,      max_width),
    PROP("max-height",       P_VAL,      max_height),
    PROP("gap",              P_VAL,      gap),
    PROP("border-radius",    P_VAL,      border_radius),
    PROP("border-width",     P_VAL,      border_width),
    PROP_BORDER("border",    border_width),
    PROP("padding",          P_INSETS,   padding),
    PROP("margin",           P_INSETS,   margin),
    PROP_COLOR("background-color", bg_color, has_bg_color),
    PROP_COLOR("background",       bg_color, has_bg_color),
    PROP_COLOR("color",            text_color, has_text_color),
    PROP_COLOR("border-color",     border_color, has_border_color),
    PROP_FLOAT("opacity",    opacity,    has_opacity),
    PROP_ENUM("flex-direction",     P_FLEX_DIR, has_flex_direction),
    PROP_ENUM("align-items",        P_ALIGN,    has_align_items),
    PROP_ENUM("justify-content",    P_JUSTIFY,  has_justify_content),
    PROP_ENUM("flex-wrap",          P_WRAP,     has_flex_wrap),
    PROP_ENUM("align-self",         P_ALIGN_SELF, has_align_self),
    PROP_FLOAT("flex-grow",  flex_grow,   has_flex_grow),
    PROP_FLOAT("flex-shrink", flex_shrink, has_flex_shrink),
    PROP_ENUM("cursor",             P_CURSOR,   has_cursor),
    PROP_HAS("transition-duration", P_DURATION, transition_duration, has_transition_duration),
    PROP_HAS("animation-duration",  P_DURATION, transition_duration, has_transition_duration),
    PROP_HAS("transition",          P_DURATION, transition_duration, has_transition_duration),
    PROP_SHADOW("box-shadow",       shadow_x,                        has_shadow),
    PROP_COLOR("ripple-color", ripple_color, has_ripple_color),
    PROP_COLOR("ripple",       ripple_color, has_ripple_color),
    PROP_HAS("font-family",  P_FAMILY,      font_family, has_font_family),
    PROP_HAS("font-size",    P_VAL,         font_size,   has_font_size),
    PROP_HAS("font-weight",  P_FONT_WEIGHT, font_weight, has_font_weight),
    PROP_ENUM("font-style",  P_FONT_STYLE,  has_font_style),
    PROP_HAS("line-height",  P_LINE_HEIGHT, line_height, has_line_height),
    PROP_ENUM("overflow",    P_OVERFLOW,    has_overflow),
    PROP_ENUM("position",    P_POSITION,    has_position),
    PROP_HAS("z-index",      P_INT,         z_index,     has_z_index),
    PROP_EDGE("padding-top", P_PAD_EDGE, padding, 0),
    PROP_EDGE("padding-right", P_PAD_EDGE, padding, 1),
    PROP_EDGE("padding-bottom", P_PAD_EDGE, padding, 2),
    PROP_EDGE("padding-left", P_PAD_EDGE, padding, 3),
    PROP_EDGE("margin-top", P_MARGIN_EDGE, margin, 0),
    PROP_EDGE("margin-right", P_MARGIN_EDGE, margin, 1),
    PROP_EDGE("margin-bottom", P_MARGIN_EDGE, margin, 2),
    PROP_EDGE("margin-left", P_MARGIN_EDGE, margin, 3),
    PROP_EDGE("border-top-width", P_BORDER_EDGE, border_edges, 0),
    PROP_EDGE("border-right-width", P_BORDER_EDGE, border_edges, 1),
    PROP_EDGE("border-bottom-width", P_BORDER_EDGE, border_edges, 2),
    PROP_EDGE("border-left-width", P_BORDER_EDGE, border_edges, 3),
    PROP_EDGE("top", P_INSET, inset, 0),
    PROP_EDGE("right", P_INSET, inset, 1),
    PROP_EDGE("bottom", P_INSET, inset, 2),
    PROP_EDGE("left", P_INSET, inset, 3),
};

static const size_t prop_count = sizeof(props) / sizeof(props[0]);
static_assert(sizeof(props) / sizeof(props[0]) <= UINT16_MAX,
              "Property indices must fit in uint16_t");
static_assert(CSS_SHADOW_SIZE <= UINT8_MAX,
              "Shadow fields must fit in prop_info.size");
static_assert(HAS_NONE == 0 && HAS_has_z_index <= UINT8_MAX,
              "Zero has_bit must mean no presence flag");

/* The rule mask has one bit per property. It grows with the property table,
   so a new property cannot overflow it. */

static uint32_t prop_mask_words(void)
{
    return (uint32_t)((prop_count + 63) / 64);
}

static bool prop_mask_init(struct css_prop_mask *mask)
{
    if (mask->bits)
        return true;
    uint32_t words = prop_mask_words();
    mask->bits = calloc(words, sizeof(*mask->bits));
    if (!mask->bits)
        return false;
    mask->words = words;
    return true;
}

static bool prop_mask_add(struct css_prop_mask *mask, size_t index)
{
    if (index >= prop_count || !prop_mask_init(mask))
        return false;
    mask->bits[index / 64] |= 1ULL << (index % 64);
    return true;
}

static bool prop_mask_has(const struct css_prop_mask *mask, size_t index)
{
    return index < prop_count && index / 64 < mask->words &&
           (mask->bits[index / 64] >> (index % 64)) & 1ULL;
}

static void prop_mask_clear(struct css_prop_mask *mask)
{
    free(mask->bits);
    mask->bits = NULL;
    mask->words = 0;
}

static bool prop_mask_copy(struct css_prop_mask *dst,
                           const struct css_prop_mask *src)
{
    dst->bits = NULL;
    dst->words = 0;
    if (!src->words)
        return true;
    dst->bits = malloc((size_t)src->words * sizeof(*dst->bits));
    if (!dst->bits)
        return false;
    memcpy(dst->bits, src->bits, (size_t)src->words * sizeof(*dst->bits));
    dst->words = src->words;
    return true;
}

static void set_style_has_bit(struct css_style *s, uint8_t bit, bool v)
{
    switch ((enum style_has_bit)bit) {
    case HAS_NONE:                    break;
    case HAS_has_bg_color:            s->has_bg_color = v; break;
    case HAS_has_text_color:          s->has_text_color = v; break;
    case HAS_has_border_color:        s->has_border_color = v; break;
    case HAS_has_shadow:              s->has_shadow = v; break;
    case HAS_has_opacity:             s->has_opacity = v; break;
    case HAS_has_transition_duration: s->has_transition_duration = v; break;
    case HAS_has_cursor:              s->has_cursor = v; break;
    case HAS_has_ripple_color:        s->has_ripple_color = v; break;
    case HAS_has_flex_direction:      s->has_flex_direction = v; break;
    case HAS_has_align_items:         s->has_align_items = v; break;
    case HAS_has_justify_content:     s->has_justify_content = v; break;
    case HAS_has_flex_wrap:           s->has_flex_wrap = v; break;
    case HAS_has_flex_grow:           s->has_flex_grow = v; break;
    case HAS_has_flex_shrink:         s->has_flex_shrink = v; break;
    case HAS_has_align_self:          s->has_align_self = v; break;
    case HAS_has_font_family:         s->has_font_family = v; break;
    case HAS_has_font_size:           s->has_font_size = v; break;
    case HAS_has_font_weight:         s->has_font_weight = v; break;
    case HAS_has_font_style:          s->has_font_style = v; break;
    case HAS_has_line_height:         s->has_line_height = v; break;
    case HAS_has_overflow:            s->has_overflow = v; break;
    case HAS_has_position:            s->has_position = v; break;
    case HAS_has_z_index:             s->has_z_index = v; break;
    }
}

static bool get_style_has_bit(const struct css_style *s, uint8_t bit)
{
    switch ((enum style_has_bit)bit) {
    case HAS_NONE:                    return false;
    case HAS_has_bg_color:            return s->has_bg_color;
    case HAS_has_text_color:          return s->has_text_color;
    case HAS_has_border_color:        return s->has_border_color;
    case HAS_has_shadow:              return s->has_shadow;
    case HAS_has_opacity:             return s->has_opacity;
    case HAS_has_transition_duration: return s->has_transition_duration;
    case HAS_has_cursor:              return s->has_cursor;
    case HAS_has_ripple_color:        return s->has_ripple_color;
    case HAS_has_flex_direction:      return s->has_flex_direction;
    case HAS_has_align_items:         return s->has_align_items;
    case HAS_has_justify_content:     return s->has_justify_content;
    case HAS_has_flex_wrap:           return s->has_flex_wrap;
    case HAS_has_flex_grow:           return s->has_flex_grow;
    case HAS_has_flex_shrink:         return s->has_flex_shrink;
    case HAS_has_align_self:          return s->has_align_self;
    case HAS_has_font_family:         return s->has_font_family;
    case HAS_has_font_size:           return s->has_font_size;
    case HAS_has_font_weight:         return s->has_font_weight;
    case HAS_has_font_style:          return s->has_font_style;
    case HAS_has_line_height:         return s->has_line_height;
    case HAS_has_overflow:            return s->has_overflow;
    case HAS_has_position:            return s->has_position;
    case HAS_has_z_index:             return s->has_z_index;
    }
    return false;
}

enum css_declaration_result {
    CSS_DECLARATION_OK,
    CSS_DECLARATION_INVALID,
    CSS_DECLARATION_UNKNOWN,
    CSS_DECLARATION_SYNTAX,
    CSS_DECLARATION_SELECTOR,
};

static int find_prop_idx(const char *key)
{
    while (isspace((unsigned char)*key)) key++;
    for (size_t i = 0; i < prop_count; i++) {
        if (strcmp(key, props[i].name) == 0)
            return (int)i;
    }
    return -1;
}

static enum css_declaration_result apply_prop_at(struct css_rule *r, size_t idx,
                                                  const char *val)
{
    while (isspace((unsigned char)*val)) val++;
    const struct prop_info *p = &props[idx];
    char *dst = (char *)&r->style + p->offset;
    /* A `border` value that holds no width must not declare the width, or it
       would clear the width of a rule below it. */
    bool sets_field = true;
    bool sets_has = true;
    switch (p->kind) {
        case P_VAL: {
            struct css_value v = parse_value(val);
            if (v.unit == CSS_UNIT_NONE) return CSS_DECLARATION_INVALID;
            const char *end = val;
            while (*end && !isspace((unsigned char)*end)) end++;
            while (isspace((unsigned char)*end)) end++;
            if (*end) return CSS_DECLARATION_INVALID;
            *(struct css_value *)dst = v;
            break;
        }
        case P_INSETS:
            if (!parse_insets(val, (struct css_box *)dst)) return CSS_DECLARATION_INVALID;
            break;
        case P_COLOR: {
            uint32_t color;
            if (!parse_color(val, &color)) return CSS_DECLARATION_INVALID;
            *(uint32_t *)dst = color;
            break;
        }
        case P_FLOAT: {
            char *end = NULL;
            float f = strtof(val, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (!end || end == val || *end != '\0' || !isfinite(f)) return CSS_DECLARATION_INVALID;
            if (p->offset == offsetof(struct css_style, opacity)) {
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
            }
            if (p->offset == offsetof(struct css_style, flex_grow) ||
                p->offset == offsetof(struct css_style, flex_shrink)) {
                if (f < 0.0f) f = 0.0f;
            }
            memcpy(dst, &f, sizeof f);
            break;
        }
        case P_FLEX_DIR:
            if (!strcmp(val, "column"))
                r->style.flex_direction = CSS_FLEX_COLUMN;
            else if (!strcmp(val, "row"))
                r->style.flex_direction = CSS_FLEX_ROW;
            else
                return CSS_DECLARATION_INVALID;
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
            if (!found) return CSS_DECLARATION_INVALID;
            r->style.align_items = align;
            break;
        }
        case P_JUSTIFY: {
            static const struct { const char *name; enum css_justify val; } justify_map[] = {
                { "center",        CSS_JUSTIFY_CENTER },
                { "flex-end",      CSS_JUSTIFY_END },
                { "end",           CSS_JUSTIFY_END },
                { "flex-start",    CSS_JUSTIFY_START },
                { "start",         CSS_JUSTIFY_START },
                { "space-between", CSS_JUSTIFY_SPACE_BETWEEN },
                { "space-around",  CSS_JUSTIFY_SPACE_AROUND },
                { "space-evenly",  CSS_JUSTIFY_SPACE_EVENLY },
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
            if (!found) return CSS_DECLARATION_INVALID;
            r->style.justify_content = just;
            break;
        }
        case P_WRAP: {
            if (!strcmp(val, "nowrap"))
                r->style.flex_wrap = CSS_WRAP_NONE;
            else if (!strcmp(val, "wrap"))
                r->style.flex_wrap = CSS_WRAP_WRAP;
            else if (!strcmp(val, "wrap-reverse"))
                r->style.flex_wrap = CSS_WRAP_REVERSE;
            else
                return CSS_DECLARATION_INVALID;
            break;
        }
        case P_ALIGN_SELF: {
            if (!strcmp(val, "auto")) {
                sets_has = false;
                break;
            }
            static const struct { const char *name; enum css_align val; } self_map[] = {
                { "center",     CSS_ALIGN_CENTER },
                { "flex-start", CSS_ALIGN_START },
                { "start",      CSS_ALIGN_START },
                { "flex-end",   CSS_ALIGN_END },
                { "end",        CSS_ALIGN_END },
                { "stretch",    CSS_ALIGN_STRETCH },
            };
            enum css_align align = CSS_ALIGN_STRETCH;
            bool found = false;
            for (size_t k = 0; k < sizeof(self_map)/sizeof(self_map[0]); k++) {
                if (!strcmp(val, self_map[k].name)) {
                    align = self_map[k].val;
                    found = true;
                    break;
                }
            }
            if (!found) return CSS_DECLARATION_INVALID;
            r->style.align_self = align;
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
            if (!found) return CSS_DECLARATION_INVALID;
            r->style.cursor = cur;
            break;
        }
        case P_FAMILY: {
            char *family = (char *)dst;
            size_t len = strlen(val);
            if (len == 0 || len >= 48)
                return CSS_DECLARATION_INVALID;
            memcpy(family, val, len + 1);
            break;
        }
        case P_FONT_WEIGHT: {
            int weight;
            if (!strcmp(val, "normal"))
                weight = 400;
            else if (!strcmp(val, "bold"))
                weight = 700;
            else {
                char *end = NULL;
                long v = strtol(val, &end, 10);
                if (!end || end == val || *end || v < 1 || v > 1000)
                    return CSS_DECLARATION_INVALID;
                weight = (int)v;
            }
            *(int *)dst = weight;
            break;
        }
        case P_FONT_STYLE:
            if (!strcmp(val, "normal"))
                r->style.font_style = CSS_FONT_NORMAL;
            else if (!strcmp(val, "italic"))
                r->style.font_style = CSS_FONT_ITALIC;
            else if (!strcmp(val, "oblique"))
                r->style.font_style = CSS_FONT_OBLIQUE;
            else
                return CSS_DECLARATION_INVALID;
            break;
        case P_LINE_HEIGHT: {
            struct css_style *style = &r->style;
            if (!strcmp(val, "normal")) {
                style->line_height = (struct css_value){ .unit = CSS_UNIT_AUTO };
                break;
            }
            char *end = NULL;
            float f = strtof(val, &end);
            while (end && isspace((unsigned char)*end)) end++;
            if (end && end != val && *end == '\0') {
                if (!isfinite(f) || f < 0.0f) return CSS_DECLARATION_INVALID;
                style->line_height =
                    (struct css_value){ .val = f * 100.0f,
                                        .unit = CSS_UNIT_PERCENT };
                break;
            }
            struct css_value v = parse_value(val);
            if (v.unit == CSS_UNIT_NONE) return CSS_DECLARATION_INVALID;
            style->line_height = v;
            break;
        }
        case P_OVERFLOW:
            if (!strcmp(val, "visible"))
                r->style.overflow = CSS_OVERFLOW_VISIBLE;
            else if (!strcmp(val, "hidden"))
                r->style.overflow = CSS_OVERFLOW_HIDDEN;
            else if (!strcmp(val, "clip"))
                r->style.overflow = CSS_OVERFLOW_CLIP;
            else if (!strcmp(val, "scroll"))
                r->style.overflow = CSS_OVERFLOW_SCROLL;
            else if (!strcmp(val, "auto"))
                r->style.overflow = CSS_OVERFLOW_AUTO;
            else
                return CSS_DECLARATION_INVALID;
            break;
        case P_POSITION:
            if (!strcmp(val, "static"))
                r->style.position = CSS_POSITION_STATIC;
            else if (!strcmp(val, "relative"))
                r->style.position = CSS_POSITION_RELATIVE;
            else if (!strcmp(val, "absolute"))
                r->style.position = CSS_POSITION_ABSOLUTE;
            else
                return CSS_DECLARATION_INVALID;
            break;
        case P_INT: {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            if (!end || end == val || *end || v < INT_MIN || v > INT_MAX)
                return CSS_DECLARATION_INVALID;
            *(int *)dst = (int)v;
            break;
        }
        case P_PAD_EDGE:
        case P_MARGIN_EDGE:
        case P_BORDER_EDGE:
        case P_INSET: {
            struct css_value v = parse_value(val);
            if (v.unit == CSS_UNIT_NONE) return CSS_DECLARATION_INVALID;
            struct css_value *edges = (struct css_value *)dst;
            edges[p->size] = v;
            break;
        }
        case P_SHADOW:
            if (!parse_box_shadow(val, &r->style)) return CSS_DECLARATION_INVALID;
            break;
        case P_BORDER: {
            struct css_style border = {0};
            bool has_width = false;
            if (!parse_border(val, &border, &has_width))
                return CSS_DECLARATION_INVALID;
            if (has_width)
                r->style.border_width = border.border_width;
            else
                sets_field = false;
            if (border.has_border_color) {
                int color = find_prop_idx("border-color");
                r->style.border_color = border.border_color;
                r->style.has_border_color = true;
                if (color >= 0 && !prop_mask_add(&r->set_mask, (size_t)color))
                    return CSS_DECLARATION_INVALID;
            }
            break;
        }
        case P_DURATION: {
            const char *s = val;
            while (*s && !isdigit((unsigned char)*s) && *s != '.')
                s++;
            char *end = NULL;
            float f = strtof(s, &end);
            if (!end || end == s || !isfinite(f))
                return CSS_DECLARATION_INVALID;
            while (isspace((unsigned char)*end)) end++;
            if (!strncmp(end, "s", 1) && strncmp(end, "ms", 2) != 0)
                f *= 1000.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 3600000.0f) f = 3600000.0f;
            memcpy(dst, &f, sizeof(f));
            break;
        }
    }
    if (p->has_bit && p->kind != P_SHADOW && sets_has)
        set_style_has_bit(&r->style, p->has_bit, true);
    if (sets_field && !prop_mask_add(&r->set_mask, idx))
        return CSS_DECLARATION_INVALID;
    return CSS_DECLARATION_OK;
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

static bool rule_add_part(struct css_rule *r, uint32_t offset, uint16_t length)
{
    if (r->part_count == r->part_cap) {
        uint16_t cap = r->part_cap ? (uint16_t)(r->part_cap * 2) : 4;
        struct css_selector_part *next =
            realloc(r->parts, (size_t)cap * sizeof(*next));
        if (!next)
            return false;
        r->parts = next;
        r->part_cap = cap;
    }
    r->parts[r->part_count++] = (struct css_selector_part){ .offset = offset,
                                                            .length = length };
    return true;
}

static bool rule_add_class(struct css_rule *r, uint32_t offset, uint16_t length)
{
    if (r->class_count == r->class_cap) {
        uint16_t cap = r->class_cap ? (uint16_t)(r->class_cap * 2) : 4;
        struct css_class_token *next =
            realloc(r->classes, (size_t)cap * sizeof(*next));
        if (!next)
            return false;
        r->classes = next;
        r->class_cap = cap;
    }
    r->classes[r->class_count++] =
        (struct css_class_token){ .offset = offset, .length = length };
    return true;
}

static bool parse_pseudo_name(const char *s, enum css_pseudo *out)
{
    if (!strcmp(s, "hover"))         *out = CSS_PSEUDO_HOVER;
    else if (!strcmp(s, "active"))   *out = CSS_PSEUDO_ACTIVE;
    else if (!strcmp(s, "focus"))    *out = CSS_PSEUDO_FOCUS;
    else if (!strcmp(s, "disabled")) *out = CSS_PSEUDO_DISABLED;
    else if (!strcmp(s, "checked"))  *out = CSS_PSEUDO_CHECKED;
    else return false;
    return true;
}

/* Splits the compound `part` into its class names and its key kind. A part
   that names no class name, or an empty one, matches no element. */
static bool rule_compile_part(struct css_rule *r, struct css_selector_part *part)
{
    const char *text = r->selector + part->offset;
    part->class_first = r->class_count;
    part->class_count = 0;
    if (part->length == 0) {
        part->kind = CSS_KEY_NONE;
        return true;
    }
    if (text[0] == '#') {
        part->kind = CSS_KEY_ID;
        return true;
    }
    if (text[0] != '.') {
        part->kind = CSS_KEY_TAG;
        return true;
    }
    for (uint16_t i = 0; i < part->length;) {
        if (text[i] != '.')
            break;
        uint16_t start = (uint16_t)(i + 1);
        i = start;
        while (i < part->length && text[i] != '.')
            i++;
        if (i == start) {
            /* A lone point names no class and matches any class list. A
               point after a class name names an empty class name, which no
               element has. */
            if (!(part->class_count == 0 && start == part->length)) {
                part->kind = CSS_KEY_NONE;
                part->class_count = 0;
            }
            return true;
        }
        if (!rule_add_class(r, part->offset + start, (uint16_t)(i - start)))
            return false;
        part->class_count++;
    }
    return true;
}

/* Compiles `selector` into the compound parts that a match walks. The parts
   are the compounds that white space separates, from the leftmost to the
   element itself. Only the last part can carry a pseudo-class. */
static bool rule_compile_selector(struct css_rule *r, bool *supported)
{
    *supported = true;
    r->part_count = 0;
    r->class_count = 0;
    r->pseudo = CSS_PSEUDO_NONE;
    const char *s = r->selector;
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i == n) break;
        size_t start = i;
        while (i < n && !isspace((unsigned char)s[i])) i++;
        if (!rule_add_part(r, (uint32_t)start, (uint16_t)(i - start)))
            return false;
    }
    for (uint16_t p = 0; p < r->part_count; p++) {
        struct css_selector_part *part = &r->parts[p];
        const char *text = r->selector + part->offset;
        const char *colon = memchr(text, ':', part->length);
        if (!colon)
            continue;
        if (p + 1 != r->part_count) {
            *supported = false;
            part->length = (uint16_t)(colon - text);
            continue;
        }
        enum css_pseudo pseudo;
        part->length = (uint16_t)(colon - text);
        if (parse_pseudo_name(colon + 1, &pseudo))
            r->pseudo = pseudo;
        else
            *supported = false;
    }
    for (uint16_t p = 0; p < r->part_count; p++)
        if (!rule_compile_part(r, &r->parts[p]))
            return false;
    return true;
}

static void rule_clear_selector(struct css_rule *r)
{
    free(r->selector);
    free(r->parts);
    free(r->classes);
    prop_mask_clear(&r->set_mask);
    r->selector = NULL;
    r->parts = NULL;
    r->classes = NULL;
    r->part_count = 0;
    r->part_cap = 0;
    r->class_count = 0;
    r->class_cap = 0;
}

static bool parse_single_selector(struct css_rule *r, const char *s, size_t len,
                                  bool *supported)
{
    while (len && isspace((unsigned char)*s)) { s++; len--; }
    while (len && isspace((unsigned char)s[len - 1])) len--;
    r->selector = dup_range(s, len);
    if (!r->selector)
        return false;
    return rule_compile_selector(r, supported);
}

#define CSS_VAR_MAX_DEPTH 32
#define CSS_VAR_MAX_BYTES (64 * 1024)

enum css_var_status {
    CSS_VAR_OK = 0,
    CSS_VAR_SYNTAX,
    CSS_VAR_MISSING,
    CSS_VAR_CYCLE,
    CSS_VAR_LIMIT,
    CSS_VAR_OOM,
};

struct css_buf {
    char *data;
    size_t len;
    size_t cap;
};

struct css_expand_ctx {
    size_t bytes;
    size_t calls;
};

static enum css_var_status css_buf_append(struct css_buf *b, const char *s,
                                          size_t n, size_t *work_bytes)
{
    if (work_bytes) {
        if (n > CSS_VAR_MAX_BYTES - *work_bytes)
            return CSS_VAR_LIMIT;
        *work_bytes += n;
    }
    if (n > CSS_VAR_MAX_BYTES - b->len)
        return CSS_VAR_LIMIT;
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need) {
            if (cap > (size_t)-1 / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *next = realloc(b->data, cap);
        if (!next)
            return CSS_VAR_OOM;
        b->data = next;
        b->cap = cap;
    }
    if (n > 0)
        memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return CSS_VAR_OK;
}

static void rule_clear_vars(struct css_rule *r)
{
    for (int i = 0; i < r->var_prop_count; i++)
        free(r->var_props[i].value);
    free(r->var_props);
    r->var_props = NULL;
    r->var_prop_count = 0;
    r->var_prop_cap = 0;
}

static bool rule_copy_var_props(struct css_rule *dst, const struct css_rule *src)
{
    dst->var_props = NULL;
    dst->var_prop_count = 0;
    dst->var_prop_cap = 0;
    if (src->var_prop_count <= 0)
        return true;
    dst->var_props = calloc((size_t)src->var_prop_count, sizeof(*dst->var_props));
    if (!dst->var_props)
        return false;
    dst->var_prop_cap = src->var_prop_count;
    for (int i = 0; i < src->var_prop_count; i++) {
        dst->var_props[i] = src->var_props[i];
        dst->var_props[i].value = strdup(src->var_props[i].value);
        if (!dst->var_props[i].value) {
            rule_clear_vars(dst);
            return false;
        }
        dst->var_prop_count++;
    }
    return true;
}

static void rule_remove_var_prop(struct css_rule *r, int idx)
{
    free(r->var_props[idx].value);
    if (idx + 1 < r->var_prop_count) {
        memmove(&r->var_props[idx], &r->var_props[idx + 1],
                (size_t)(r->var_prop_count - 1 - idx) * sizeof(*r->var_props));
    }
    r->var_prop_count--;
}

/* Drops the declarations before `limit_idx` that set the same field. Two
   spellings of one field, such as `border` and `border-width`, share the
   offset, so the offset names the field. */
static int rule_drop_matching_var_props(struct css_rule *r, int limit_idx,
                                        size_t prop_idx)
{
    uint16_t off = props[prop_idx].offset;
    int dropped = 0;
    for (int i = limit_idx - 1; i >= 0; i--) {
        size_t cur = r->var_props[i].prop_idx;
        if (props[cur].offset == off) {
            rule_remove_var_prop(r, i);
            dropped++;
        }
    }
    return limit_idx - dropped;
}

/* Keeps the raw text of a declaration that holds var(), so the sheet can
   expand it again after the variables change. When `replace` is false, an
   earlier declaration of the same property stays until this one resolves. */
static bool rule_set_var_prop(struct css_rule *r, size_t prop_idx, const char *val,
                              int line, int col, bool replace)
{
    char *copy = strdup(val);
    if (!copy)
        return false;
    if (r->var_prop_count == r->var_prop_cap) {
        int cap = r->var_prop_cap ? r->var_prop_cap * 2 : 4;
        struct css_var_prop *next = realloc(r->var_props,
                                            (size_t)cap * sizeof(*next));
        if (!next) {
            free(copy);
            return false;
        }
        r->var_props = next;
        r->var_prop_cap = cap;
    }
    if (replace)
        rule_drop_matching_var_props(r, r->var_prop_count, prop_idx);
    r->var_props[r->var_prop_count++] = (struct css_var_prop){
        .prop_idx = (uint16_t)prop_idx,
        .value = copy,
        .line = line,
        .column = col,
    };
    return true;
}

static bool is_valid_var_name(const char *s, size_t len)
{
    if (len <= 2 || s[0] != '-' || s[1] != '-')
        return false;
    for (size_t i = 2; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '-' && c != '_')
            return false;
    }
    return true;
}

static const struct css_var *sheet_find_var(const struct css_sheet *sheet,
                                            const char *name, size_t len)
{
    for (int i = 0; i < sheet->var_count; i++) {
        const char *vn = sheet->vars[i].name;
        if (strlen(vn) == len && memcmp(vn, name, len) == 0)
            return &sheet->vars[i];
    }
    return NULL;
}

static bool sheet_set_var(struct css_sheet *sheet, const char *name,
                          const char *val, int line, int col)
{
    char *vcopy = strdup(val);
    if (!vcopy)
        return false;
    for (int i = 0; i < sheet->var_count; i++) {
        if (strcmp(sheet->vars[i].name, name) == 0) {
            free(sheet->vars[i].value);
            sheet->vars[i].value = vcopy;
            sheet->vars[i].line = line;
            sheet->vars[i].column = col;
            return true;
        }
    }
    char *ncopy = strdup(name);
    if (!ncopy) {
        free(vcopy);
        return false;
    }
    if (sheet->var_count == sheet->var_cap) {
        int cap = sheet->var_cap ? sheet->var_cap * 2 : 16;
        struct css_var *vars = realloc(sheet->vars, (size_t)cap * sizeof(*vars));
        if (!vars) {
            free(ncopy);
            free(vcopy);
            return false;
        }
        sheet->vars = vars;
        sheet->var_cap = cap;
    }
    sheet->vars[sheet->var_count++] = (struct css_var){
        .name = ncopy,
        .value = vcopy,
        .line = line,
        .column = col,
    };
    return true;
}

static bool selector_is_root(const char *s, const char *e)
{
    const char *p = s;
    const char *tok = NULL;
    size_t tok_len = 0;
    while (p < e) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            p = p + 1 < e ? p + 2 : e;
            continue;
        }
        if (tok)
            return false;
        tok = p;
        while (p < e && !isspace((unsigned char)*p) &&
               !(p + 1 < e && p[0] == '/' && p[1] == '*'))
            p++;
        tok_len = (size_t)(p - tok);
    }
    return tok && tok_len == 5 && memcmp(tok, ":root", 5) == 0;
}

static const char *find_var_call(const char *s, const char *base)
{
    const char *p = s;
    while ((p = strstr(p, "var(")) != NULL) {
        if (p == base)
            return p;
        unsigned char prev = (unsigned char)p[-1];
        if (!isalnum(prev) && prev != '-' && prev != '_')
            return p;
        p += 4;
    }
    return NULL;
}

static bool parse_var_call(const char *start, const char **out_end,
                           const char **out_name, size_t *out_nlen,
                           const char **out_fb, size_t *out_flen,
                           bool *out_has_fb)
{
    const char *p = start + 4;
    int depth = 1;
    const char *comma = NULL;
    while (*p && depth > 0) {
        if (*p == '(')
            depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0)
                break;
        } else if (*p == ',' && depth == 1 && !comma) {
            comma = p;
        }
        p++;
    }
    if (depth != 0)
        return false;
    const char *n_start = start + 4;
    const char *n_end = comma ? comma : p;
    while (n_start < n_end && isspace((unsigned char)*n_start)) n_start++;
    while (n_end > n_start && isspace((unsigned char)n_end[-1])) n_end--;
    size_t n_len = (size_t)(n_end - n_start);
    if (!is_valid_var_name(n_start, n_len))
        return false;
    *out_end = p + 1;
    *out_name = n_start;
    *out_nlen = n_len;
    *out_has_fb = (comma != NULL);
    if (comma) {
        const char *f_start = comma + 1;
        const char *f_end = p;
        while (f_start < f_end && isspace((unsigned char)*f_start)) f_start++;
        while (f_end > f_start && isspace((unsigned char)f_end[-1])) f_end--;
        *out_fb = f_start;
        *out_flen = (size_t)(f_end - f_start);
    } else {
        *out_fb = NULL;
        *out_flen = 0;
    }
    return true;
}

/* Expands one declaration value. `stack` holds the variable names that the
   expansion already entered, so a name that appears twice is a cycle. */
static enum css_var_status expand_var_text(const struct css_sheet *sheet,
                                           const char *text,
                                           const char *stack[], int stack_len,
                                           int depth, struct css_buf *out,
                                           struct css_expand_ctx *ctx)
{
    if (depth >= CSS_VAR_MAX_DEPTH || ctx->calls >= CSS_VAR_MAX_BYTES)
        return CSS_VAR_LIMIT;
    ctx->calls++;
    const char *p = text;
    const char *call;
    while ((call = find_var_call(p, text)) != NULL) {
        enum css_var_status lit_st = css_buf_append(out, p, (size_t)(call - p),
                                                    &ctx->bytes);
        if (lit_st != CSS_VAR_OK)
            return lit_st;
        const char *call_end, *name, *fb;
        size_t nlen, flen;
        bool has_fb;
        if (!parse_var_call(call, &call_end, &name, &nlen, &fb, &flen, &has_fb))
            return CSS_VAR_SYNTAX;
        const struct css_var *v = sheet_find_var(sheet, name, nlen);
        if (v) {
            for (int k = 0; k < stack_len; k++)
                if (strcmp(stack[k], v->name) == 0)
                    return CSS_VAR_CYCLE;
            stack[stack_len] = v->name;
            struct css_buf sub = {0};
            enum css_var_status st = expand_var_text(sheet, v->value, stack,
                                                     stack_len + 1, depth + 1,
                                                     &sub, ctx);
            if (st == CSS_VAR_OK) {
                enum css_var_status cp = css_buf_append(
                    out, sub.data ? sub.data : "", sub.len, NULL);
                free(sub.data);
                if (cp != CSS_VAR_OK)
                    return cp;
                p = call_end;
                continue;
            }
            free(sub.data);
            if (st != CSS_VAR_MISSING || !has_fb)
                return st;
        } else if (!has_fb) {
            return CSS_VAR_MISSING;
        }
        char *fb_copy = dup_range(fb, flen);
        if (!fb_copy)
            return CSS_VAR_OOM;
        /* Pop v->name before expanding the caller's fallback so sibling retries work. */
        enum css_var_status st = expand_var_text(sheet, fb_copy, stack,
                                                 stack_len, depth + 1, out, ctx);
        free(fb_copy);
        if (st != CSS_VAR_OK)
            return st;
        p = call_end;
    }
    return css_buf_append(out, p, strlen(p), &ctx->bytes);
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

static bool sheet_append_rule(struct css_sheet *sheet,
                              const struct css_rule *src,
                              const char *sel, size_t sel_len,
                              bool *out_supported)
{
    if (!sheet_reserve(sheet, 1))
        return false;
    struct css_rule *r = &sheet->rules[sheet->rule_count];
    *r = *src;
    r->set_mask = (struct css_prop_mask){0};
    r->selector = NULL;
    r->parts = NULL;
    r->part_cap = 0;
    r->classes = NULL;
    r->class_cap = 0;
    r->var_props = NULL;
    r->var_prop_count = 0;
    r->var_prop_cap = 0;
    if (out_supported) {
        bool supported = true;
        *out_supported = true;
        if (!parse_single_selector(r, sel, sel_len, &supported)) {
            rule_clear_selector(r);
            return false;
        }
        *out_supported = supported;
        if (!supported) {
            rule_clear_selector(r);
            return true;
        }
    } else {
        bool supported = true;
        if (!parse_single_selector(r, src->selector, strlen(src->selector),
                                   &supported)) {
            rule_clear_selector(r);
            return false;
        }
    }
    if (!rule_copy_var_props(r, src) || !prop_mask_copy(&r->set_mask, &src->set_mask)) {
        rule_clear_selector(r);
        return false;
    }
    sheet->rule_count++;
    return true;
}

/* A rule applies in tier order: an id selector beats a class selector, and a
   class selector beats a tag selector. More classes beat fewer classes. */
static uint32_t rule_tier(const struct css_rule *r)
{
    if (r->part_count == 0)
        return 0;
    switch (r->parts[r->part_count - 1].kind) {
    case CSS_KEY_ID:    return 2;
    case CSS_KEY_CLASS: return 1;
    default:            return 0;
    }
}

static uint32_t rule_rank(const struct css_rule *r)
{
    if (r->part_count == 0)
        return 1;
    const struct css_selector_part *last = &r->parts[r->part_count - 1];
    return last->kind == CSS_KEY_CLASS && last->class_count
               ? (uint32_t)last->class_count
               : 1;
}

static int cmp_u32(uint32_t a, uint32_t b)
{
    return (a > b) - (a < b);
}

struct rule_order {
    uint32_t tier;
    uint32_t rank;
    uint32_t pos;
};

static int cmp_rule_order(const void *pa, const void *pb)
{
    const struct rule_order *a = pa, *b = pb;
    int c = cmp_u32(a->tier, b->tier);
    if (c) return c;
    c = cmp_u32(a->rank, b->rank);
    return c ? c : cmp_u32(a->pos, b->pos);
}

/* Puts the rule table in apply order, so a lookup applies the rules with no
   sort. A later rule in the source wins a tie, because it applies last. */
static bool sheet_sort_rules(struct css_sheet *sheet)
{
    size_t n = (size_t)sheet->rule_count;
    if (n < 2)
        return true;
    struct rule_order *keys = malloc(n * sizeof(*keys));
    struct css_rule *sorted = malloc(n * sizeof(*sorted));
    if (!keys || !sorted) {
        free(keys);
        free(sorted);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        keys[i] = (struct rule_order){
            .tier = rule_tier(&sheet->rules[i]),
            .rank = rule_rank(&sheet->rules[i]),
            .pos = (uint32_t)i,
        };
    }
    qsort(keys, n, sizeof(*keys), cmp_rule_order);
    for (size_t i = 0; i < n; i++)
        sorted[i] = sheet->rules[keys[i].pos];
    memcpy(sheet->rules, sorted, n * sizeof(*sorted));
    free(keys);
    free(sorted);
    return true;
}

static void source_line_col(const char *source, const char *position,
                            size_t *out_line, size_t *out_col)
{
    size_t line = 1, column = 1;
    for (const char *p = source; p < position; p++) {
        if (*p == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }
    *out_line = line;
    *out_col = column;
}

static void report_at(css_diagnostic_fn report, void *userdata,
                      enum css_declaration_result result,
                      size_t line, size_t column,
                      const char *key, const char *value)
{
    if (!report || result == CSS_DECLARATION_OK)
        return;
    const struct css_diagnostic diagnostic = {
        .kind = result == CSS_DECLARATION_UNKNOWN ? CSS_DIAGNOSTIC_UNKNOWN_PROPERTY
              : result == CSS_DECLARATION_SYNTAX ? CSS_DIAGNOSTIC_INVALID_SYNTAX
              : result == CSS_DECLARATION_SELECTOR ? CSS_DIAGNOSTIC_UNSUPPORTED_SELECTOR
              : CSS_DIAGNOSTIC_INVALID_VALUE,
        .line = line,
        .column = column,
        .property = key,
        .value = value,
    };
    report(&diagnostic, userdata);
}

static void report_declaration(css_diagnostic_fn report, void *userdata,
                               enum css_declaration_result result,
                               const char *source, const char *position,
                               const char *key, const char *value)
{
    if (!report || result == CSS_DECLARATION_OK)
        return;
    size_t line, column;
    source_line_col(source, position, &line, &column);
    report_at(report, userdata, result, line, column, key, value);
}

const char *css_diagnostic_message(enum css_diagnostic_kind kind)
{
    switch (kind) {
    case CSS_DIAGNOSTIC_UNKNOWN_PROPERTY: return "unknown property";
    case CSS_DIAGNOSTIC_INVALID_VALUE: return "invalid value";
    case CSS_DIAGNOSTIC_INVALID_SYNTAX: return "invalid syntax";
    case CSS_DIAGNOSTIC_UNSUPPORTED_SELECTOR: return "unsupported selector";
    }
    return "CSS error";
}

/* Expands `val` and applies it to `rule`. Returns false when the expansion or
   the value fails, and `out_st` says which. */
static bool try_expand_and_apply(const struct css_sheet *sheet,
                                 struct css_rule *rule, size_t prop_idx,
                                 const char *val,
                                 enum css_var_status *out_st,
                                 enum css_declaration_result *out_res)
{
    const char *stack[CSS_VAR_MAX_DEPTH];
    struct css_buf buf = {0};
    struct css_expand_ctx ctx = {0};
    *out_st = expand_var_text(sheet, val, stack, 0, 0, &buf, &ctx);
    if (*out_st != CSS_VAR_OK) {
        free(buf.data);
        *out_res = CSS_DECLARATION_INVALID;
        return false;
    }
    struct css_rule tmp = { .style = rule->style };
    if (!prop_mask_copy(&tmp.set_mask, &rule->set_mask)) {
        free(buf.data);
        *out_st = CSS_VAR_OOM;
        *out_res = CSS_DECLARATION_INVALID;
        return false;
    }
    *out_res = apply_prop_at(&tmp, prop_idx, buf.data ? buf.data : "");
    free(buf.data);
    if (*out_res != CSS_DECLARATION_OK) {
        prop_mask_clear(&tmp.set_mask);
        return false;
    }
    rule->style = tmp.style;
    prop_mask_clear(&rule->set_mask);
    rule->set_mask = tmp.set_mask;
    return true;
}

/* Resolves every declaration that holds var() against the current variables.
   The declarations are independent of each other, in the order they appear in
   the rule. A failure reports the declaration line and the text that failed,
   and only once for as long as the failure stays. */
static bool sheet_resolve_vars(struct css_sheet *sheet,
                               css_diagnostic_fn report, void *userdata)
{
    for (int i = 0; i < sheet->rule_count; i++) {
        struct css_rule *r = &sheet->rules[i];
        for (int j = 0; j < r->var_prop_count; j++) {
            struct css_var_prop *vp = &r->var_props[j];
            enum css_var_status st = CSS_VAR_OK;
            enum css_declaration_result res = CSS_DECLARATION_OK;
            if (try_expand_and_apply(sheet, r, vp->prop_idx, vp->value, &st,
                                     &res)) {
                vp->reported = false;
                continue;
            }
            if (st == CSS_VAR_OOM)
                return false;
            if (vp->reported)
                continue;
            vp->reported = true;
            report_at(report, userdata, CSS_DECLARATION_INVALID,
                      (size_t)vp->line, (size_t)vp->column,
                      props[vp->prop_idx].name, vp->value);
        }
    }
    return true;
}

static bool handle_root_decl(struct css_sheet *sheet,
                             const char *source, const char *k_start,
                             const char *key, const char *val,
                             css_diagnostic_fn report, void *userdata)
{
    if (key[0] != '-' || key[1] != '-') {
        report_declaration(report, userdata, CSS_DECLARATION_UNKNOWN,
                           source, k_start, key, val);
        return true;
    }
    if (!is_valid_var_name(key, strlen(key)) || val[0] == '\0') {
        report_declaration(report, userdata, CSS_DECLARATION_INVALID,
                           source, k_start, key, val);
        return true;
    }
    size_t line, col;
    source_line_col(source, k_start, &line, &col);
    return sheet_set_var(sheet, key, val, (int)line, (int)col);
}

static bool handle_rule_decl(const struct css_sheet *sheet, struct css_rule *proto,
                             const char *source, const char *k_start,
                             const char *key, const char *val,
                             css_diagnostic_fn report, void *userdata)
{
    int idx = find_prop_idx(key);
    if (idx < 0) {
        report_declaration(report, userdata, CSS_DECLARATION_UNKNOWN,
                           source, k_start, key, val);
        return true;
    }
    if (!find_var_call(val, val)) {
        enum css_declaration_result result = apply_prop_at(proto, (size_t)idx, val);
        /* A declaration that sets the field replaces the earlier one that
           holds var(). One that leaves the field alone keeps it. */
        if (result == CSS_DECLARATION_OK &&
            prop_mask_has(&proto->set_mask, (size_t)idx))
            rule_drop_matching_var_props(proto, proto->var_prop_count, (size_t)idx);
        report_declaration(report, userdata, result, source, k_start, key, val);
        return true;
    }
    size_t line, col;
    source_line_col(source, k_start, &line, &col);
    enum css_var_status st = CSS_VAR_OK;
    enum css_declaration_result res = CSS_DECLARATION_OK;
    bool resolved = try_expand_and_apply(sheet, proto, (size_t)idx, val, &st, &res);
    if (st == CSS_VAR_OOM)
        return false;
    /* A value that resolves now replaces an earlier declaration of the same
       property. A value that waits for a later :root block keeps the earlier
       one until the pass at the end of the parse resolves it. */
    return rule_set_var_prop(proto, (size_t)idx, val, (int)line, (int)col,
                             resolved);
}

static bool parse_rules(struct css_sheet *sheet, const char *source,
                        css_diagnostic_fn report, void *userdata)
{
    bool ok = true;
    struct css_rule proto = {0};
    const char *p = source;
    while (*p) {
        p = skip_ws_and_comments(p);
        if (!*p) break;
        const char *s_start = p;
        while (*p && *p != '{') {
            if (p[0] == '/' && p[1] == '*') {
                p += 2;
                while (*p && !(p[0] == '*' && p[1] == '/')) p++;
                if (*p) p += 2;
            } else {
                p++;
            }
        }
        if (!*p) {
            report_declaration(report, userdata, CSS_DECLARATION_SYNTAX,
                               source, s_start, "rule", "missing '{'");
            break;
        }
        const char *s_end = p;
        bool is_root = selector_is_root(s_start, s_end);

        proto = (struct css_rule){0};
        p++;
        while (*p && *p != '}') {
            p = skip_ws_and_comments(p);
            if (!*p || *p == '}') break;
            const char *k_start = p;
            while (*p && *p != ':' && *p != ';' && *p != '}') p++;
            if (*p != ':') {
                report_declaration(report, userdata, CSS_DECLARATION_SYNTAX,
                                   source, k_start, "declaration", "missing ':'");
                if (*p == ';') {
                    p++;
                    continue;
                }
                break;
            }
            const char *k_end = p++;
            const char *v_start = p;
            while (*p && *p != ';' && *p != '}') p++;
            const char *v_end = p;
            if (*p == ';') p++;

            while (k_end > k_start && isspace((unsigned char)k_end[-1])) k_end--;
            while (v_start < v_end && isspace((unsigned char)*v_start)) v_start++;
            while (v_end > v_start && isspace((unsigned char)v_end[-1])) v_end--;
            char *key = dup_range(k_start, (size_t)(k_end - k_start));
            char *val = dup_range(v_start, (size_t)(v_end - v_start));
            if (!key || !val) {
                free(key);
                free(val);
                ok = false;
                goto done;
            }
            bool decl_ok = is_root
                ? handle_root_decl(sheet, source, k_start, key, val, report,
                                   userdata)
                : handle_rule_decl(sheet, &proto, source, k_start, key, val,
                                   report, userdata);
            free(key);
            free(val);
            if (!decl_ok) {
                ok = false;
                goto done;
            }
        }
        if (*p == '}') {
            p++;
        } else {
            report_declaration(report, userdata, CSS_DECLARATION_SYNTAX,
                               source, s_start, "rule", "missing '}'");
        }

        if (is_root) {
            rule_clear_vars(&proto);
            prop_mask_clear(&proto.set_mask);
            continue;
        }

        const char *sel_ptr = s_start;
        while (sel_ptr < s_end) {
            sel_ptr = skip_ws_and_comments(sel_ptr);
            if (sel_ptr > s_end) break;
            const char *comma = sel_ptr;
            while (comma < s_end && *comma != ',') {
                if (comma[0] == '/' && comma + 1 < s_end && comma[1] == '*') {
                    comma += 2;
                    while (comma + 1 < s_end && !(comma[0] == '*' && comma[1] == '/'))
                        comma++;
                    comma = comma + 1 < s_end ? comma + 2 : s_end;
                } else {
                    comma++;
                }
            }
            if (comma == s_end) comma = NULL;
            const char *chunk_end = comma ? comma : s_end;
            if (chunk_end > sel_ptr) {
                bool supported;
                if (!sheet_append_rule(sheet, &proto, sel_ptr,
                                       (size_t)(chunk_end - sel_ptr), &supported)) {
                    ok = false;
                    goto done;
                }
                if (!supported && report) {
                    char *selector = dup_range(sel_ptr, (size_t)(chunk_end - sel_ptr));
                    if (!selector) {
                        ok = false;
                        goto done;
                    }
                    report_declaration(report, userdata, CSS_DECLARATION_SELECTOR,
                                       source, sel_ptr, selector,
                                       "unsupported pseudo-class");
                    free(selector);
                }
            }
            if (!comma) break;
            sel_ptr = comma + 1;
        }
        rule_clear_vars(&proto);
        prop_mask_clear(&proto.set_mask);
    }
done:
    rule_clear_vars(&proto);
    prop_mask_clear(&proto.set_mask);
    return ok;
}

bool css_parse_with_diagnostics(struct css_sheet *sheet, const char *p,
                                css_diagnostic_fn report, void *userdata)
{
    if (!sheet || !p) return false;
    bool ok = parse_rules(sheet, p, report, userdata);
    if (!sheet_sort_rules(sheet))
        return false;
    if (!sheet_resolve_vars(sheet, report, userdata))
        ok = false;
    return sheet_build_index(sheet) && ok;
}

static void css_report_stderr(const struct css_diagnostic *diagnostic, void *userdata)
{
    (void)userdata;
    fprintf(stderr, "css:%zu:%zu: %s '%s: %s'\n",
            diagnostic->line, diagnostic->column,
            css_diagnostic_message(diagnostic->kind),
            diagnostic->property, diagnostic->value);
}

bool css_parse(struct css_sheet *sheet, const char *p)
{
    return css_parse_with_diagnostics(sheet, p, css_report_stderr, NULL);
}

void css_sheet_clear(struct css_sheet *sheet)
{
    if (!sheet)
        return;
    for (int i = 0; i < sheet->rule_count; i++) {
        rule_clear_selector(&sheet->rules[i]);
        rule_clear_vars(&sheet->rules[i]);
    }
    free(sheet->rules);
    free(sheet->index);
    for (int i = 0; i < sheet->var_count; i++) {
        free(sheet->vars[i].name);
        free(sheet->vars[i].value);
    }
    free(sheet->vars);
    *sheet = (struct css_sheet){0};
}

static bool append_vars(struct css_sheet *dst, const struct css_sheet *src)
{
    for (int i = 0; i < src->var_count; i++) {
        const struct css_var *v = &src->vars[i];
        if (!sheet_set_var(dst, v->name, v->value, v->line, v->column))
            return false;
    }
    return true;
}

static bool append_rules(struct css_sheet *dst, const struct css_sheet *src)
{
    if (!sheet_reserve(dst, src->rule_count))
        return false;
    for (int i = 0; i < src->rule_count; i++) {
        if (!sheet_append_rule(dst, &src->rules[i], NULL, 0, NULL))
            return false;
    }
    return true;
}

bool css_sheet_append(struct css_sheet *dst, const struct css_sheet *src)
{
    if (!dst || !src || dst == src)
        return false;
    bool ok = append_vars(dst, src) && append_rules(dst, src);
    dst->has_defaults = dst->has_defaults || src->has_defaults;
    if (!sheet_sort_rules(dst))
        return false;
    /* A variable of the appended sheet can change a rule of the sheet it
       lands on, so every declaration resolves again. */
    if (!sheet_resolve_vars(dst, NULL, NULL))
        ok = false;
    return sheet_build_index(dst) && ok;
}

void css_init_default(struct css_sheet *sheet)
{
    if (!sheet) return;
    css_sheet_clear(sheet);
    sheet->has_defaults = css_parse(sheet, kUiCoreCss);
}

static struct css_sheet default_sheet;
static bool default_sheet_ready;

const struct css_sheet *css_default_sheet(void)
{
    if (!default_sheet_ready) {
        css_init_default(&default_sheet);
        default_sheet_ready = true;
    }
    return &default_sheet;
}

bool css_parse_app(struct css_sheet *sheet)
{
    if (!sheet)
        return false;
    return css_parse(sheet, kUiAppCss);
}

void css_shutdown(void)
{
    if (default_sheet_ready) {
        css_sheet_clear(&default_sheet);
        default_sheet_ready = false;
    }
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

static bool part_matches(const struct css_rule *r,
                         const struct css_selector_part *part,
                         const struct css_element *element)
{
    const char *text = r->selector + part->offset;
    switch (part->kind) {
    case CSS_KEY_CLASS:
        if (!element->class_name)
            return false;
        for (uint16_t i = 0; i < part->class_count; i++) {
            const struct css_class_token *token = &r->classes[part->class_first + i];
            if (!class_list_has(element->class_name,
                                strlen(element->class_name),
                                r->selector + token->offset, token->length))
                return false;
        }
        return true;
    case CSS_KEY_ID:
        return range_equals(text + 1, part->length - 1, element->id_name);
    case CSS_KEY_TAG:
        return range_equals(text, part->length, element->tag_name);
    case CSS_KEY_NONE:
        break;
    }
    return false;
}

/* A selector applies when its last part matches `element` and each part to its
   left matches an ancestor above the match of the part to its right. */
static bool rule_matches(const struct css_rule *r,
                         const struct css_element *element,
                         unsigned pseudo_flags,
                         const struct css_element *ancestors,
                         size_t ancestor_count)
{
    if (r->pseudo != CSS_PSEUDO_NONE && !(pseudo_flags & r->pseudo))
        return false;
    if (r->part_count == 0)
        return false;
    if (!part_matches(r, &r->parts[r->part_count - 1], element))
        return false;
    size_t cursor = ancestor_count;
    for (int i = (int)r->part_count - 2; i >= 0; i--) {
        bool found = false;
        while (cursor > 0) {
            cursor--;
            if (part_matches(r, &r->parts[i], &ancestors[cursor])) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static void apply_rule_props(const struct css_rule *r, struct css_style *out)
{
    for (uint32_t word = 0; word < r->set_mask.words; word++) {
        uint64_t mask = r->set_mask.bits[word];
        while (mask) {
            int bit = __builtin_ctzll(mask);
            mask &= ~(1ULL << bit);
            size_t index = (size_t)word * 64 + (size_t)bit;
            if (index >= prop_count)
                continue;
            const struct prop_info *p = &props[index];
            char *dst = (char *)out + p->offset;
            const char *src = (const char *)&r->style + p->offset;
            switch ((enum prop_kind)p->kind) {
            case P_INSETS: {
                const struct css_value *srcv = (const struct css_value *)src;
                struct css_value *dstv = (struct css_value *)dst;
                for (int e = 0; e < 4; e++)
                    if (srcv[e].unit != CSS_UNIT_NONE)
                        dstv[e] = srcv[e];
                break;
            }
            case P_PAD_EDGE:
            case P_MARGIN_EDGE:
            case P_BORDER_EDGE:
            case P_INSET: {
                struct css_value *dstv = (struct css_value *)dst;
                const struct css_value *srcv = (const struct css_value *)src;
                dstv[p->size] = srcv[p->size];
                break;
            }
            case P_FLEX_DIR: out->flex_direction = r->style.flex_direction; break;
            case P_ALIGN:    out->align_items = r->style.align_items; break;
            case P_JUSTIFY:  out->justify_content = r->style.justify_content; break;
            case P_CURSOR:   out->cursor = r->style.cursor; break;
            case P_WRAP:     out->flex_wrap = r->style.flex_wrap; break;
            case P_ALIGN_SELF: out->align_self = r->style.align_self; break;
            case P_FONT_STYLE: out->font_style = r->style.font_style; break;
            case P_OVERFLOW: out->overflow = r->style.overflow; break;
            case P_POSITION: out->position = r->style.position; break;
            case P_FONT_WEIGHT: out->font_weight = r->style.font_weight; break;
            case P_INT:      out->z_index = r->style.z_index; break;
            default:         memcpy(dst, src, p->size); break;
            }
            if (p->has_bit)
                set_style_has_bit(out, p->has_bit,
                                  get_style_has_bit(&r->style, p->has_bit));
        }
    }
}

static int index_cmp_key(enum css_key_kind kind, const char *key, size_t len,
                         const struct css_index_entry *entry)
{
    if (kind != entry->kind)
        return kind < entry->kind ? -1 : 1;
    size_t n = entry->key_len < len ? entry->key_len : len;
    int c = memcmp(key, entry->key, n);
    if (c)
        return c < 0 ? -1 : 1;
    if (len == entry->key_len)
        return 0;
    return len < entry->key_len ? -1 : 1;
}

static int cmp_index_entry(const void *pa, const void *pb)
{
    const struct css_index_entry *a = pa, *b = pb;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    size_t n = a->key_len < b->key_len ? a->key_len : b->key_len;
    int c = memcmp(a->key, b->key, n);
    if (c)
        return c;
    if (a->key_len != b->key_len)
        return a->key_len < b->key_len ? -1 : 1;
    return (a->rule > b->rule) - (a->rule < b->rule);
}

/* Indexes every rule under the key of its last compound. A lookup then reads
   only the rules that the key of one element can select. */
static bool sheet_build_index(struct css_sheet *sheet)
{
    free(sheet->index);
    sheet->index = NULL;
    sheet->index_count = 0;
    sheet->index_ready = false;
    int count = 0;
    for (int i = 0; i < sheet->rule_count; i++) {
        const struct css_rule *r = &sheet->rules[i];
        if (r->part_count == 0)
            continue;
        if (r->parts[r->part_count - 1].kind != CSS_KEY_NONE)
            count++;
    }
    if (count == 0) {
        sheet->index_ready = true;
        return true;
    }
    struct css_index_entry *index = malloc((size_t)count * sizeof(*index));
    if (!index)
        return false;
    int at = 0;
    for (int i = 0; i < sheet->rule_count; i++) {
        const struct css_rule *r = &sheet->rules[i];
        if (r->part_count == 0)
            continue;
        const struct css_selector_part *last = &r->parts[r->part_count - 1];
        if (last->kind == CSS_KEY_NONE)
            continue;
        const char *text = r->selector + last->offset;
        struct css_index_entry *entry = &index[at++];
        entry->kind = last->kind;
        entry->rule = (uint32_t)i;
        if (last->kind == CSS_KEY_CLASS) {
            if (last->class_count == 0) {
                /* A lone point matches any class list, so its key is empty. */
                entry->key = "";
                entry->key_len = 0;
            } else {
                const struct css_class_token *token = &r->classes[last->class_first];
                entry->key = r->selector + token->offset;
                entry->key_len = token->length;
            }
        } else if (last->kind == CSS_KEY_ID) {
            entry->key = text + 1;
            entry->key_len = (uint16_t)(last->length - 1);
        } else {
            entry->key = text;
            entry->key_len = last->length;
        }
    }
    qsort(index, (size_t)count, sizeof(*index), cmp_index_entry);
    sheet->index = index;
    sheet->index_count = count;
    sheet->index_ready = true;
    return true;
}

/* Finds the first entry whose key is not less than the query key. */
static int index_lower_bound(const struct css_sheet *sheet, enum css_key_kind kind,
                             const char *key, size_t len)
{
    int lo = 0, hi = sheet->index_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (index_cmp_key(kind, key, len, &sheet->index[mid]) > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

#define CANDIDATE_INLINE 128

/* Adds the rules that the key selects to `candidates`, keeping them in apply
   order. Returns false when a candidate does not fit. */
static bool candidates_add_key(const struct css_sheet *sheet, enum css_key_kind kind,
                               const char *key, size_t len, uint32_t *candidates,
                               size_t *count, size_t cap)
{
    for (int i = index_lower_bound(sheet, kind, key, len);
         i < sheet->index_count &&
         index_cmp_key(kind, key, len, &sheet->index[i]) == 0; i++) {
        uint32_t rule = sheet->index[i].rule;
        if (*count == cap)
            return false;
        size_t at = *count;
        while (at > 0 && candidates[at - 1] > rule) {
            candidates[at] = candidates[at - 1];
            at--;
        }
        candidates[at] = rule;
        (*count)++;
    }
    return true;
}

static bool candidates_add_classes(const struct css_sheet *sheet, const char *list,
                                   uint32_t *candidates, size_t *count, size_t cap)
{
    if (!list)
        return true;
    for (const char *p = list; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (p == start)
            break;
        if (!candidates_add_key(sheet, CSS_KEY_CLASS, start, (size_t)(p - start),
                                candidates, count, cap))
            return false;
    }
    return true;
}

/* Collects the rules that can match `element` and applies them in apply order. */
static void apply_candidates(const struct css_sheet *sheet,
                             const struct css_element *element,
                             unsigned pseudo_flags,
                             const struct css_element *ancestors,
                             size_t ancestor_count, const uint32_t *candidates,
                             size_t count, struct css_style *out)
{
    uint32_t previous = UINT32_MAX;
    for (size_t i = 0; i < count; i++) {
        uint32_t rule = candidates[i];
        if (rule == previous)
            continue;
        previous = rule;
        const struct css_rule *r = &sheet->rules[rule];
        if (rule_matches(r, element, pseudo_flags, ancestors, ancestor_count))
            apply_rule_props(r, out);
    }
}

/* Applies the rules that the rule index selects for `element`. Returns false
   when the candidate list needs heap storage. */
static bool apply_indexed(const struct css_sheet *sheet,
                          const struct css_element *element, unsigned pseudo_flags,
                          const struct css_element *ancestors,
                          size_t ancestor_count, struct css_style *out)
{
    uint32_t stack_candidates[CANDIDATE_INLINE];
    size_t count = 0;
    bool fits = true;
    if (element->class_name) {
        fits = candidates_add_key(sheet, CSS_KEY_CLASS, "", 0, stack_candidates,
                                  &count, CANDIDATE_INLINE);
        if (fits)
            fits = candidates_add_classes(sheet, element->class_name,
                                          stack_candidates, &count,
                                          CANDIDATE_INLINE);
    }
    if (fits && element->id_name)
        fits = candidates_add_key(sheet, CSS_KEY_ID, element->id_name,
                                  strlen(element->id_name), stack_candidates,
                                  &count, CANDIDATE_INLINE);
    if (fits && element->tag_name)
        fits = candidates_add_key(sheet, CSS_KEY_TAG, element->tag_name,
                                  strlen(element->tag_name), stack_candidates,
                                  &count, CANDIDATE_INLINE);
    if (!fits)
        return false;
    apply_candidates(sheet, element, pseudo_flags, ancestors, ancestor_count,
                     stack_candidates, count, out);
    return true;
}

void css_style_resolve_node(const struct css_sheet *sheet,
                            const struct css_element *element,
                            unsigned pseudo_flags,
                            const struct css_element *ancestors,
                            size_t ancestor_count,
                            struct css_style *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->opacity = 1.0f;
    if (!sheet || !element) return;

    /* The table is in apply order. A rule that matches replaces the props of
       the rules before it. */
    if (sheet->index_ready && apply_indexed(sheet, element, pseudo_flags,
                                            ancestors, ancestor_count, out))
        return;
    memset(out, 0, sizeof(*out));
    out->opacity = 1.0f;
    for (int i = 0; i < sheet->rule_count; i++) {
        const struct css_rule *r = &sheet->rules[i];
        if (rule_matches(r, element, pseudo_flags, ancestors, ancestor_count))
            apply_rule_props(r, out);
    }
}

void css_style_resolve(const struct css_sheet *sheet, const char *class_name,
                       const char *id_name, const char *tag_name, unsigned pseudo_flags,
                       struct css_style *out)
{
    const struct css_element element = {
        .class_name = class_name,
        .id_name = id_name,
        .tag_name = tag_name,
    };
    css_style_resolve_node(sheet, &element, pseudo_flags, NULL, 0, out);
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
           box_eq(&a->border_edges, &b->border_edges) &&
           value_eq(a->gap, b->gap) &&
           value_eq(a->border_width, b->border_width) &&
           a->has_flex_direction == b->has_flex_direction &&
           a->flex_direction == b->flex_direction &&
           a->has_align_items == b->has_align_items &&
           a->align_items == b->align_items &&
           a->has_justify_content == b->has_justify_content &&
           a->justify_content == b->justify_content &&
           a->has_flex_wrap == b->has_flex_wrap &&
           a->flex_wrap == b->flex_wrap &&
           a->has_flex_grow == b->has_flex_grow &&
           a->flex_grow == b->flex_grow &&
           a->has_flex_shrink == b->has_flex_shrink &&
           a->flex_shrink == b->flex_shrink &&
           a->has_align_self == b->has_align_self &&
           a->align_self == b->align_self;
}
