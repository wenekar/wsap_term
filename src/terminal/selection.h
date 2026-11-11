#ifndef TERMINAL_SELECTION_H
#define TERMINAL_SELECTION_H
#include "input/pointer.h"
#include <wsap_vtcore/wsap_vtcore.h>

struct text_selection {
    uint64_t time_ms;
    struct vector2i pointer;
    int clicks, x, y, view_offset;
    uint32_t double_click_ms;
    bool pressed;
    bool right_half;
};

void selection_handle_pointer(struct text_selection *s, struct wsap_vtcore *t, struct vector2i cell,
                       bool right_half, enum pointer_kind kind, uint64_t time_ms);

void selection_extend_to_pointer(const struct text_selection *s, struct wsap_vtcore *t);
#endif
