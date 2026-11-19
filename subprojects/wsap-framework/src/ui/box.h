#ifndef WSAP_UI_BOX_H
#define WSAP_UI_BOX_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/widget.h"

struct ui_box;

enum ui_box_direction : uint8_t {
    UI_BOX_ROW = 0,
    UI_BOX_COLUMN,
    UI_BOX_STACK,
};

struct ui_box {
    struct ui_widget widget;
    struct {
        enum ui_box_direction direction;
    } state;
};

void ui_box_init(struct ui_box *box);

static inline struct ui_widget *ui_box_widget(struct ui_box *box)
{
    if (box && !box->widget.ops)
        ui_box_init(box);
    return box ? &box->widget : NULL;
}
void ui_box_set_direction(struct ui_box *box, enum ui_box_direction direction);

#endif
