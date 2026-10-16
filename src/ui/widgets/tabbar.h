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
        struct ui_tab_label *labels;
        int labels_cap;
    } impl;
};

void ui_tabbar_init(struct ui_tabbar *tb);
void ui_tabbar_set_tabs(struct ui_tabbar *tb, const struct ui_tab_label *tabs,
                        int n_tabs);
void ui_tabbar_set_bg_color(struct ui_tabbar *tb, uint32_t bg);

bool ui_tabbar_insert_tab(struct ui_tabbar *tb, int slot);
bool ui_tabbar_remove_tab(struct ui_tabbar *tb, int index);
void ui_tabbar_move_tab(struct ui_tabbar *tb, int from, int to);
bool ui_tabbar_begin_open(struct ui_tabbar *tb, int index, uint64_t now_ms);
bool ui_tabbar_begin_close(struct ui_tabbar *tb, int index, uint64_t now_ms);
int ui_tabbar_closing_tab(const struct ui_tabbar *tb);
bool ui_tabbar_is_closing(const struct ui_tabbar *tb, int index);
bool ui_tabbar_closing_done(const struct ui_tabbar *tb);

int ui_tabbar_height(const struct ui_tabbar *tb);
int ui_tabbar_steady_height(const struct ui_tabbar *tb);
bool ui_tabbar_is_dragging(const struct ui_tabbar *tb);

bool ui_tabbar_tab_rect(const struct ui_tabbar *tb, int index,
                        struct rect2i *out);
bool ui_tabbar_close_rect(const struct ui_tabbar *tb, int index,
                          struct rect2i *out);
bool ui_tabbar_new_tab_rect(const struct ui_tabbar *tb, struct rect2i *out);

void ui_tabbar_set_external_drag(struct ui_tabbar *tb, bool active,
                                 struct vector2i pos);
int ui_tabbar_external_slot(const struct ui_tabbar *tb);

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
