#ifndef WSAP_UI_CSS_H
#define WSAP_UI_CSS_H

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ui/metrics.h"
#include "util/geom.h"

enum css_flex_direction : uint8_t {
    CSS_FLEX_ROW = 0,
    CSS_FLEX_COLUMN,
};

enum css_align : uint8_t {
    CSS_ALIGN_STRETCH = 0,
    CSS_ALIGN_START,
    CSS_ALIGN_CENTER,
    CSS_ALIGN_END,
};

enum css_justify : uint8_t {
    CSS_JUSTIFY_START = 0,
    CSS_JUSTIFY_CENTER,
    CSS_JUSTIFY_END,
    CSS_JUSTIFY_SPACE_BETWEEN,
    CSS_JUSTIFY_SPACE_AROUND,
    CSS_JUSTIFY_SPACE_EVENLY,
};

enum css_position : uint8_t {
    CSS_POSITION_STATIC = 0,
    CSS_POSITION_RELATIVE,
    CSS_POSITION_ABSOLUTE,
};

enum css_overflow : uint8_t {
    CSS_OVERFLOW_VISIBLE = 0,
    CSS_OVERFLOW_HIDDEN,
    CSS_OVERFLOW_CLIP,
    CSS_OVERFLOW_SCROLL,
    CSS_OVERFLOW_AUTO,
};

enum css_font_style : uint8_t {
    CSS_FONT_NORMAL = 0,
    CSS_FONT_ITALIC,
    CSS_FONT_OBLIQUE,
};

enum css_flex_wrap : uint8_t {
    CSS_WRAP_NONE = 0,
    CSS_WRAP_WRAP,
    CSS_WRAP_REVERSE,
};

/* The units of a CSS length:
   - px: logical pixels. The value is scaled by the dpi scale.
   - %: a share of the parent dimension.
   - em: cell heights.
   - rem: cell heights. This code base has no root font size, so em and rem
     mean the same size.
   - ch: cell widths.
   - grow: the free space of the parent.
   - auto, fit: the size of the content. fit is an alias of auto.
   - none: no value parsed, so the declaration is invalid. */
enum css_unit_type {
    CSS_UNIT_AUTO = 0,
    CSS_UNIT_PX,
    CSS_UNIT_PERCENT,
    CSS_UNIT_EM,
    CSS_UNIT_REM,
    CSS_UNIT_CH,
    CSS_UNIT_NONE,
    CSS_UNIT_GROW,
};

struct css_value {
    float val;
    enum css_unit_type unit;
};

struct css_box {
    union {
        struct { struct css_value top, right, bottom, left; };
        struct css_value v[4];
    };
};

enum css_pseudo : uint8_t {
    CSS_PSEUDO_NONE     = 0,
    CSS_PSEUDO_HOVER    = 1 << 0,
    CSS_PSEUDO_ACTIVE   = 1 << 1,
    CSS_PSEUDO_FOCUS    = 1 << 2,
    CSS_PSEUDO_DISABLED = 1 << 3,
    CSS_PSEUDO_CHECKED  = 1 << 4,
};

struct css_scale_context {
    float dpi_scale;
    float cell_w;
    float cell_h;
};

/* Resolves a CSS value to physical pixels. */
float css_value_resolve(struct css_value v, float parent_dim, float auto_val,
                        const struct css_scale_context *ctx);

enum ui_cursor : uint8_t {
    UI_CURSOR_AUTO = 0,
    UI_CURSOR_DEFAULT,
    UI_CURSOR_ARROW = UI_CURSOR_DEFAULT,
    UI_CURSOR_TEXT,
    UI_CURSOR_POINTER,
    UI_CURSOR_LINK = UI_CURSOR_POINTER,
    UI_CURSOR_CLICKABLE = UI_CURSOR_POINTER,
    UI_CURSOR_NONE,
    UI_CURSOR_CROSSHAIR,
    UI_CURSOR_MOVE,
    UI_CURSOR_GRAB,
    UI_CURSOR_RESIZE_EW,
    UI_CURSOR_RESIZE_NS,
    UI_CURSOR_RESIZE_NWSE,
    UI_CURSOR_RESIZE_NESW,
};

struct css_style {
    union {
        struct { struct css_value width, height; };
        struct css_value size[2];
    };
    union {
        struct { struct css_value min_width, min_height; };
        struct css_value min_size[2];
    };
    union {
        struct { struct css_value max_width, max_height; };
        struct css_value max_size[2];
    };

