#ifndef WSAP_UI_WIDGETS_SLIDER_H
#define WSAP_UI_WIDGETS_SLIDER_H

#include <stdbool.h>

#include "ui/widget.h"

/* A horizontal slider. The value stays inside the range. A positive step
 * quantizes it. The slider sends UI_ACTION_CHANGE with the value in
 * thousandths in `arg`. */
struct ui_slider {
    struct ui_widget widget;
    struct {
        float min, max, value, step;
        int action_id;
        unsigned flags;
        bool disabled;
        bool focused;
    } state;
};

void ui_slider_init(struct ui_slider *slider);
/* Set the range. The arguments swap when the maximum is smaller than the
 * minimum. The value is clamped to the new range. */
void ui_slider_set_range(struct ui_slider *slider, float min, float max);
void ui_slider_set_step(struct ui_slider *slider, float step);
/* Clamp the value to the range and the step. Sends no action. */
void ui_slider_set_value(struct ui_slider *slider, float value);
float ui_slider_value(const struct ui_slider *slider);
float ui_slider_min(const struct ui_slider *slider);
float ui_slider_max(const struct ui_slider *slider);
void ui_slider_set_disabled(struct ui_slider *slider, bool disabled);
bool ui_slider_is_disabled(const struct ui_slider *slider);
void ui_slider_set_action_id(struct ui_slider *slider, int id);

static inline struct ui_widget *ui_slider_widget(struct ui_slider *slider)
{
    if (slider && !slider->widget.ops)
        ui_slider_init(slider);
    return slider ? &slider->widget : NULL;
}

#endif
