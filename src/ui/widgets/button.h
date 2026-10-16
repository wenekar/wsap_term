#ifndef WSAP_UI_WIDGETS_BUTTON_H
#define WSAP_UI_WIDGETS_BUTTON_H

#include <stdbool.h>

#include "ui/widget.h"

#define UI_BUTTON_TEXT_CAP 128

/* A focusable push button. A click, Enter, or Space sends
 * UI_ACTION_ACTIVATE with the button's action ID. CSS styles it under
 * "button", with :hover, :active, :focus, and :disabled. */
struct ui_button {
    struct ui_widget widget;
    struct {
        char text[UI_BUTTON_TEXT_CAP];
        unsigned state;
        int action_id;
    } impl;
};

void ui_button_init(struct ui_button *button);
/* Cuts the text at a UTF-8 boundary when it does not fit. */
void ui_button_set_text(struct ui_button *button, const char *text);
const char *ui_button_text(const struct ui_button *button);
void ui_button_set_action_id(struct ui_button *button, int id);
void ui_button_set_disabled(struct ui_button *button, bool disabled);
bool ui_button_is_disabled(const struct ui_button *button);
bool ui_button_is_pressed(const struct ui_button *button);

static inline struct ui_widget *ui_button_widget(struct ui_button *button)
{
    if (button && !button->widget.ops)
        ui_button_init(button);
    return button ? &button->widget : NULL;
}

#endif
