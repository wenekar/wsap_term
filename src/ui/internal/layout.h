#ifndef WSAP_UI_INTERNAL_LAYOUT_H
#define WSAP_UI_INTERNAL_LAYOUT_H

#include "ui/box.h"
#include "ui/constraints.h"

struct ui_box_params {
    struct insets2i padding;
    int gap;
    enum ui_box_direction direction;
    enum css_align align;
    enum css_justify justify;
};

struct ui_box_params ui_box_params(const struct ui_box *box,
                                   struct vector2i basis);
bool ui_widget_is_box(const struct ui_widget *widget);
struct vector2i ui_layout_measure(struct ui_widget *root,
                                   struct ui_constraints constraints);
void ui_layout_place(struct ui_widget *root, struct rect2i bounds);

#endif