    struct css_box padding;
    struct css_box margin;
    /* Per-edge border widths. A unit of NONE or AUTO uses `border_width`. */
    struct css_box border_edges;

    struct css_value gap;
    /* top, right, bottom, left */
    struct css_value inset[4];

    uint32_t bg_color;
    uint32_t text_color;
    uint32_t border_color;

    struct css_value border_width;
    struct css_value border_radius;
    float opacity;
    float transition_duration;
    float flex_grow;
    float flex_shrink;
    struct css_value font_size;
    struct css_value line_height;
    int font_weight;
    int z_index;
    char font_family[48];

    struct css_value shadow_x;
    struct css_value shadow_y;
    struct css_value shadow_blur;
    uint32_t shadow_color;
    uint32_t ripple_color;

    /* Pack enum values and presence flags into one 32-bit word. */
    uint32_t flex_direction : 2;
    uint32_t align_items : 3;
    uint32_t justify_content : 3;
    uint32_t flex_wrap : 2;
    uint32_t align_self : 3;
    uint32_t cursor : 4;
    uint32_t has_bg_color : 1;
    uint32_t has_text_color : 1;
    uint32_t has_border_color : 1;
    uint32_t has_shadow : 1;
    uint32_t has_opacity : 1;
    uint32_t has_transition_duration : 1;
    uint32_t has_ripple_duration : 1;
    uint32_t has_cursor : 1;
    uint32_t has_ripple_color : 1;
    uint32_t has_flex_direction : 1;
    uint32_t has_align_items : 1;
    uint32_t has_justify_content : 1;
    uint32_t has_flex_wrap : 1;
    uint32_t has_flex_grow : 1;
    uint32_t has_flex_shrink : 1;
    uint32_t has_align_self : 1;
    uint32_t has_font_family : 1;
    uint32_t has_font_size : 1;
    uint32_t has_font_weight : 1;
    uint32_t has_font_style : 1;
    uint32_t has_line_height : 1;
    uint32_t has_overflow : 1;
    uint32_t has_position : 1;
    uint32_t has_z_index : 1;
    uint32_t overflow : 3;
    uint32_t position : 2;
    uint32_t font_style : 2;
    /* The time of a ripple of the part, in whole milliseconds. It is not a
     * transition. A larger value is clamped to 65535 ms. The spare bits of
     * the packed word hold it, so the style keeps its size. */
    uint32_t ripple_duration_ms : 16;
};

_Static_assert(sizeof(struct css_style) == 340, "css_style must stay 340 bytes");

/* Returns the effective opacity of `s`, or 1.0 when unset. */
static inline float css_style_effective_opacity(const struct css_style *s)
{
    return (s && s->has_opacity) ? s->opacity : 1.0f;
}

struct css_var {
    char *name;
    char *value;
    int line;
    int column;
};

/* A declaration that holds var(). The sheet keeps the raw text and resolves it
   again when the variables change. */
struct css_var_prop {
    char *value;
    int line;
    int column;
    uint16_t prop_idx;
    bool reported; /* A diagnostic already named this declaration. */
};

enum css_key_kind : uint8_t {
    CSS_KEY_CLASS = 0,
    CSS_KEY_ID,
    CSS_KEY_TAG,
    CSS_KEY_NONE, /* The compound matches no element. */
};

/* One class name of a compound selector. The offsets index the selector
   string of the rule. */
struct css_class_token {
    uint32_t offset;
    uint16_t length;
};

/* One compound of a selector: the part that matches one element. */
struct css_selector_part {
    uint32_t offset;      /* Start of the compound in the selector string. */
    uint32_t class_first; /* Index of its first entry in the class tokens. */
    uint16_t length;
    uint16_t class_count;
    enum css_key_kind kind;
};

/* The properties that a rule sets. Bit `i` belongs to entry `i` of the
   property table, so a set bit names its own entry. The mask grows with the
   table. */
struct css_prop_mask {
    uint64_t *bits;
    uint32_t words;
};

struct css_rule {
    char *selector;
    struct css_prop_mask set_mask;
    enum css_pseudo pseudo;
    struct css_selector_part *parts; /* Compiled when the sheet is built. */
    uint16_t part_count;
    uint16_t part_cap;
    struct css_class_token *classes;
    uint16_t class_count;
    uint16_t class_cap;
    struct css_var_prop *var_props; /* Unexpanded declarations referencing var(). */
    int var_prop_count;
    int var_prop_cap;
    struct css_style style;
};

