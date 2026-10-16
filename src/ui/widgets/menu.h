#ifndef WSAP_UI_WIDGETS_MENU_H
#define WSAP_UI_WIDGETS_MENU_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/action.h"
#include "ui/widget.h"
#include "ui/widgets/list.h"
#include "util/geom.h"

typedef struct ui_list_item ui_menu_item;

#define UI_MENU_ITEM(lbl, act, ...) \
    ((ui_menu_item){ .label = (lbl), .action = (act), __VA_ARGS__ })

#define UI_MENU_SEPARATOR \
    ((ui_menu_item){ .flags = UI_LIST_ITEM_SEPARATOR })

#define UI_MENU_MAX_LEVELS 8

typedef const ui_menu_item *(*ui_menu_submenu_fn)(void *userdata, int action,
                                                  int *n);

struct ui_menu;

struct ui_menu_level {
    struct ui_widget widget;
    struct ui_list list;
    struct ui_menu *menu;
    int depth;
    int parent_item;
};

struct ui_menu {
    struct ui_menu_level levels[UI_MENU_MAX_LEVELS];
    struct {
        struct ui_action_handler actions;
        ui_menu_submenu_fn submenu;
        void *submenu_userdata;
        uint32_t hover_delay_ms;
        uint32_t generation;
        int n_levels;
        bool open;
        int pending_level;
        int pending_item;
        uint64_t hover_deadline;
        int aim_level;
        uint64_t aim_deadline;
        uint64_t now_ms;
        struct vector2i last_pointer;
        int highlight;
        bool highlight_known;
    } impl;
};

struct ui_menu_layer;

struct ui_menu_desc {
    struct vector2i pos;
    const ui_menu_item *items;
    int count;
    ui_action_fn on_action;
    ui_menu_submenu_fn on_submenu;
    void *userdata;
    uint32_t hover_delay_ms;
};

bool ui_menu_show(struct ui_menu *menu, struct ui_menu_layer *layer,
                  const struct ui_menu_desc *desc);

void ui_menu_init(struct ui_menu *m);
void ui_menu_destroy(struct ui_menu *m);
void ui_menu_set_action_handler(struct ui_menu *m, ui_action_fn fn,
                                void *userdata);
void ui_menu_set_submenu_provider(struct ui_menu *m, ui_menu_submenu_fn fn,
                                  void *userdata);
void ui_menu_set_hover_delay(struct ui_menu *m, uint32_t ms);

bool ui_menu_open(struct ui_menu *m, const ui_menu_item *items, int n);
void ui_menu_close(struct ui_menu *m);
bool ui_menu_is_open(const struct ui_menu *m);

uint32_t ui_menu_generation(const struct ui_menu *m);
int ui_menu_level_count(const struct ui_menu *m);
struct ui_widget *ui_menu_level_widget(struct ui_menu *m, int depth);
struct rect2i ui_menu_level_anchor(const struct ui_menu *m, int depth);
struct insets2i ui_menu_level_shadow(const struct ui_menu *m, int depth);

uint64_t ui_menu_tick(struct ui_menu *m, uint64_t now_ms);

struct ui_menu_layer {
    struct ui_widget widget;
    struct {
        struct ui_menu *menu;
        struct vector2i origin;
        int first_level;
        uint32_t shown;
        bool stale;
    } impl;
};

void ui_menu_layer_init(struct ui_menu_layer *layer, struct ui_menu *menu);
void ui_menu_layer_set_origin(struct ui_menu_layer *layer,
                              struct vector2i origin);
void ui_menu_layer_set_first_level(struct ui_menu_layer *layer, int first);

static inline struct ui_widget *ui_menu_layer_widget(struct ui_menu_layer *layer)
{
    return layer ? &layer->widget : NULL;
}

#endif
