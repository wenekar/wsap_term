#ifndef WSAP_UI_WIDGETS_DROPDOWN_H
#define WSAP_UI_WIDGETS_DROPDOWN_H

#include "ui/box.h"
#include "ui/widgets/button.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/list.h"
#include "ui/widgets/scrollview.h"

struct ui_dropdown_item {
    const char *label;
    const char *value;
};

/* A searchable choice control. Items are copied. A choice sends CHANGE
 * with its item index. Programmatic selection sends no action.
 * Set an overlay parent with stack layout before opening the popup. The
 * parent is borrowed. Clear it before the parent is destroyed, or keep the
 * parent alive until the dropdown is destroyed. */
struct ui_dropdown {
    struct ui_widget widget;
    struct ui_button button;
    struct ui_widget layer;
    struct ui_box panel;
    struct ui_input search;
    struct ui_scrollview scroll;
    struct ui_list list;
    struct ui_label empty;
    struct {
        struct ui_dropdown_item *items;
        struct ui_widget *overlay;
        size_t count;
        int selected;
        int action_id;
        bool open;
    } impl;
};

void ui_dropdown_init(struct ui_dropdown *dropdown);
bool ui_dropdown_set_items(struct ui_dropdown *dropdown,
                            const struct ui_dropdown_item *items, size_t count);
void ui_dropdown_set_overlay(struct ui_dropdown *dropdown,
                              struct ui_widget *parent);
void ui_dropdown_set_value(struct ui_dropdown *dropdown, const char *value);
/* Sent as the id on UI_ACTION_CHANGE. */
void ui_dropdown_set_action_id(struct ui_dropdown *dropdown, int id);
const char *ui_dropdown_value(const struct ui_dropdown *dropdown);
bool ui_dropdown_open(struct ui_dropdown *dropdown);
void ui_dropdown_close(struct ui_dropdown *dropdown);
bool ui_dropdown_is_open(const struct ui_dropdown *dropdown);

static inline struct ui_widget *ui_dropdown_widget(struct ui_dropdown *dropdown)
{
    if (dropdown && !dropdown->widget.ops)
        ui_dropdown_init(dropdown);
    return dropdown ? &dropdown->widget : NULL;
}

#endif