/* One entry of the rule index. Every rule whose last compound names a class,
   an id, or a tag appears here under that key, so a lookup reads only the
   rules that can match. */
struct css_index_entry {
    const char *key; /* Borrowed from the selector string of the rule. */
    uint32_t rule;
    uint16_t key_len;
    enum css_key_kind kind;
};

struct css_sheet {
    struct css_rule *rules; /* Sorted in apply order. */
    int rule_count;
    int rule_cap;
    struct css_index_entry *index; /* Sorted by key, then by apply order. */
    int index_count;
    struct css_var *vars;   /* Custom properties declared in :root blocks. */
    int var_count;
    int var_cap;
    bool index_ready;
    bool has_defaults; /* The sheet is built on the css_init_default rules. */
};

enum css_diagnostic_kind {
    CSS_DIAGNOSTIC_UNKNOWN_PROPERTY,
    CSS_DIAGNOSTIC_INVALID_VALUE,
    CSS_DIAGNOSTIC_INVALID_SYNTAX,
    CSS_DIAGNOSTIC_UNSUPPORTED_SELECTOR,
};

/* Property and value are borrowed and valid only during the callback. */
struct css_diagnostic {
    enum css_diagnostic_kind kind;
    size_t line;
    size_t column;
    const char *property;
    const char *value;
};

typedef void (*css_diagnostic_fn)(const struct css_diagnostic *diagnostic,
                                  void *userdata);

const char *css_diagnostic_message(enum css_diagnostic_kind kind);

/* Parses CSS text and reports invalid CSS through `report`. Returns false when
 * `sheet` or `css_text` is NULL, or on allocation failure. A NULL `report`
 * ignores diagnostics. */
bool css_parse_with_diagnostics(struct css_sheet *sheet, const char *css_text,
                                css_diagnostic_fn report, void *userdata);

/* Parses CSS text and adds the rules to `sheet`. */
bool css_parse(struct css_sheet *sheet, const char *css_text);
/* Parses CSS text and adds the rules to `sheet`. A declaration that holds a
   var() stays unresolved until css_sheet_append() adds the variables. A
   stylesheet that layers over another sheet uses this call. */
bool css_parse_deferred(struct css_sheet *sheet, const char *css_text,
                        css_diagnostic_fn report, void *userdata);
/* Appends rules from `src` to `dst`. */
bool css_sheet_append(struct css_sheet *dst, const struct css_sheet *src);
/* Releases the rules, the index, and the variables of `sheet`. */
void css_sheet_clear(struct css_sheet *sheet);

/* Populates `sheet` with the UI core rules of assets/ui/core.css. */
void css_init_default(struct css_sheet *sheet);

/* Returns the shared sheet of the UI core rules, which assets/ui/core.css
   holds. */
const struct css_sheet *css_default_sheet(void);

/* Clears the shared default sheet. */
void css_shutdown(void);

/* Resolves the style of an element from its class, id, and tag names and the
 * pseudo-class flags. */
void css_style_resolve(const struct css_sheet *sheet,
                       const char *class_name,
                       const char *id_name,
                       const char *tag_name,
                       unsigned pseudo_flags,
                       struct css_style *out_style);

/* The identity of one element: the class list, the id, and the tag that a
   selector tests. A NULL field means that the element has no value for it. */
struct css_element {
    const char *class_name;
    const char *id_name;
    const char *tag_name;
};

/* Resolves styling properties matching `element` and the pseudo-class flags.
   `ancestors` holds the ancestors from the root down to the parent, and
   `ancestor_count` is their number. */
void css_style_resolve_node(const struct css_sheet *sheet,
                            const struct css_element *element,
                            unsigned pseudo_flags,
                            const struct css_element *ancestors,
                            size_t ancestor_count,
                            struct css_style *out_style);

/* Returns true when two styles have identical layout properties. */
bool css_style_layout_eq(const struct css_style *a, const struct css_style *b);

/* Returns `v` when it is finite and above zero, else `fallback`. */
static inline float ui_positive_or(float v, float fallback)
{
    return isfinite(v) && v > 0.0f ? v : fallback;
}

/* Returns `dpi_scale` when it is finite and positive, else 1.0. */
static inline float ui_dpi_scale(float dpi_scale)
{
    return ui_positive_or(dpi_scale, 1.0f);
}

