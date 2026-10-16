#ifndef WSAP_UI_WIDGETS_LIST_H
#define WSAP_UI_WIDGETS_LIST_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/css.h"
#include "ui/widget.h"
#include "util/geom.h"

#define UI_LIST_ITEM_DISABLED  (1u << 0)
#define UI_LIST_ITEM_CHECKED   (1u << 1)
#define UI_LIST_ITEM_SEPARATOR (1u << 2)
#define UI_LIST_ITEM_SUBMENU   (1u << 3)

/* An item without a label is a separator. */
struct ui_list_item {
    const char *label;
    const char *shortcut;
    int action;
    uint32_t flags;
};

/* Row geometry, resolved from the theme in the list's scale. */
struct ui_list_metrics {
    struct css_scale_context scale;
    struct insets2i item_pad;
    struct insets2i sep_margin;
    struct insets2i shadow;
    int cell_w;
    int cell_h;
    int min_h;
    int gap;
    int sep_thickness;
    int edge_pad;
    bool valid;
};

struct ui_list;

struct ui_list_row {
    struct ui_widget widget;
    struct ui_list *list;
    int index;
};

/*
 * A vertical list of items with labels and shortcuts. Each item is a row
 * widget. The pointer and Up, Down, Home, End, and Enter choose items. Up and
 * Down wrap by default. Call ui_list_set_wrap_navigation() to stop at the
 * ends.
 *
 * Actions: UI_ACTION_HIGHLIGHT when the item under the pointer changes,
 * UI_ACTION_CHANGE when the selection changes, and UI_ACTION_ACTIVATE on
 * a click or Enter. Each carries the item's action as id and its index.
 *
 * CSS: the list is ".menu-list" plus its own class. Items are
 * ".menu-item" (with "submenu" and "disabled" classes and :hover, :focus,
 * :disabled, and :checked), ".menu-label", ".menu-shortcut", and
 * ".menu-separator".
 */
struct ui_list {
    struct ui_widget widget;
    struct {
        struct ui_list_item *items;
        struct ui_list_row *rows;
        int n;
        int hovered;
        int selected;
        char style_class[64];
        struct ui_list_metrics metrics;
        bool pinned;
        /* Zero keeps the normal wrap behavior. */
        bool stop_at_edges;
        bool allow_focus;
    } impl;
};

void ui_list_init(struct ui_list *l);
/* Sets whether this list can take keyboard focus. Default is true. */
void ui_list_set_focusable(struct ui_list *l, bool focusable);
/* Copies the items. The selection and the hover reset; attachment,
 * focus, and handlers stay. */
bool ui_list_set_items(struct ui_list *l, const struct ui_list_item *items,
                       int n);
/* An extra class for theme rules, such as "context-menu". */
void ui_list_set_style_class(struct ui_list *l, const char *class_name);
/* Sets whether Up and Down stop at the first and last item. */
void ui_list_set_wrap_navigation(struct ui_list *l, bool wrap);
/* Keeps the row geometry fixed while colors follow the theme, so rows
 * cannot move under the pointer during a theme preview. With like, the
 * list takes that list's geometry. */
void ui_list_pin_metrics(struct ui_list *l, const struct ui_list *like);

int ui_list_count(const struct ui_list *l);
const struct ui_list_item *ui_list_item(const struct ui_list *l, int index);
/* -1 when none. */
int ui_list_selected(const struct ui_list *l);
int ui_list_hovered(const struct ui_list *l);
void ui_list_select(struct ui_list *l, int index);
/* Selects the first or last selectable item for dir 1 or -1. */
void ui_list_select_edge(struct ui_list *l, int dir);
/* Selects the next selectable item in dir, wrapping. Returns its index. */
int ui_list_step(struct ui_list *l, int dir);

/* The row of an item, in surface space. */
struct rect2i ui_list_item_rect(const struct ui_list *l, int index);
/* Distance from the list's left edge to the item labels. */
int ui_list_label_inset(const struct ui_list *l);
/* Room the drop shadow takes outside the list's bounds. */
struct insets2i ui_list_shadow(const struct ui_list *l);

static inline struct ui_widget *ui_list_widget(struct ui_list *l)
{
    return l ? &l->widget : NULL;
}

#endif
