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

/* The id of the UI_ACTION_ACTIVATE that the new-tab button sends. */
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

/*
 * A bar of tabs with a new-tab button. Its height animates in when the
 * second tab opens and out when only one is left. Positions are in
 * surface space unless a call says otherwise.
 *
 * Keyboard: When focused, Left and Right move between tabs and the new-tab
 * button. Home and End choose the first and last tab. Enter or Space
 * activates the focused item. Delete closes a tab, + opens a new tab, and
 * Escape returns focus to the host.
 *
 * Actions, each with the tab's index:
 * - UI_ACTION_ACTIVATE: a press or key chose a tab. The new-tab button sends it
 *   with id UI_TABBAR_NEW_TAB and index -1.
 * - UI_ACTION_CLOSE: the close box or a middle press closed a tab.
 * - UI_ACTION_HIGHLIGHT: the pointer moved onto a tab, or off all tabs
 *   with index -1.
 * - UI_ACTION_DRAG: a dragged tab moved. id is its ui_tab_drag_state,
 *   and pos the pointer. UI_TAB_DRAG_NONE means the drag was cancelled.
 * - UI_ACTION_TEAR_OFF: a dragged tab left the bar. pos is where the
 *   pointer holds the tab. A handler that returns true takes the drag
 *   over, and the bar ends its own gesture.
 * - UI_ACTION_MOVE: a drag ended. to is the new slot, or -1 when the tab
 *   was dropped outside the bar. Then pos is where the pointer holds the
 *   tab, as for UI_ACTION_TEAR_OFF.
 *
 * CSS: ".tab-bar", ".tab" (with "active" and :hover), ".tab-close",
 * ".tab-new", ".tab-separator", and ".tab.drop-preview".
 */
struct ui_tabbar {
    struct ui_widget widget;
    struct {
        struct ui_tab_state *tabs;
        int tab_cap;
        int capacity;
        uint64_t last_tick_ms;
        struct ui_hover_anim reveal;
        uint32_t bg_color;
        int hover_tab;
        int hover_close;
        int keyboard_index;
        bool hover_new;
        struct ui_hover_anim new_hover;
        int external_slot;
        bool external_drag;
        enum ui_tab_drag_state drag_state;
        int drag_tab;
        int drag_target_slot;
        int drag_grab_x;
        int pressed_tab;
        struct vector2i drag_origin;
        struct vector2i drag_pos;
        int n_tabs;
        uint64_t new_accessibility_id;
        struct vector2i size;
        /* The labels the bar paints. It owns their text. */
        struct ui_tab_label *labels;
        int labels_cap;
    } impl;
};

void ui_tabbar_init(struct ui_tabbar *tb);
/* Copies the labels when they differ from the shown ones. */
void ui_tabbar_set_tabs(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                        int n_tabs);
/* The bar's color where the theme sets none. */
void ui_tabbar_set_bg_color(struct ui_tabbar *tb, uint32_t bg);

/* Keep the bar's per-tab state in step with the tabs it shows. */
bool ui_tabbar_insert_tab(struct ui_tabbar *tb, int slot);
bool ui_tabbar_remove_tab(struct ui_tabbar *tb, int index);
void ui_tabbar_move_tab(struct ui_tabbar *tb, int from, int to);
/* Animates tab index in or out. A closing tab stays until
 * ui_tabbar_closing_done(); then remove it. */
bool ui_tabbar_begin_open(struct ui_tabbar *tb, int index, uint64_t now_ms);
bool ui_tabbar_begin_close(struct ui_tabbar *tb, int index, uint64_t now_ms);
int ui_tabbar_closing_tab(const struct ui_tabbar *tb);
bool ui_tabbar_is_closing(const struct ui_tabbar *tb, int index);
bool ui_tabbar_closing_done(const struct ui_tabbar *tb);

/* The bar's height now, during its animation, and once it settles. */
int ui_tabbar_height(const struct ui_tabbar *tb);
int ui_tabbar_steady_height(const struct ui_tabbar *tb);
bool ui_tabbar_is_dragging(const struct ui_tabbar *tb);

/* Rects of tab index and its parts, in surface space. */
bool ui_tabbar_tab_rect(const struct ui_tabbar *tb, int index,
                        struct rect2i *out);
bool ui_tabbar_close_rect(const struct ui_tabbar *tb, int index,
                          struct rect2i *out);
bool ui_tabbar_new_tab_rect(const struct ui_tabbar *tb, struct rect2i *out);

/* A tab from another bar is over this one at pos, or has left it. The
 * bar opens a gap at the slot it would drop into. */
void ui_tabbar_set_external_drag(struct ui_tabbar *tb, bool active,
                                 struct vector2i pos);
/* The slot an incoming tab drops into, or -1. */
int ui_tabbar_external_slot(const struct ui_tabbar *tb);

/* Paints tab index as a drag shows it, above preview_image when there is
 * one. The scene is in the image's own frame; hotspot is where the
 * pointer holds it. */
bool ui_tabbar_build_drag_scene(struct ui_tabbar *tb, int index,
                                const struct ui_image *preview_image,
                                struct ui_scene *scene,
                                struct vector2i *image_size,
                                struct vector2i *hotspot);

static inline struct ui_widget *ui_tabbar_widget(struct ui_tabbar *tb)
{
    return tb ? &tb->widget : NULL;
}

#endif