/* Resolves `px` logical pixels to physical pixels at `dpi_scale`. */
static inline int ui_px(float dpi_scale, int px)
{
    float v = (float)px * ui_dpi_scale(dpi_scale);
    return v > 0.0f && v < (float)INT_MAX ? (int)lroundf(v) : px;
}

/* Resolves `px` logical pixels to physical pixels at the scale of `ctx`. */
static inline int ui_ctx_px(const struct css_scale_context *ctx, int px)
{
    return ui_px(ctx ? ctx->dpi_scale : 0.0f, px);
}

/* Stores finite `v` in `dst` and returns true when `v` is above zero. */
static inline bool ui_set_if_positive(float *dst, float v)
{
    if (!dst || !isfinite(v) || v <= 0.0f)
        return false;
    *dst = v;
    return true;
}

/* Stores each of `dpi_scale`, `cell_w`, and `cell_h` in `ctx` when the value
 * is finite and positive. */
static inline void ui_scale_ctx_set(struct css_scale_context *ctx,
                                    float dpi_scale, float cell_w, float cell_h)
{
    if (!ctx)
        return;
    ui_set_if_positive(&ctx->dpi_scale, dpi_scale);
    ui_set_if_positive(&ctx->cell_w, cell_w);
    ui_set_if_positive(&ctx->cell_h, cell_h);
}

/* Returns the scale context of an unscaled surface with default cells. */
static inline struct css_scale_context ui_scale_ctx_default(void)
{
    return (struct css_scale_context){
        .dpi_scale = 1.0f,
        .cell_w = (float)UI_DEFAULT_CELL_W,
        .cell_h = (float)UI_DEFAULT_CELL_H,
    };
}

/* Returns `color` with its alpha multiplied by `opacity`. */
static inline uint32_t ui_color_opacity(uint32_t color, float opacity)
{
    if (!isfinite(opacity) || opacity <= 0.0f)
        return color & 0x00FFFFFFu;
    if (opacity >= 1.0f)
        return color;
    uint32_t a = (uint32_t)(color >> 24);
    a = (uint32_t)((float)a * opacity + 0.5f);
    return (color & 0x00FFFFFFu) | (a << 24);
}

/* The pixel rounding rule of the UI core: one length rounds to the nearest
   whole pixel, and a half pixel rounds away from zero. A rectangle rounds each
   of its two edges, so its size follows from the rounded edges. Two touching
   rectangles keep a shared edge, and no gap or overlap appears. */

/* Returns `v` as a whole number, clamped to the int range. With `nonnegative`,
   a negative result becomes zero. */
static inline int ui_float_to_int(float v, bool nonnegative)
{
    if (!isfinite(v))
        return 0;
    if (nonnegative && v < 0.0f)
        return 0;
    if (v >= (float)INT_MAX)
        return INT_MAX;
    if (v <= (float)INT_MIN)
        return nonnegative ? 0 : INT_MIN;
    return (int)lroundf(v);
}

/* Resolves CSS value `v` to whole pixels. */
static inline int ui_css_resolve_int(struct css_value v, float parent_dim,
                                     float auto_val,
                                     const struct css_scale_context *ctx,
                                     bool nonnegative)
{
    return ui_float_to_int(css_value_resolve(v, parent_dim, auto_val, ctx),
                           nonnegative);
}

/* Resolves the four edges of `box` to whole pixels against `basis`. */
static inline struct insets2i ui_css_resolve_insets(struct css_box box,
                                                    struct vector2i basis,
                                                    const struct css_scale_context *ctx,
                                                    bool nonnegative)
{
    struct insets2i out = {0};
    for (int i = 0; i < 4; i++)
        out.v[i] = ui_css_resolve_int(box.v[i], (i & 1) ? (float)basis.x : (float)basis.y,
                                      0, ctx, nonnegative);
    return out;
}

/* Resolves CSS value `v` to whole pixels, with `fallback` when the result is
   not positive or does not fit an int. */
static inline int ui_ctx_size(const struct css_scale_context *ctx,
                              struct css_value v, float fallback)
{
    float resolved = css_value_resolve(v, 0, fallback, ctx);
    if (!isfinite(resolved) || resolved <= 0.0f)
        return (int)fallback;
    return resolved < (float)INT_MAX ? ui_float_to_int(resolved, true)
                                     : (int)fallback;
}

