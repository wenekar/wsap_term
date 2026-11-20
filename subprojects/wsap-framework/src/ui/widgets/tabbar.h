#ifndef WSAP_APP_TABS_TABBAR_H
#define WSAP_APP_TABS_TABBAR_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/anim.h"
#include "paint/scene.h"
#include "ui/surface.h"
#include "ui/widget.h"
#include "util/geom.h"

#define UI_TABBAR_ANIM_DURATION_MS 150
#define UI_TABBAR_RESIZE_ANIM_MS   200
#define UI_TABBAR_SLIDE_ANIM_MS    250
/* The width of the right edge fade. It has no CSS property. */
#define UI_TABBAR_FADE_DP          32

#define UI_TABBAR_NEW_TAB (-2)

/* The action kinds that the tab bar sends. They start at UI_ACTION_USER so
 * that they stay out of the core enum. */
enum ui_tab_action_kind {
    UI_TAB_ACTION_MOVE = UI_ACTION_USER,
    UI_TAB_ACTION_DRAG,
    UI_TAB_ACTION_TEAR_OFF,
};

/* A core kind must stay below the tab kinds. */
_Static_assert((int)UI_TAB_ACTION_MOVE >= (int)UI_ACTION_USER,
               "a tab action kind collides with a core action kind");

enum ui_tab_drag_state : uint8_t {
    UI_TAB_DRAG_NONE = 0,
    UI_TAB_DRAG_PENDING,
    UI_TAB_DRAG_REORDER,
    UI_TAB_DRAG_DETACH,
};

struct ui_tab_state {
    uint64_t accessibility_id;
    struct ui_hover_anim close_hover;
    /* The background of the tab moves from the idle color to the hover color
     * over the CSS transition-duration of the tab part. */
    struct ui_hover_anim hover_bg;
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
        const char *empty_title;
        const char *shortcut_prefix;
        struct vector2i drag_origin;
        struct vector2i drag_pos;
        struct vector2i size;
        /* The mark that a drag from outside the app draws on the bar. */
        struct ui_drop_indicator drop;
        int tab_cap;
        int capacity;
        uint32_t bg_color;
        int hover_tab;
        int hover_close;
        int keyboard_index;
        int external_slot;
        int drag_tab;
        int drag_target_slot;
        int drag_slot;
        int drag_grab_x;
        int pressed_tab;
        int n_tabs;
        int labels_cap;
        enum ui_tab_drag_state drag_state;
        bool hover_new;
        bool external_drag;
    } state;
};

void ui_tabbar_init(struct ui_tabbar *tb);
/* Sets the title that a tab shows when its label text is empty. The bar
 * borrows `text`. A NULL text shows nothing. */
void ui_tabbar_set_empty_title(struct ui_tabbar *tb, const char *text);
/* Sets the text that comes before the tab number in a shortcut badge, for
 * example "^". The bar borrows `text`. A NULL text shows the number alone. */
void ui_tabbar_set_shortcut_prefix(struct ui_tabbar *tb, const char *text);
/* Copies the label text of each tab. With a NULL `tabs`, every tab gets an
 * empty label. */
void ui_tabbar_set_tabs(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                        int n_tabs);
/* Sets the background color of `tb`, which the bar uses when its part has no
 * background color. */
void ui_tabbar_set_bg_color(struct ui_tabbar *tb, uint32_t bg);

bool ui_tabbar_insert_tab(struct ui_tabbar *tb, int slot);
bool ui_tabbar_remove_tab(struct ui_tabbar *tb, int index);
void ui_tabbar_move_tab(struct ui_tabbar *tb, int from, int to);
bool ui_tabbar_begin_open(struct ui_tabbar *tb, int index, uint64_t now_ms);
bool ui_tabbar_begin_close(struct ui_tabbar *tb, int index, uint64_t now_ms);
/* Returns the index of the tab currently closing, or -1 if none is closing. */
int ui_tabbar_closing_tab(const struct ui_tabbar *tb);
/* Returns true when the tab at `index` is marked as closing, also after its
 * close animation ends. */
bool ui_tabbar_is_closing(const struct ui_tabbar *tb, int index);
/* Returns true when the first closing tab has finished its close animation.
 * Returns false when no tab is closing. */
bool ui_tabbar_closing_done(const struct ui_tabbar *tb);

/* Returns the current animated height of `tb`. */
int ui_tabbar_height(const struct ui_tabbar *tb);
/* Returns the resting height of `tb` without animations. */
int ui_tabbar_steady_height(const struct ui_tabbar *tb);
/* Returns the full CSS height of the tab bar. It does not depend on the tab
 * count or the animations. */
int ui_tabbar_bar_height(const struct ui_tabbar *tb);
/* Returns true after the pointer has moved far enough to start a tab drag. */
bool ui_tabbar_is_dragging(const struct ui_tabbar *tb);
/* Returns the index of the hovered tab, or -1 if none. */
int ui_tabbar_hovered_tab(const struct ui_tabbar *tb);

/* Fills `out` with the tab rectangle in surface coordinates. Returns false
 * when the tab has no rectangle. */
bool ui_tabbar_tab_rect(const struct ui_tabbar *tb, int index,
                        struct rect2i *out);
/* Fills `out` with the close button rectangle in surface coordinates. Returns
 * false when the close button has no rectangle. */
bool ui_tabbar_close_rect(const struct ui_tabbar *tb, int index,
                          struct rect2i *out);
/* Fills `out` with the new tab button rectangle in surface coordinates.
 * Returns false when the button has no rectangle. */
bool ui_tabbar_new_tab_rect(const struct ui_tabbar *tb, struct rect2i *out);

/* Updates the external drag state from the surface position `pos`. */
void ui_tabbar_set_external_drag(struct ui_tabbar *tb, bool active,
                                 struct vector2i pos);
/* Returns the drop mark of a drag from outside the app, or NULL. */
const struct ui_drop_indicator *ui_tabbar_drop_indicator(
    const struct ui_tabbar *tb);
/* Returns the insert slot marked for a native drag over the bar, or -1. */
int ui_tabbar_drop_slot(const struct ui_tabbar *tb);
/* Returns the insert slot for an external tab drop, or -1 when no external
 * drag is active. */
int ui_tabbar_external_slot(const struct ui_tabbar *tb);

/* Builds the drag scene of the tab at `index`. Fills `image_size` with the
 * scene size and `hotspot` with the grab point in scene coordinates. */
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
