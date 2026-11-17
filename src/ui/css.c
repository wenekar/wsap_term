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

enum prop_kind { P_VAL, P_INSETS, P_COLOR, P_FLOAT, P_FLEX_DIR, P_ALIGN, P_JUSTIFY, P_CURSOR, P_DURATION, P_SHADOW };

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
    PROP("border",           P_VAL,      border_width),
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
    PROP_ENUM("cursor",             P_CURSOR,   has_cursor),
    PROP_HAS("transition-duration", P_DURATION, transition_duration, has_transition_duration),
    PROP_HAS("animation-duration",  P_DURATION, transition_duration, has_transition_duration),
    PROP_HAS("transition",          P_DURATION, transition_duration, has_transition_duration),
    PROP_SHADOW("box-shadow",       shadow_x,                        has_shadow),
    PROP_COLOR("ripple-color", ripple_color, has_ripple_color),
    PROP_COLOR("ripple",       ripple_color, has_ripple_color),
};

static const size_t prop_count = sizeof(props) / sizeof(props[0]);
static_assert(sizeof(props) / sizeof(props[0]) <= 64,
              "The rule set mask holds one bit per property");
static_assert(sizeof(props) / sizeof(props[0]) <= UINT16_MAX,
              "Property indices must fit in uint16_t");
static_assert(CSS_SHADOW_SIZE <= UINT8_MAX,
              "Shadow fields must fit in prop_info.size");
static_assert(HAS_NONE == 0 && HAS_has_justify_content <= UINT8_MAX,
              "Zero has_bit must mean no presence flag");

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
        case P_SHADOW:
            if (!parse_box_shadow(val, &r->style)) return CSS_DECLARATION_INVALID;
            break;
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
    if (p->has_bit && p->kind != P_SHADOW)
        set_style_has_bit(&r->style, p->has_bit, true);
    r->set_mask |= (1ULL << idx);
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

static bool parse_single_selector(struct css_rule *r, const char *s, size_t len,
                                  bool *supported)
{
    *supported = true;
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
        else *supported = false;
    }
    if (memchr(r->selector, ':', r->last_off))
        *supported = false;
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

#define CSS_VAR_MAX_DEPTH 32
#define CSS_VAR_MAX_BYTES (64 * 1024)

static const char css_var_absent_sentinel;
#define VAR_OVERRIDE_ABSENT (&css_var_absent_sentinel)

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

struct css_var_change {
    int var_idx;
    char *old_value; /* NULL when this pass adds the variable. */
};

struct css_var_tracker {
    struct css_var_change *changes;
    int count;
    int cap;
};

