#ifndef WSAP_UI_WIDGETS_TOGGLE_H
#define WSAP_UI_WIDGETS_TOGGLE_H

#include <stdbool.h>

#include "ui/widget.h"

enum ui_toggle_kind {
    UI_TOGGLE_CHECKBOX = 0,
    UI_TOGGLE_RADIO,
    UI_TOGGLE_SWITCH,
};

/* A checkbox, a radio button, or a switch. They share one state model:
 * a label, a checked flag, and a disabled flag.

 * A radio button does not clear itself. Its group does. For that, set the
 * checked flag of the group members. */
struct ui_toggle {
    struct ui_widget widget;
    struct {
        char *label;
        int action_id;
        unsigned flags;
        enum ui_toggle_kind kind;
        bool checked;
        bool disabled;
        bool focused;
    } state;
};

void ui_checkbox_init(struct ui_toggle *toggle);
void ui_radio_init(struct ui_toggle *toggle);
void ui_switch_init(struct ui_toggle *toggle);
void ui_toggle_init(struct ui_toggle *toggle, enum ui_toggle_kind kind);

enum ui_toggle_kind ui_toggle_kind_of(const struct ui_toggle *toggle);
/* Copy the label. Pass NULL to clear it. A failure keeps the old label. */
bool ui_toggle_set_label(struct ui_toggle *toggle, const char *label);
const char *ui_toggle_label(const struct ui_toggle *toggle);
/* Set the checked state without sending an action. */
void ui_toggle_set_checked(struct ui_toggle *toggle, bool checked);
bool ui_toggle_is_checked(const struct ui_toggle *toggle);
void ui_toggle_set_disabled(struct ui_toggle *toggle, bool disabled);
bool ui_toggle_is_disabled(const struct ui_toggle *toggle);
void ui_toggle_set_action_id(struct ui_toggle *toggle, int id);

static inline struct ui_widget *ui_toggle_widget(struct ui_toggle *toggle)
{
    if (toggle && !toggle->widget.ops)
        ui_toggle_init(toggle, UI_TOGGLE_CHECKBOX);
    return toggle ? &toggle->widget : NULL;
}

#endif
