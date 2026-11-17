#ifndef APP_TABS_TABBAR_INTERNAL_H
#define APP_TABS_TABBAR_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/widget.h"
#include "app/tabs/tabbar.h"

/* The interned part handles of the bar. struct tb_parts stays zero until
 * tb_part() fills it. */
struct tb_parts {
    ui_part_id bar;
    ui_part_id tab;
    ui_part_id tab_active;
    ui_part_id tab_drop;
    ui_part_id tab_close;
    ui_part_id tab_new;
    ui_part_id tab_sep;
    ui_part_id tab_preview;
};

/* The pixel metrics of the bar and its tab slots for one width.
 * geometry.c fills the record from the part styles. scene.c and tabbar.c
 * read it. */
struct tb_layout {
    int height;
    struct insets2i pad;
    int gap;
    int tab_height;
    int tab_pad;
    int tab_width;
    int new_tab_width;
    int new_tab_height;
    int radius;
    int preview_radius;
    int close_w;
    int close_h;
    int close_pad;
    int close_gap;
    int focus_w;
    int stroke_w;
    int fade_w;
    int cell_w;
    int cell_h;
    float n_eff;
    float share;
    int n_tabs;
};

/* The result of a hit test. `index` is -1 when the point hits no tab. */
struct tb_hit {
    int index;
    bool on_close;
    bool on_new;
};

static inline const struct css_scale_context *tb_ctx(const struct ui_tabbar *tb)
{
    return &tb->widget.style_scale;
}

/* style.c */
/* Returns the interned part handles of the bar. */
const struct tb_parts *tb_part(void);
/* Returns the style of `part` of the bar in state `pseudo`. */
struct css_style tb_style(const struct ui_tabbar *tb, ui_part_id part,
                          unsigned pseudo);
/* Returns the CSS height of the bar in device pixels. */
int tb_css_height(const struct ui_tabbar *tb);

/* geometry.c */
/* Returns the open animation progress of the tab at `index`, in 0 to 1. */
float tb_open_progress(const struct ui_tabbar *tb, int index);
/* Returns the slide offset of the tab at `index` in pixels. */
float tb_tab_offset(const struct ui_tabbar *tb, int index);
/* Fills the bar and tab metrics for `n_tabs` tabs at width `size.x`. */
struct tb_layout tb_layout_for(const struct ui_tabbar *tb, int n_tabs,
                               struct vector2i size);
/* Returns the fractional width of the tab at `index`. */
float tb_tab_width_f(const struct ui_tabbar *tb,
                     const struct tb_layout *l, int index);
/* Returns the rounded width of the tab at `index`, or 0. */
int tb_tab_width_at(const struct ui_tabbar *tb,
                    const struct tb_layout *l, int index);
/* Returns the close button rectangle of the tab rectangle `tab_r`. */
struct rect2i tb_close_box(struct rect2i tab_r, int close_w, int close_h,
                           int close_pad);
/* Fills the drop preview rectangle. False when no preview is visible. */
bool tb_drop_rect(const struct ui_tabbar *tb, const struct tb_layout *l,
                  struct vector2i size, struct rect2i *out);
/* Fills the visible span of the tab at `index`. False when it has no span. */
bool tb_tab_span(const struct ui_tabbar *tb, const struct tb_layout *l,
                 int n_tabs, int width, int index, int *x, int *w);
/* Fills the rectangle of the tab that the pointer drags. */
bool tb_drag_rect(const struct ui_tabbar *tb, const struct tb_layout *l,
                  int n_tabs, struct vector2i size, struct rect2i *out);
/* Counts the tabs that stay after the close animations end. */
int tb_live_tabs(const struct ui_tabbar *tb, int n_tabs);
/* Returns the animated height of the bar. */
int tb_bar_height(const struct ui_tabbar *tb, int css_height, int n_tabs);
/* Returns the resting height of the bar. */
int tb_steady_height(const struct ui_tabbar *tb, int n_tabs);
/* Returns the full height of the bar while an external drag is over it. */
int tb_drop_height(const struct ui_tabbar *tb, int n_tabs);
/* Fills the rectangle of the tab at `index`. */
bool tb_tab_rect_at(const struct ui_tabbar *tb, int n_tabs,
                    struct vector2i size, int index, struct rect2i *out);
/* Fills the rectangle of the close button of the tab at `index`. */
bool tb_close_rect_at(const struct ui_tabbar *tb, int n_tabs,
                      struct vector2i size, int index, struct rect2i *out);
/* Fills the rectangle of the new tab button. */
bool tb_new_tab_rect_at(const struct ui_tabbar *tb, int n_tabs,
                        struct vector2i size, struct rect2i *out);
/* Hit-tests the point `p` in widget space. */
struct tb_hit tb_hit_at(const struct ui_tabbar *tb, int n_tabs,
                        struct vector2i size, struct vector2i p);

/* scene.c */
/* Emits the scene of the bar at width `size.x` into `scene`. */
bool tb_scene_emit(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                   int n_tabs, struct vector2i size, struct ui_scene *scene,
                   uint64_t now_ms);

/* tabbar.c */
/* Grows the tab list to `n` entries and starts their open animation. */
bool tb_ensure_tabs(struct ui_tabbar *tb, int n);

#endif
