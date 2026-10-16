#include "terminal/selection.h"
#include "internal.h"

static struct vector2i clamp_cell(const struct wsap_vtcore *t, struct vector2i cell)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t);
    return (struct vector2i){
        .x = cell.x < 0 ? 0 : cell.x >= size.x ? size.x - 1 : cell.x,
        .y = cell.y < 0 ? 0 : cell.y >= size.y ? size.y - 1 : cell.y,
    };
}

void selection_extend_to_pointer(const struct text_selection *s, struct wsap_vtcore *t)
{
    if (!s->pressed)
        return;
    wsap_vtcore_select_extend(t, term_vec_from_geom(clamp_cell(t, s->pointer)), s->right_half);
}

void selection_handle_pointer(struct text_selection *s, struct wsap_vtcore *t, struct vector2i cell,
                       bool right_half, enum pointer_kind kind, uint64_t time_ms)
{
    int x = cell.x, y = cell.y;
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t);
    if (!size.x || !size.y) return;
    if (x < 0) { x = 0; right_half = false; }
    if (x >= size.x) { x = size.x - 1; right_half = true; }
    y = y < 0 ? 0 : y >= size.y ? size.y - 1 : y;
    s->pointer = (struct vector2i){ .x = x, .y = y }; s->right_half = right_half;
    if (kind == PTR_PRESS) {
        int offset = wsap_vtcore_scrollback(t).offset;
        uint64_t interval = s->double_click_ms ? s->double_click_ms : 400;
        s->clicks = s->clicks && time_ms - s->time_ms <= interval && x == s->x && y == s->y
                  && offset == s->view_offset ? s->clicks % 3 + 1 : 1;
        s->x = x; s->y = y; s->view_offset = offset;
        s->time_ms = time_ms; s->pressed = true;
        enum wsap_vtcore_select_unit unit = s->clicks == 1 ? WSAP_VTCORE_SELECT_CELL
                                   : s->clicks == 2 ? WSAP_VTCORE_SELECT_WORD : WSAP_VTCORE_SELECT_LINE;
        wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = x, .y = y }, right_half, unit);
    } else {
        selection_extend_to_pointer(s, t);
    }
    if (kind == PTR_RELEASE) s->pressed = false;
}
