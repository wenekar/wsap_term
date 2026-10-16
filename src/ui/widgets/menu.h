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

/* Returns the items of the submenu that item action opens, or NULL. The
 * items stay valid until the call returns; the menu copies them. */
typedef const ui_menu_item *(*ui_menu_submenu_fn)(void *userdata, int action,
                                                  int *n);

struct ui_menu;

/* One open level: a list in a widget the host shows, inline or in a
 * popup surface of its own. */
struct ui_menu_level {
    struct ui_widget widget;
    struct ui_list list;
    struct ui_menu *menu;
    int depth;
    /* The item of the level above that opened this one. */
    int parent_item;
};

/*
 * A context menu with submenus. The menu is not a widget: it owns one
 * level widget per open depth, and the host shows each one, in its own
 * surface or as an overlay.
 *
 * The pointer, the arrow keys, Home, End, Enter, Escape, Backspace, Ctrl+C,
 * and the first letter of a label drive it. Hovering a submenu item opens the
 * submenu after the hover delay. A pointer that leaves the item toward
 * the open submenu keeps it open for a short grace time.
 *
 * Actions go to the handler set with ui_menu_set_action_handler():
 * UI_ACTION_HIGHLIGHT when the item under the pointer or the selection
 * changes, UI_ACTION_ACTIVATE for a chosen item, and UI_ACTION_CLOSE once
 * when the menu closes, after any ACTIVATE.
 *
 * Level storage lives in the menu, so a handler can close the menu from
 * inside its own dispatch. Keep the menu alive while a layer uses it. The
 * row geometry is fixed while the menu is open, so a theme preview changes
 * only colors.
 */
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
        /* Level that opens its submenu at hover_deadline, plus one. */
        int pending_level;
        int pending_item;
        uint64_t hover_deadline;
        /* Level kept open toward the pointer until aim_deadline, plus one. */
        int aim_level;
        uint64_t aim_deadline;
        uint64_t now_ms;
        struct vector2i last_pointer;
        int highlight;
        bool highlight_known;
    } impl;
};

struct ui_menu_layer;

/* Data-driven menu descriptor for opening a menu in one call. Items are
 * copied. The menu borrows userdata for its callbacks until they are
 * replaced or the menu is destroyed. */
struct ui_menu_desc {
    struct vector2i pos;
    const ui_menu_item *items;
    int count;
    ui_action_fn on_action;
    ui_menu_submenu_fn on_submenu;
    void *userdata;
    uint32_t hover_delay_ms;
};

/* Configures and opens a menu from a descriptor. Sets origin on layer
 * when layer is non-NULL. Returns false when the menu fails to open. */
bool ui_menu_show(struct ui_menu *menu, struct ui_menu_layer *layer,
                  const struct ui_menu_desc *desc);

void ui_menu_init(struct ui_menu *m);
void ui_menu_destroy(struct ui_menu *m);
/* Callback userdata is borrowed until the handler is changed or the menu
 * is destroyed. */
void ui_menu_set_action_handler(struct ui_menu *m, ui_action_fn fn,
                                void *userdata);
/* Callback userdata is borrowed until the provider is changed or the menu
 * is destroyed. */
void ui_menu_set_submenu_provider(struct ui_menu *m, ui_menu_submenu_fn fn,
                                  void *userdata);
/* Zero restores the default delay. */
void ui_menu_set_hover_delay(struct ui_menu *m, uint32_t ms);

bool ui_menu_open(struct ui_menu *m, const ui_menu_item *items, int n);
void ui_menu_close(struct ui_menu *m);
bool ui_menu_is_open(const struct ui_menu *m);

/* Changes each time a level opens, closes, or takes new items. */
uint32_t ui_menu_generation(const struct ui_menu *m);
int ui_menu_level_count(const struct ui_menu *m);
/* The level widget at depth, or NULL when that level is closed. Show it
 * where the level belongs: depth 0 at the point the menu opened, and a
 * deeper level beside ui_menu_level_anchor(). */
struct ui_widget *ui_menu_level_widget(struct ui_menu *m, int depth);
/* The item that opened level depth, in the surface space of the level
 * above it, moved up so the two first rows line up. */
struct rect2i ui_menu_level_anchor(const struct ui_menu *m, int depth);
/* Room the level's shadow takes outside its bounds. */
struct insets2i ui_menu_level_shadow(const struct ui_menu *m, int depth);

/* Runs the hover delay and the aim grace. Returns the next time the menu
 * needs a tick, or 0. */
uint64_t ui_menu_tick(struct ui_menu *m, uint64_t now_ms);

/*
 * Shows the open levels of a menu inline, over the widgets below it. Add
 * it as the last child of a parent without its own layout. While a level
 * shows, the layer is modal: a press outside the levels closes the menu.
 */
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

/* Borrows menu. Keep it alive while the layer can be prepared or used. */
void ui_menu_layer_init(struct ui_menu_layer *layer, struct ui_menu *menu);
/* Level 0 opens at origin, in the parent's surface space. */
void ui_menu_layer_set_origin(struct ui_menu_layer *layer,
                              struct vector2i origin);
/* Levels below first show elsewhere, such as in native popups.
 * The value is clamped to [0, UI_MENU_MAX_LEVELS]. */
void ui_menu_layer_set_first_level(struct ui_menu_layer *layer, int first);

static inline struct ui_widget *ui_menu_layer_widget(struct ui_menu_layer *layer)
{
    return layer ? &layer->widget : NULL;
}

#endif
