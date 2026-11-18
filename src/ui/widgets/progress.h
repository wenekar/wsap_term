#ifndef WSAP_UI_WIDGETS_PROGRESS_H
#define WSAP_UI_WIDGETS_PROGRESS_H

#include <stdbool.h>

#include "ui/widget.h"

/* A horizontal progress bar. A value from 0 to 1 fills the track. An
 * indeterminate bar shows its busy state and animates its fill. */
struct ui_progress {
    struct ui_widget widget;
    struct {
        float value;
        uint64_t next_tick;
        int phase;
        bool indeterminate;
    } state;
};

void ui_progress_init(struct ui_progress *progress);
/* Clamp the value to 0 to 1. */
void ui_progress_set_value(struct ui_progress *progress, float value);
float ui_progress_value(const struct ui_progress *progress);
void ui_progress_set_indeterminate(struct ui_progress *progress,
                                   bool indeterminate);
bool ui_progress_is_indeterminate(const struct ui_progress *progress);

static inline struct ui_widget *ui_progress_widget(struct ui_progress *progress)
{
    if (progress && !progress->widget.ops)
        ui_progress_init(progress);
    return progress ? &progress->widget : NULL;
}

#endif
