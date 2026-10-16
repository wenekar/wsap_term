#ifndef TERMINAL_SELECTION_H
#define TERMINAL_SELECTION_H
#include "input/pointer.h"
#include <wsap_vtcore/wsap_vtcore.h>

/* Pointer state of a text selection: click counting and the last pointer
 * cell. The terminal holds the selected range itself. */
struct text_selection {
    uint64_t time_ms;
    int clicks, x, y, view_offset;
    struct vector2i pointer;
    bool pressed;
    bool right_half;
    /* Milliseconds between clicks of a multi-click. Zero uses the default. */
    uint32_t double_click_ms;
};

void selection_handle_pointer(struct text_selection *s, struct wsap_vtcore *t, struct vector2i cell,
                       bool right_half, enum pointer_kind kind, uint64_t time_ms);

/* Extend the selection to the last pointer cell, for example after the view
 * scrolled under a held button. */
void selection_extend_to_pointer(const struct text_selection *s, struct wsap_vtcore *t);
#endif
