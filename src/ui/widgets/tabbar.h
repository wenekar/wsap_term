#ifndef WSAP_UI_WIDGETS_TABBAR_H
#define WSAP_UI_WIDGETS_TABBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/anim.h"
#include "paint/scene.h"
#include "ui/widget.h"
#include "util/geom.h"

#define UI_TABBAR_HEIGHT_DP      28
#define UI_TABBAR_PAD_DP         10
#define UI_TABBAR_ANIM_DURATION_MS 150
#define UI_TABBAR_RESIZE_ANIM_MS   200
#define UI_TABBAR_SLIDE_ANIM_MS    250
#define UI_TABBAR_FADE_DP          32

#define UI_TABBAR_NEW_TAB (-2)

enum ui_tab_drag_state {
    UI_TAB_DRAG_NONE = 0,
    UI_TAB_DRAG_PENDING,
    UI_TAB_DRAG_REORDER,
    UI_TAB_DRAG_DETACH,
};

struct ui_tab_state {
    uint64_t accessibility_id;
    struct ui_hover_anim close_hover;
    struct ui_hover_anim open;
    struct ui_hover_anim slide;
    struct ui_ripple ripple;
    bool closing;
};

struct ui_tab_label {
    const char *text;
    bool active;
    uint8_t pad[7];
};

struct ui_tabbar {
    struct ui_widget widget;
    struct {
        struct ui_tab_state *tabs;
        struct ui_tab_label *labels;
        uint64_t last_tick_ms;
        uint64_t new_accessibility_id;
        struct ui_hover_anim reveal;
        struct ui_hover_anim new_hover;
        struct vector2i drag_origin;
        struct vector2i drag_pos;
        struct vector2i size;
        int tab_cap;
        int capacity;
        uint32_t bg_color;
        int hover_tab;
        int hover_close;
        int keyboard_index;
        int external_slot;
        enum ui_tab_drag_state drag_state;
        int drag_tab;
        int drag_target_slot;
        int drag_grab_x;
        int pressed_tab;
        int n_tabs;
        int labels_cap;
        bool hover_new;
        bool external_drag;
    } impl;
};

/* Initializes tab bar widget `tb`. */
void ui_tabbar_init(struct ui_tabbar *tb);
/* Sets tab labels and tab count for `tb`. */
void ui_tabbar_set_tabs(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                        int n_tabs);
/* Sets the background color of tab bar `tb`. */
void ui_tabbar_set_bg_color(struct ui_tabbar *tb, uint32_t bg);

/* Inserts a new tab entry at `slot`. */
bool ui_tabbar_insert_tab(struct ui_tabbar *tb, int slot);
/* Removes the tab at `index`. */
bool ui_tabbar_remove_tab(struct ui_tabbar *tb, int index);
/* Moves a tab from index `from` to index `to`. */
void ui_tabbar_move_tab(struct ui_tabbar *tb, int from, int to);
/* Starts the tab open animation for the tab at `index` at `now_ms`. */
bool ui_tabbar_begin_open(struct ui_tabbar *tb, int index, uint64_t now_ms);
/* Starts the tab close animation for the tab at `index` at `now_ms`. */
bool ui_tabbar_begin_close(struct ui_tabbar *tb, int index, uint64_t now_ms);
/* Returns the index of the tab currently closing, or -1 if none is closing. */
int ui_tabbar_closing_tab(const struct ui_tabbar *tb);
/* Returns true when the tab at `index` is in closing animation. */
bool ui_tabbar_is_closing(const struct ui_tabbar *tb, int index);
/* Returns true when all closing tab animations have finished. */
bool ui_tabbar_closing_done(const struct ui_tabbar *tb);

/* Returns the current animated height of `tb`. */
int ui_tabbar_height(const struct ui_tabbar *tb);
/* Returns the resting height of `tb` without animations. */
int ui_tabbar_steady_height(const struct ui_tabbar *tb);
/*
 * Returns the full CSS height of the tab bar.
 * Does not depend on tab count or animations.
 */
int ui_tabbar_bar_height(const struct ui_tabbar *tb);
/* Returns true when a tab drag operation is in progress. */
bool ui_tabbar_is_dragging(const struct ui_tabbar *tb);

/* Calculates the bounding rectangle of the tab at `index`. */
bool ui_tabbar_tab_rect(const struct ui_tabbar *tb, int index,
                        struct rect2i *out);
/* Calculates the bounding rectangle of the close button for the tab at `index`. */
bool ui_tabbar_close_rect(const struct ui_tabbar *tb, int index,
                          struct rect2i *out);
/* Calculates the bounding rectangle of the new tab button. */
bool ui_tabbar_new_tab_rect(const struct ui_tabbar *tb, struct rect2i *out);

/* Updates external tab drag hover state and pointer position `pos`. */
void ui_tabbar_set_external_drag(struct ui_tabbar *tb, bool active,
                                 struct vector2i pos);
/* Returns the target insertion slot index for an external tab drop. */
int ui_tabbar_external_slot(const struct ui_tabbar *tb);

/* Builds scene draw commands and thumbnail image for dragging the tab at `index`. */
bool ui_tabbar_build_drag_scene(struct ui_tabbar *tb, int index,
                                const struct ui_image *preview_image,
                                struct ui_scene *scene,
                                struct vector2i *image_size,
                                struct vector2i *hotspot);

/* Returns the base widget pointer of `tb`, or NULL if `tb` is NULL. */
static inline struct ui_widget *ui_tabbar_widget(struct ui_tabbar *tb)
{
    return tb ? &tb->widget : NULL;
}

#endif
