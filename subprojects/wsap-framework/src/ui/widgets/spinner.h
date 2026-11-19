#ifndef WSAP_UI_WIDGETS_SPINNER_H
#define WSAP_UI_WIDGETS_SPINNER_H

#include <stdbool.h>

#include "ui/widget.h"

/* A spinning busy indicator. It takes one text cell by default. */
struct ui_spinner {
    struct ui_widget widget;
    struct {
        uint64_t next_tick;
        int phase;
        int cell_span;
        bool running;
    } state;
};

void ui_spinner_init(struct ui_spinner *spinner);
/* Set the square size in text cells. A value below 1 becomes 1. */
void ui_spinner_set_cell_span(struct ui_spinner *spinner, int cells);
int ui_spinner_cell_span(const struct ui_spinner *spinner);
/* Stop or start the animation. A stopped spinner keeps its phase. */
void ui_spinner_set_running(struct ui_spinner *spinner, bool running);
bool ui_spinner_is_running(const struct ui_spinner *spinner);

static inline struct ui_widget *ui_spinner_widget(struct ui_spinner *spinner)
{
    if (spinner && !spinner->widget.ops)
        ui_spinner_init(spinner);
    return spinner ? &spinner->widget : NULL;
}

#endif