struct css_expand_ctx {
    size_t bytes;
    size_t calls;
    const int *dirty_map; /* sheet->vars[i] -> tracker change index, or -1 */
    const char *const *overrides;
    const struct css_var *blame;
    const struct css_var *first_dirty;
    bool blame_dirty;
    bool touched_dirty;
    bool touched_untouched;
    bool saw_dirty;
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

static void var_tracker_clear(struct css_var_tracker *t)
{
    for (int i = 0; i < t->count; i++)
        free(t->changes[i].old_value);
    free(t->changes);
    *t = (struct css_var_tracker){0};
}

static bool var_tracker_record(struct css_var_tracker *t, int var_idx,
                               const char *old_value)
{
    for (int i = 0; i < t->count; i++) {
        if (t->changes[i].var_idx == var_idx)
            return true;
    }
    char *old_copy = NULL;
    if (old_value) {
        old_copy = strdup(old_value);
        if (!old_copy)
            return false;
    }
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        struct css_var_change *next = realloc(t->changes,
                                              (size_t)cap * sizeof(*next));
        if (!next) {
            free(old_copy);
            return false;
        }
        t->changes = next;
        t->cap = cap;
    }
    t->changes[t->count++] = (struct css_var_change){
        .var_idx = var_idx,
        .old_value = old_copy,
    };
    return true;
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

static int rule_drop_matching_var_props(struct css_rule *r, int limit_idx,
                                        size_t prop_idx)
{
    uint16_t off = props[prop_idx].offset;
    uint8_t kind = props[prop_idx].kind;
    int dropped = 0;
    for (int i = limit_idx - 1; i >= 0; i--) {
        size_t cur = r->var_props[i].prop_idx;
        if (props[cur].offset == off && props[cur].kind == kind) {
            rule_remove_var_prop(r, i);
            dropped++;
        }
    }
    return limit_idx - dropped;
}

static bool rule_set_var_prop(struct css_rule *r, size_t prop_idx, const char *val,
                              int line, int col, bool pending)
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
    /* Preserve earlier live bindings while a later var() awaits :root definitions. */
    if (!pending)
        rule_drop_matching_var_props(r, r->var_prop_count, prop_idx);
    r->var_props[r->var_prop_count++] = (struct css_var_prop){
        .prop_idx = (uint16_t)prop_idx,
        .value = copy,
        .line = line,
        .column = col,
        .pending = pending,
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

static bool sheet_set_var(struct css_sheet *sheet, struct css_var_tracker *tracker,
                          const char *name, const char *val, int line, int col)
{
    char *vcopy = strdup(val);
    if (!vcopy)
        return false;
    for (int i = 0; i < sheet->var_count; i++) {
        if (strcmp(sheet->vars[i].name, name) == 0) {
            if (tracker && !var_tracker_record(tracker, i, sheet->vars[i].value)) {
                free(vcopy);
                return false;
            }
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
    int idx = sheet->var_count;
    if (tracker && !var_tracker_record(tracker, idx, NULL)) {
        free(ncopy);
        free(vcopy);
        return false;
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

static const struct css_var *lookup_var_value(const struct css_sheet *sheet,
                                              const char *name, size_t nlen,
                                              struct css_expand_ctx *ctx,
                                              const char **out_val,
                                              bool *out_dirty)
{
    const struct css_var *v = sheet_find_var(sheet, name, nlen);
    if (!v) {
        *out_val = NULL;
        *out_dirty = false;
        return NULL;
    }
    int c = ctx->dirty_map ? ctx->dirty_map[v - sheet->vars] : -1;
    if (c >= 0 && ctx->overrides && ctx->overrides[c] == VAR_OVERRIDE_ABSENT) {
        *out_val = NULL;
        *out_dirty = false;
        return NULL;
    }
    *out_dirty = (c >= 0);
    if (*out_dirty) {
        ctx->touched_dirty = true;
        if (!ctx->first_dirty)
            ctx->first_dirty = v;
    } else if (ctx->dirty_map) {
        ctx->touched_untouched = true;
    }
    *out_val = (c >= 0 && ctx->overrides && ctx->overrides[c])
        ? ctx->overrides[c]
        : v->value;
    return v;
}

static void record_var_blame(struct css_expand_ctx *ctx, const struct css_var *v,
                             bool v_dirty, bool prefer_dirty)
{
    if (!ctx->blame || (prefer_dirty && v_dirty && !ctx->blame_dirty)) {
        ctx->blame = v;
        ctx->blame_dirty = v_dirty;
    }
    if (v_dirty)
        ctx->saw_dirty = true;
}

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
        const char *val = NULL;
        bool v_dirty = false;
        const struct css_var *v = lookup_var_value(sheet, name, nlen, ctx,
                                                   &val, &v_dirty);
        if (v) {
            for (int k = 0; k < stack_len; k++) {
                if (strcmp(stack[k], v->name) == 0) {
                    record_var_blame(ctx, v, v_dirty, true);
                    return CSS_VAR_CYCLE;
                }
            }
            stack[stack_len] = v->name;
            struct css_buf sub = {0};
            enum css_var_status st = expand_var_text(sheet, val, stack,
                                                     stack_len + 1, depth + 1,
                                                     &sub, ctx);
            if (st == CSS_VAR_OK) {
                enum css_var_status cp = css_buf_append(
                    out, sub.data ? sub.data : "", sub.len, NULL);
                free(sub.data);
                if (cp != CSS_VAR_OK) {
                    record_var_blame(ctx, v, v_dirty, false);
                    return cp;
                }
                p = call_end;
                continue;
            }
            free(sub.data);
            if (st != CSS_VAR_MISSING || !has_fb) {
                record_var_blame(ctx, v, v_dirty, st == CSS_VAR_CYCLE);
                return st;
            }
            if (v_dirty)
                ctx->saw_dirty = true;
            ctx->blame = NULL;
            ctx->blame_dirty = false;
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
    r->var_props = NULL;
    r->var_prop_count = 0;
    r->var_prop_cap = 0;
    if (out_supported) {
        if (!parse_single_selector(r, sel, sel_len, out_supported))
            return false;
        if (!*out_supported) {
            free(r->selector);
            r->selector = NULL;
            return true;
        }
    } else {
        r->selector = strdup(src->selector);
        if (!r->selector)
            return false;
    }
    if (!rule_copy_var_props(r, src)) {
        free(r->selector);
        r->selector = NULL;
        return false;
    }
    sheet->rule_count++;
    return true;
}

/* A rule applies in tier order: an id selector beats a class selector, and a
   class selector beats a tag selector. More classes beat fewer classes. */
static uint32_t rule_tier(const struct css_rule *r)
{
    return r->is_id ? 2 : r->is_class ? 1 : 0;
}

static uint32_t rule_rank(const struct css_rule *r)
{
    return r->is_class ? (uint32_t)r->class_rank : 1;
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

static bool try_expand_and_apply(const struct css_sheet *sheet,
                                 struct css_rule *rule, size_t prop_idx,
                                 const char *val, const int *dirty_map,
                                 const char *const *overrides,
                                 enum css_var_status *out_st,
                                 enum css_declaration_result *out_res,
                                 struct css_expand_ctx *out_ctx)
{
    const char *stack[CSS_VAR_MAX_DEPTH];
    struct css_buf buf = {0};
    struct css_expand_ctx ctx = {
        .dirty_map = dirty_map,
        .overrides = overrides,
    };
    *out_st = expand_var_text(sheet, val, stack, 0, 0, &buf, &ctx);
    *out_ctx = ctx;
    if (*out_st != CSS_VAR_OK) {
        free(buf.data);
        *out_res = CSS_DECLARATION_INVALID;
        return false;
    }
    struct css_rule tmp = {
        .style = rule->style,
        .set_mask = rule->set_mask,
    };
    *out_res = apply_prop_at(&tmp, prop_idx, buf.data ? buf.data : "");
    free(buf.data);
    if (*out_res != CSS_DECLARATION_OK)
        return false;
    rule->style = tmp.style;
    rule->set_mask = tmp.set_mask;
    return true;
}

static bool probe_var_prop(const struct css_sheet *sheet,
                           const struct css_rule *rule,
                           const struct css_var_prop *vp,
                           const int *dirty_map,
                           const char *const *overrides,
                           enum css_var_status *out_st)
{
    struct css_rule tmp = {
        .style = rule->style,
        .set_mask = rule->set_mask,
    };
    enum css_declaration_result res = CSS_DECLARATION_OK;
    struct css_expand_ctx ctx = {0};
    return try_expand_and_apply(sheet, &tmp, vp->prop_idx, vp->value,
                                dirty_map, overrides, out_st, &res, &ctx);
}

static bool isolate_failing_var(const struct css_sheet *sheet,
                                const struct css_rule *rule,
                                const struct css_var_prop *vp,
                                const struct css_var_tracker *tracker,
                                const int *dirty_map,
                                const struct css_expand_ctx *fail_ctx,
                                const struct css_var **out_blame,
                                bool *out_skip_report)
{
    *out_blame = NULL;
    *out_skip_report = false;
    if (!tracker || tracker->count <= 0 || !dirty_map)
        return true;

    const char **overrides = calloc((size_t)tracker->count, sizeof(*overrides));
    if (!overrides)
        return false;
    for (int c = 0; c < tracker->count; c++) {
        overrides[c] = tracker->changes[c].old_value
            ? tracker->changes[c].old_value
            : VAR_OVERRIDE_ABSENT;
    }

    enum css_var_status st = CSS_VAR_OK;
    bool base_ok = probe_var_prop(sheet, rule, vp, dirty_map, overrides, &st);
    if (st == CSS_VAR_OOM) {
        free(overrides);
        return false;
    }
    if (!base_ok) {
        if (fail_ctx->touched_untouched)
            *out_skip_report = true;
        else
            *out_blame = fail_ctx->first_dirty;
        free(overrides);
        return true;
    }

    for (int c = 0; c < tracker->count; c++) {
        const char *saved = overrides[c];
        overrides[c] = NULL;
        bool ok = probe_var_prop(sheet, rule, vp, dirty_map, overrides, &st);
        overrides[c] = saved;
        if (st == CSS_VAR_OOM) {
            free(overrides);
            return false;
        }
        if (!ok) {
            *out_blame = &sheet->vars[tracker->changes[c].var_idx];
            free(overrides);
            return true;
        }
    }

    *out_blame = fail_ctx->first_dirty;
    free(overrides);
    return true;
}

static bool build_dirty_map(const struct css_sheet *sheet,
                            const struct css_var_tracker *tracker,
                            int **out_map)
{
    *out_map = NULL;
    if (!tracker || tracker->count <= 0 || sheet->var_count <= 0)
        return true;
    int *map = malloc((size_t)sheet->var_count * sizeof(*map));
    if (!map)
        return false;
    for (int i = 0; i < sheet->var_count; i++)
        map[i] = -1;
    for (int c = 0; c < tracker->count; c++)
        map[tracker->changes[c].var_idx] = c;
    *out_map = map;
    return true;
}

static bool report_refresh_failure(const struct css_sheet *sheet,
                                   const struct css_rule *rule,
                                   const struct css_var_prop *vp,
                                   const struct css_var_tracker *tracker,
                                   const int *dirty_map,
                                   enum css_var_status st,
                                   const struct css_expand_ctx *ctx,
                                   css_diagnostic_fn report, void *userdata)
{
    if (!report)
        return true;
    if (vp->pending) {
        report_at(report, userdata, CSS_DECLARATION_INVALID,
                  (size_t)vp->line, (size_t)vp->column,
                  props[vp->prop_idx].name, vp->value);
        return true;
    }
    const struct css_var *blame = NULL;
    if (st != CSS_VAR_OK) {
        if (!ctx->saw_dirty)
            return true;
        blame = ctx->blame;
    } else {
        if (!ctx->touched_dirty)
            return true;
        bool skip = false;
        if (!isolate_failing_var(sheet, rule, vp, tracker, dirty_map, ctx,
                                 &blame, &skip))
            return false;
        if (skip)
            return true;
    }
    if (blame) {
        report_at(report, userdata, CSS_DECLARATION_INVALID,
                  (size_t)blame->line, (size_t)blame->column,
                  blame->name, blame->value);
    } else {
        report_at(report, userdata, CSS_DECLARATION_INVALID,
                  (size_t)vp->line, (size_t)vp->column,
                  props[vp->prop_idx].name, vp->value);
    }
    return true;
}

static bool sheet_refresh_vars(struct css_sheet *sheet,
                               const struct css_var_tracker *tracker,
                               int refresh_limit, int force_from,
                               css_diagnostic_fn report, void *userdata)
{
    int *dirty_map = NULL;
    if (!build_dirty_map(sheet, tracker, &dirty_map))
        return false;
    bool vars_updated = dirty_map != NULL;

    for (int i = 0; i < sheet->rule_count; i++) {
        struct css_rule *r = &sheet->rules[i];
        for (int j = 0; j < r->var_prop_count;) {
            struct css_var_prop *vp = &r->var_props[j];
            bool force_eval = vp->pending || i >= force_from;
            if (!force_eval && (!vars_updated || i >= refresh_limit)) {
                j++;
                continue;
            }
            const int *prop_dirty = force_eval ? NULL : dirty_map;
            enum css_var_status st = CSS_VAR_OK;
            enum css_declaration_result res = CSS_DECLARATION_OK;
            struct css_expand_ctx ctx = {0};
            if (try_expand_and_apply(sheet, r, vp->prop_idx, vp->value,
                                     prop_dirty, NULL, &st, &res, &ctx)) {
                if (vp->pending) {
                    vp->pending = false;
                    j = rule_drop_matching_var_props(r, j, vp->prop_idx);
                }
                j++;
                continue;
            }
            if (st == CSS_VAR_OOM) {
                free(dirty_map);
                return false;
            }
            if (!report_refresh_failure(sheet, r, vp, tracker, dirty_map, st,
                                        &ctx, report, userdata)) {
                free(dirty_map);
                return false;
            }
            if (vp->pending) {
                rule_remove_var_prop(r, j);
                continue;
            }
            j++;
        }
    }
    free(dirty_map);
    return true;
}

static bool handle_root_decl(struct css_sheet *sheet,
                             struct css_var_tracker *tracker,
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
    return sheet_set_var(sheet, tracker, key, val, (int)line, (int)col);
}

static bool handle_rule_decl(const struct css_sheet *sheet, struct css_rule *proto,
                             const char *source, const char *k_start,
                             const char *key, const char *val,
                             bool *any_pending, css_diagnostic_fn report,
                             void *userdata)
{
    int idx = find_prop_idx(key);
    if (idx < 0) {
        report_declaration(report, userdata, CSS_DECLARATION_UNKNOWN,
                           source, k_start, key, val);
        return true;
    }
    if (!find_var_call(val, val)) {
        enum css_declaration_result result = apply_prop_at(proto, (size_t)idx, val);
        if (result == CSS_DECLARATION_OK)
            rule_drop_matching_var_props(proto, proto->var_prop_count, (size_t)idx);
        report_declaration(report, userdata, result, source, k_start, key, val);
        return true;
    }
    size_t line, col;
    source_line_col(source, k_start, &line, &col);
    enum css_var_status st = CSS_VAR_OK;
    enum css_declaration_result res = CSS_DECLARATION_OK;
    struct css_expand_ctx ctx = {0};
    if (try_expand_and_apply(sheet, proto, (size_t)idx, val, NULL, NULL,
                             &st, &res, &ctx)) {
        return rule_set_var_prop(proto, (size_t)idx, val, (int)line, (int)col,
                                 false);
    }
    if (st == CSS_VAR_OOM)
        return false;
    if (st == CSS_VAR_MISSING) {
        *any_pending = true;
        return rule_set_var_prop(proto, (size_t)idx, val, (int)line, (int)col,
                                 true);
    }
    report_declaration(report, userdata, res, source, k_start, key, val);
    return true;
}

static bool parse_rules(struct css_sheet *sheet, const char *source,
                        css_diagnostic_fn report, void *userdata)
{
    int refresh_limit = sheet->rule_count;
    struct css_var_tracker tracker = {0};
    bool any_pending = false;
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
                ? handle_root_decl(sheet, &tracker, source, k_start, key, val,
                                   report, userdata)
                : handle_rule_decl(sheet, &proto, source, k_start, key, val,
                                   &any_pending, report, userdata);
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
            refresh_limit = sheet->rule_count;
            rule_clear_vars(&proto);
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
    }
    if (tracker.count > 0 || any_pending) {
        if (!sheet_refresh_vars(sheet, &tracker, refresh_limit,
                                sheet->rule_count, report, userdata))
            ok = false;
    }
done:
    rule_clear_vars(&proto);
    var_tracker_clear(&tracker);
    return ok;
}

bool css_parse_with_diagnostics(struct css_sheet *sheet, const char *p,
                                css_diagnostic_fn report, void *userdata)
{
    if (!sheet || !p) return false;
    bool ok = parse_rules(sheet, p, report, userdata);
    return sheet_sort_rules(sheet) && ok;
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
        free(sheet->rules[i].selector);
        rule_clear_vars(&sheet->rules[i]);
    }
    free(sheet->rules);
    for (int i = 0; i < sheet->var_count; i++) {
        free(sheet->vars[i].name);
        free(sheet->vars[i].value);
    }
    free(sheet->vars);
    *sheet = (struct css_sheet){0};
}

static bool append_vars(struct css_sheet *dst, const struct css_sheet *src,
                        struct css_var_tracker *tracker)
{
    for (int i = 0; i < src->var_count; i++) {
        const struct css_var *v = &src->vars[i];
        if (!sheet_set_var(dst, tracker, v->name, v->value, v->line, v->column))
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
    int old_rule_count = dst->rule_count;
    int old_var_count = dst->var_count;
    struct css_var_tracker tracker = {0};
    bool ok = append_vars(dst, src, &tracker) && append_rules(dst, src);
    dst->has_defaults = dst->has_defaults || src->has_defaults;
    int force_from = old_var_count > 0 ? old_rule_count : dst->rule_count;
    if (ok && (tracker.count > 0 || force_from < dst->rule_count)) {
        ok = sheet_refresh_vars(dst, &tracker, old_rule_count, force_from,
                                NULL, NULL);
    }
    var_tracker_clear(&tracker);
    return sheet_sort_rules(dst) && ok;
}

void css_init_default(struct css_sheet *sheet)
{
    if (!sheet) return;
    css_sheet_clear(sheet);
    bool ok = css_parse(sheet,
        ":root {"
        " --surface: #1e1e2e;"
        " --border: #45475a;"
        " --fg: #cdd6f4;"
        " --muted: #7f849c;"
        " --disabled-fg: #585b70;"
        " --dim-fg: var(--muted);"
        " --accent: #89b4fa;"
        " --accent-hover: #b4befe;"
        " --accent-fg: #11111b;"
        " --danger: #f38ba8;"
        " --danger-fg: var(--accent-fg);"
        " --drop-bg: #89b4fa33;"
        " --section-fg: var(--accent);"
        " --control-bg: #313244;"
        " --control-hover-bg: #45475a;"
        " --control-hover-fg: var(--fg);"
        " --control-disabled-bg: var(--control-bg);"
        " --control-disabled-fg: var(--disabled-fg);"
        " --button-focus-fg: #ffffff;"
        " --menu-hover-bg: #313244;"
        " --menu-hover-fg: var(--fg);"
        " --shortcut-hover-fg: var(--muted);"
        " --settings-row-bg: #181825;"
        " --settings-help-fg: #a6adc8;"
        " --settings-active-fg: var(--accent);"
        " --selection-bg: #45475a;"
        " --selection-fg: #ffffff;"
        " --caret-color: #f5e0dc;"
        " --tab-bg: var(--surface);"
        " --tab-fg: #a6adc8;"
        " --tab-hover-bg: #313244;"
        " --tab-hover-fg: var(--fg);"
        " --tab-active-bg: #45475a;"
        " --tab-active-fg: #ffffff;"
        " --tab-close-fg: #a6adc8;"
        " --tab-radius: 4px;"
        " --tab-close-radius: 0.5em;"
        " --scrollbar-track-hover: #bac2de26;"
        " --scrollbar-thumb: #a6adc8;"
        " --scrollbar-thumb-hover: #bac2de;"
        " --scrollbar-thumb-active: var(--accent);"
        " --scrollbar-radius: 0px;"
        " --menu-radius: 6px;"
        " --menu-shadow: 0 8px 24px #00000073;"
        " }"
        ".label-selection { background: var(--selection-bg); color: var(--selection-fg); }"
        ".label-link { color: var(--accent); border-width: 1px; }"
        ".label-link:hover, .label-link:focus { color: var(--accent-hover); }"
        ".label-link:disabled { color: var(--disabled-fg); }"
        ".label-focus { border-color: var(--accent); border-width: 1px; }"
        ".tab-bar { height: 2.2em; flex-direction: row; align-items: center; padding: 0.25em 0.5em; gap: 0.4em; }"
        ".tab { height: 1.6em; background: var(--tab-bg); color: var(--tab-fg); border-radius: var(--tab-radius); padding: 0 0.8em; flex-direction: row; align-items: center; gap: 0.5em; cursor: pointer; }"
        ".tab:hover { background: var(--tab-hover-bg); color: var(--tab-hover-fg); }"
        ".tab:focus { border-color: var(--accent); border-width: 2px; }"
        ".tab.active { background: var(--tab-active-bg); color: var(--tab-active-fg); }"
        ".tab.drop-preview { background: var(--accent); opacity: 0.45; box-shadow: 0 2px 5px #00000066; }"
        ".tab-title { cursor: pointer; }"
        ".tab-close { width: 1em; height: 1em; color: var(--tab-close-fg); border-radius: var(--tab-close-radius); cursor: pointer; }"
        ".tab-close:hover { background: var(--danger); color: var(--danger-fg); }"
        ".tab-new { color: var(--tab-fg); border-radius: var(--tab-radius); padding: 0 0.4em; cursor: pointer; }"
        ".tab-new:hover { background: var(--tab-hover-bg); color: var(--tab-hover-fg); }"
        ".scrollbar { width: 8px; padding: 0 1px; }"
        ".scrollbar:hover, .scrollbar:active { width: 12px; background: var(--scrollbar-track-hover); }"
        ".scrollbar-thumb { width: 3px; min-height: 24px; background: var(--scrollbar-thumb); border-radius: var(--scrollbar-radius); }"
        ".scrollbar-thumb:hover { width: 8px; background: var(--scrollbar-thumb-hover); }"
        ".scrollbar-thumb:active { width: 8px; background: var(--scrollbar-thumb-active); }"
        ".context-menu { background: var(--surface); border-color: var(--border); border-radius: var(--menu-radius); box-shadow: var(--menu-shadow); }"
        ".menu-item { color: var(--fg); cursor: pointer; }"
        ".menu-item:hover { background: var(--menu-hover-bg); color: var(--menu-hover-fg); }"
        ".menu-item.disabled { color: var(--disabled-fg); cursor: default; }"
        ".menu-item:disabled { color: var(--disabled-fg); cursor: default; }"
        ".menu-item:checked { color: var(--fg); }"
        ".menu-item.submenu { color: var(--fg); }"
        ".menu-label { cursor: pointer; }"
        ".menu-label:disabled { cursor: default; }"
        ".menu-shortcut { color: var(--muted); }"
        ".menu-shortcut:hover, .menu-shortcut:focus { color: var(--shortcut-hover-fg); }"
        ".menu-separator { background: var(--border); }"
        ".menu-list { flex-direction: column; padding: 4px; }"
        ".menu-list .menu-item { min-height: 1.25em; padding: 0.15em 0.8em; gap: 2ch; border-radius: 4px; cursor: pointer; }"
        ".menu-list .menu-item:focus { background: var(--menu-hover-bg); color: var(--control-hover-fg); }"
        ".menu-list .menu-item:disabled { color: var(--disabled-fg); cursor: default; }"
        ".menu-list .menu-shortcut { color: var(--muted); }"
        ".menu-list .menu-separator { height: 1px; margin: 4px 6px; background: var(--border); }"
        ".text-input { background: var(--surface); border-color: var(--border); border-width: 1px; border-radius: 4px; padding: 2px 1ch; min-width: 20ch; cursor: text; }"
        ".text-input:focus { border-color: var(--accent); }"
        ".text-input-text { color: var(--fg); }"
        ".text-input-text.empty { color: var(--dim-fg); }"
        ".text-input-selection { background: var(--selection-bg); }"
        ".text-input-caret { background: var(--caret-color); }"
        ".button:focus { background: var(--control-hover-bg); color: var(--button-focus-fg); }"
        ".command-palette { background: var(--surface); border-color: var(--border); border-width: 1px; border-radius: 6px; }"
        ".command-palette .text-input { background: transparent; border-width: 0; border-radius: 0; min-width: 0; padding: 6px 0; }"
        ".palette-empty { color: var(--dim-fg); }"
        ".tab-preview { width: 400px; padding: 8px; gap: 6px; background: var(--surface); border-color: var(--border); border-width: 1px; border-radius: 8px; box-shadow: 0 6px 18px #00000073; }"
        ".tab-preview.dragging { width: 280px; }"
        ".tab-preview-title { color: var(--fg); }"
        ".dropdown-button { padding: 0.4em 1ch; background: var(--control-bg); color: var(--fg); border-radius: 4px; cursor: pointer; }"
        ".dropdown-button:hover, .dropdown-button:focus { background: var(--control-hover-bg); color: var(--control-hover-fg); }"
        ".dropdown-popup { width: 46ch; height: 17em; padding: 0.6em 1ch; gap: 0.5em; background: var(--surface); color: var(--fg); border-radius: 6px; }"
        ".dropdown-scroll { height: grow; }"
        ".terminal-drop-target { background: var(--drop-bg); border-color: var(--accent); border-width: 2px; }"
        ".settings-form { flex-direction: column; padding: 1.2em 2ch; gap: 0.8em; color: var(--fg); }"
        ".settings-header { align-items: center; gap: 2ch; }"
        ".settings-title { width: grow; }"
        ".settings-search { padding: 0.45em 1ch; }"
        ".settings-body { height: grow; gap: 2ch; align-items: stretch; }"
        ".settings-sidebar { width: 17ch; gap: 0.4em; }"
        ".settings-scroll { width: grow; }"
        ".settings-fields { gap: 0.55em; }"
        ".settings-section { padding: 0.4em 0; color: var(--section-fg); }"
        ".settings-row { padding: 0.65em 1ch; gap: 2ch; align-items: center; background: var(--settings-row-bg); border-radius: 6px; }"
        ".settings-description { width: grow; min-width: 0; gap: 0.25em; }"
        ".settings-help { color: var(--settings-help-fg); }"
        ".settings-editor { width: grow; min-width: 0; align-items: stretch; gap: 0.4em; }"
        ".settings-editor .text-input { min-width: 0; padding: 0.4em 1ch; }"
        ".settings-error { color: var(--danger); }"
        ".settings-footer { align-items: center; gap: 1ch; }"
        ".settings-status { width: grow; }"
        ".settings-button { padding: 0.4em 1ch; background: var(--control-bg); border-radius: 4px; cursor: pointer; }"
        ".settings-button:hover, .settings-button:focus { background: var(--control-hover-bg); color: var(--button-focus-fg); }"
        ".settings-button:disabled { background: var(--control-disabled-bg); color: var(--control-disabled-fg); cursor: default; }"
        ".settings-primary { background: var(--accent); color: var(--accent-fg); }"
        ".settings-primary:hover, .settings-primary:focus { background: var(--accent-hover); color: var(--accent-fg); }"
        ".settings-primary:disabled { background: var(--control-disabled-bg); color: var(--control-disabled-fg); }"
        ".settings-nav.active { background: var(--tab-active-bg); color: var(--settings-active-fg); }"
        ".settings-link { color: var(--section-fg); background: transparent; cursor: pointer; }"
    );
    sheet->has_defaults = ok;
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
        mask &= ~(1ULL << bit);

        if ((size_t)bit >= prop_count)
            continue;
        const struct prop_info *p = &props[bit];
        char *dst = (char *)out + p->offset;
        const char *src = (const char *)&r->style + p->offset;
        if (p->kind == P_INSETS) {
            const struct css_value *srcv = (const struct css_value *)src;
            struct css_value *dstv = (struct css_value *)dst;
            for (int e = 0; e < 4; e++)
                if (srcv[e].unit != CSS_UNIT_NONE)
                    dstv[e] = srcv[e];
            continue;
        }
        switch ((enum prop_kind)p->kind) {
        case P_FLEX_DIR: out->flex_direction = r->style.flex_direction; break;
        case P_ALIGN:    out->align_items = r->style.align_items; break;
        case P_JUSTIFY:  out->justify_content = r->style.justify_content; break;
        case P_CURSOR:   out->cursor = r->style.cursor; break;
        default:         memcpy(dst, src, p->size); break;
        }
        if (p->has_bit)
            set_style_has_bit(out, p->has_bit, get_style_has_bit(&r->style, p->has_bit));
    }
}

void css_style_resolve_ex(const struct css_sheet *sheet, const char *class_name,
                          const char *id_name, const char *tag_name,
                          unsigned pseudo_flags, const char *ancestor_path,
                          struct css_style *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->opacity = 1.0f;
    if (!sheet) return;

    /* The table is in apply order. A rule that matches replaces the props of
       the rules before it. */
    for (int i = 0; i < sheet->rule_count; i++) {
        const struct css_rule *r = &sheet->rules[i];
        if (selector_matches(r, class_name, id_name, tag_name, pseudo_flags,
                             ancestor_path))
            apply_rule_props(r, out);
    }
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