/* Resolves CSS value `v` to a whole pixel inset. A negative result is zero. */
static inline int ui_ctx_inset(const struct css_scale_context *ctx,
                               struct css_value v, float fallback)
{
    int px = ui_ctx_size(ctx, v, fallback);
    return px < 0 ? 0 : px;
}

/* Resolves the four edges of `box` to whole pixel insets. */
static inline struct insets2i ui_ctx_insets(const struct css_scale_context *ctx,
                                            struct css_box box, float fallback)
{
    struct insets2i out = {0};
    for (int i = 0; i < 4; i++)
        out.v[i] = ui_ctx_inset(ctx, box.v[i], fallback);
    return out;
}

/* Border width of `style` on `edge` (top, right, bottom, left), in pixels.
   A per-edge value wins over `border-width`. */
static inline int ui_css_border_edge(const struct css_style *style,
                                     const struct css_scale_context *scale,
                                     int edge)
{
    if (!style || !style->has_border_color)
        return 0;
    if (edge >= 0 && edge < 4 &&
        style->border_edges.v[edge].unit != CSS_UNIT_NONE &&
        style->border_edges.v[edge].unit != CSS_UNIT_AUTO)
        return ui_css_resolve_int(style->border_edges.v[edge], 0, 0, scale,
                                  true);
    return ui_css_resolve_int(style->border_width, 0, 0, scale, true);
}

/* Border width of `style`, in pixels. Zero without a border color. The paint
   uses the widest edge. */
static inline int ui_css_border_width(const struct css_style *style,
                                      const struct css_scale_context *scale)
{
    int width = 0;
    for (int edge = 0; edge < 4; edge++)
        width = width > ui_css_border_edge(style, scale, edge)
            ? width : ui_css_border_edge(style, scale, edge);
    return width;
}

/* Resolves the four border widths in CSS order top, right, bottom, left. */
static inline struct insets2i ui_css_border_insets(
    const struct css_style *style, const struct css_scale_context *scale)
{
    struct insets2i out = {0};
    for (int edge = 0; edge < 4; edge++)
        out.v[edge] = ui_css_border_edge(style, scale, edge);
    return out;
}

/* The C-side fallbacks of the properties that a widget reads. Each one takes
   the value that the widget uses when CSS leaves the property unset. */
static inline float ui_css_font_size(const struct css_style *style,
                                     const struct css_scale_context *scale,
                                     float fallback)
{
    return style && style->has_font_size
        ? css_value_resolve(style->font_size, 0, fallback, scale) : fallback;
}

/* Returns the line height of `style` in pixels. A percentage, including a
 * unitless value, is a share of the font size. `normal` and an unset property
 * use `fallback`, which is the cell height of the widget. */
static inline float ui_css_line_height(const struct css_style *style,
                                       const struct css_scale_context *scale,
                                       float fallback)
{
    if (!style || !style->has_line_height)
        return fallback;
    if (style->line_height.unit == CSS_UNIT_PERCENT)
        return ui_css_font_size(style, scale, fallback) *
               style->line_height.val / 100.0f;
    return css_value_resolve(style->line_height, 0, fallback, scale);
}

static inline int ui_css_font_weight(const struct css_style *style,
                                     int fallback)
{
    return style && style->has_font_weight ? style->font_weight : fallback;
}

static inline enum css_font_style ui_css_font_style(
    const struct css_style *style, enum css_font_style fallback)
{
    return style && style->has_font_style
        ? (enum css_font_style)style->font_style : fallback;
}

static inline enum css_overflow ui_css_overflow(const struct css_style *style,
                                                enum css_overflow fallback)
{
    return style && style->has_overflow
        ? (enum css_overflow)style->overflow : fallback;
}

static inline enum css_position ui_css_position(const struct css_style *style,
                                                enum css_position fallback)
{
    return style && style->has_position
        ? (enum css_position)style->position : fallback;
}

/* Resolves one `top`, `right`, `bottom`, or `left` value. */
static inline int ui_css_inset(const struct css_style *style, int edge,
                               const struct css_scale_context *scale,
                               int fallback)
{
    if (!style || edge < 0 || edge > 3 ||
        style->inset[edge].unit == CSS_UNIT_NONE ||
        style->inset[edge].unit == CSS_UNIT_AUTO)
        return fallback;
    return ui_css_resolve_int(style->inset[edge], 0, 0, scale, false);
}

static inline int ui_css_z_index(const struct css_style *style, int fallback)
{
    return style && style->has_z_index ? style->z_index : fallback;
}

#endif
