#ifndef WSAP_UI_BOX_H
#define WSAP_UI_BOX_H

#include <stdbool.h>

#include "ui/widget.h"

struct ui_box;

enum ui_box_direction {
    UI_BOX_ROW = 0,
    UI_BOX_COLUMN,
    UI_BOX_STACK,
};

enum ui_box_align {
    UI_BOX_ALIGN_START = 0,
    UI_BOX_ALIGN_CENTER,
    UI_BOX_ALIGN_END,
    UI_BOX_ALIGN_STRETCH,
};

enum ui_box_justify {
    UI_BOX_JUSTIFY_START = 0,
    UI_BOX_JUSTIFY_CENTER,
    UI_BOX_JUSTIFY_END,
};

/* A row, a column, or a stack of children. Hidden children take no
 * space.
 *
 * The setters give the defaults. CSS on the box overrides them:
 * flex-direction (row or column), align-items, justify-content, gap, and
 * padding per edge. Child axes use fit, grow, fixed, or percent sizing.
 *
 * Clay computes child sizes and positions. Grow children share space.
 * Fixed sizes do not shrink.
 * Fit sizes shrink to their minimum size. */
struct ui_box {
    struct ui_widget widget;
    struct {
        enum ui_box_direction direction;
        enum ui_box_align align;
        enum ui_box_justify justify;
        struct insets2i padding;
        int gap;
    } impl;
};

void ui_box_init(struct ui_box *box);

static inline struct ui_widget *ui_box_widget(struct ui_box *box)
{
    if (box && !box->widget.ops)
        ui_box_init(box);
    return box ? &box->widget : NULL;
}
void ui_box_set_direction(struct ui_box *box, enum ui_box_direction direction);
void ui_box_set_align(struct ui_box *box, enum ui_box_align align);
void ui_box_set_justify(struct ui_box *box, enum ui_box_justify justify);
void ui_box_set_gap(struct ui_box *box, int gap);
void ui_box_set_padding(struct ui_box *box, struct insets2i padding);


#endif
