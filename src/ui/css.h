#ifndef WSAP_UI_CSS_H
#define WSAP_UI_CSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum css_flex_direction {
    CSS_FLEX_ROW = 0,
    CSS_FLEX_COLUMN,
};

enum css_align {
    CSS_ALIGN_STRETCH = 0,
    CSS_ALIGN_START,
    CSS_ALIGN_CENTER,
    CSS_ALIGN_END,
};

enum css_justify {
    CSS_JUSTIFY_START = 0,
    CSS_JUSTIFY_CENTER,
    CSS_JUSTIFY_END,
};

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

enum css_pseudo {
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

float css_value_resolve(struct css_value v, float parent_dim, float auto_val,
                        const struct css_scale_context *ctx);

enum ui_cursor {
    UI_CURSOR_AUTO = 0,
    UI_CURSOR_DEFAULT,
    UI_CURSOR_ARROW = UI_CURSOR_DEFAULT,
    UI_CURSOR_TEXT,
    UI_CURSOR_POINTER,
    UI_CURSOR_LINK = UI_CURSOR_POINTER,
    UI_CURSOR_CLICKABLE = UI_CURSOR_POINTER,
    UI_CURSOR_NONE,
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

    enum css_flex_direction flex_direction;
    enum css_align align_items;
    enum css_justify justify_content;
    enum ui_cursor cursor;
    struct css_value gap;

    uint32_t bg_color;
    uint32_t text_color;
    uint32_t border_color;

    struct css_value border_width;
    struct css_value border_radius;
    float opacity;
    float transition_duration;

    struct css_value shadow_x;
    struct css_value shadow_y;
    struct css_value shadow_blur;
    uint32_t shadow_color;
    uint32_t ripple_color;

    bool has_bg_color;
    bool has_text_color;
    bool has_border_color;
    bool has_shadow;
    bool has_opacity;
    bool has_transition_duration;
    bool has_cursor;
    bool has_ripple_color;
    bool has_flex_direction;
    bool has_align_items;
    bool has_justify_content;

};

static inline float css_style_effective_opacity(const struct css_style *s)
{
    return (s && s->has_opacity) ? s->opacity : 1.0f;
}

struct css_rule {
    char *selector;
    uint64_t set_mask;
    struct css_style style;
    enum css_pseudo pseudo;
    size_t last_off;
    int class_rank;
    bool is_class;
    bool is_id;
};

struct css_index;

struct css_sheet {
    struct css_rule *rules;
    int rule_count;
    int rule_cap;
    struct css_index *index;
};

bool css_parse(struct css_sheet *sheet, const char *css_text);
bool css_sheet_append(struct css_sheet *dst, const struct css_sheet *src);
void css_sheet_clear(struct css_sheet *sheet);

void css_init_default(struct css_sheet *sheet);

const struct css_sheet *css_default_sheet(void);

void css_style_resolve(const struct css_sheet *sheet,
                       const char *class_name,
                       const char *id_name,
                       const char *tag_name,
                       unsigned pseudo_flags,
                       struct css_style *out_style);

void css_style_resolve_ex(const struct css_sheet *sheet,
                          const char *class_name,
                          const char *id_name,
                          const char *tag_name,
                          unsigned pseudo_flags,
                          const char *ancestor_path,
                          struct css_style *out_style);

bool css_style_layout_eq(const struct css_style *a, const struct css_style *b);

#endif
