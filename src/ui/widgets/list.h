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

struct ui_list_item {
    const char *label;
    const char *shortcut;
    int action;
    uint32_t flags;
};

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
        bool stop_at_edges;
        bool allow_focus;
    } impl;
};

void ui_list_init(struct ui_list *l);
void ui_list_set_focusable(struct ui_list *l, bool focusable);
bool ui_list_set_items(struct ui_list *l, const struct ui_list_item *items,
                       int n);
void ui_list_set_style_class(struct ui_list *l, const char *class_name);
void ui_list_set_wrap_navigation(struct ui_list *l, bool wrap);
void ui_list_pin_metrics(struct ui_list *l, const struct ui_list *like);

int ui_list_count(const struct ui_list *l);
const struct ui_list_item *ui_list_item(const struct ui_list *l, int index);
int ui_list_selected(const struct ui_list *l);
int ui_list_hovered(const struct ui_list *l);
void ui_list_select(struct ui_list *l, int index);
void ui_list_select_edge(struct ui_list *l, int dir);
int ui_list_step(struct ui_list *l, int dir);

struct rect2i ui_list_item_rect(const struct ui_list *l, int index);
int ui_list_label_inset(const struct ui_list *l);
struct insets2i ui_list_shadow(const struct ui_list *l);

static inline struct ui_widget *ui_list_widget(struct ui_list *l)
{
    return l ? &l->widget : NULL;
}

#endif
