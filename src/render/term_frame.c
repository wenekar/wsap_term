#include "term_frame.h"
#include "font/glyph/intern.h"

const struct wsap_vtcore_cell *
terminal_frame_row(const struct terminal_frame *frame, int row)
{
    return frame && frame->source ? wsap_vtcore_snapshot_row(frame->source, row) : NULL;
}

uint64_t terminal_frame_row_key(const struct terminal_frame *frame, int row)
{
    return frame && frame->source ? wsap_vtcore_snapshot_row_key(frame->source, row) : 0;
}

void terminal_frame_row_selection(const struct terminal_frame *frame, int row,
                                  int cols, int *c0, int *c1)
{
    *c0 = cols;
    *c1 = -1;
    if (!frame || !frame->has_selection || !frame->source)
        return;
    struct wsap_vtcore_span span = wsap_vtcore_snapshot_row_selection(frame->source, row);
    if (span.end > span.begin) {
        *c0 = span.begin;
        *c1 = span.end - 1 < cols ? span.end - 1 : cols - 1;
    }
}

uint32_t terminal_frame_color(const struct terminal_frame *frame, wsap_vtcore_color c)
{
    return wsap_vtcore_view_color(wsap_vtcore_snapshot_view(frame->source), c);
}

uint32_t terminal_frame_glyph(const struct terminal_frame *frame, uint32_t text)
{
    if (!(text & WSAP_VTCORE_TEXT_CLUSTER))
        return text;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = wsap_vtcore_snapshot_cluster(frame->source, text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    if (n <= 0)
        return ' ';
    return glyph_intern(cps, n < WSAP_VTCORE_GRAPHEME_MAX_LEN ? n : WSAP_VTCORE_GRAPHEME_MAX_LEN);
}

void terminal_frame_init(struct terminal_frame *frame, const struct wsap_vtcore_snapshot *snap,
                         bool cursor_visible)
{
    *frame = (struct terminal_frame){ .cursor_y = -1 };
    if (!snap)
        return;
    const struct wsap_vtcore_view *v = wsap_vtcore_snapshot_view(snap);
    if (!v->size.x || !v->size.y)
        return;
    frame->source = snap;
    frame->cols = v->size.x;
    frame->rows = v->size.y;
    frame->view_offset = v->scroll_offset;
    frame->scrolled = v->scrolled;
    frame->full_gen = v->palette_version;
    frame->default_fg = v->palette[WSAP_VTCORE_PALETTE_FG];
    frame->default_bg = v->palette[WSAP_VTCORE_PALETTE_BG];
    frame->cursor_color = v->palette[WSAP_VTCORE_PALETTE_CURSOR];
    frame->cursor_x = v->cursor.pos.x;
    frame->cursor_style = v->cursor.style;
    frame->reverse_video = v->reverse_video;
    for (int y = 0; y < frame->rows && !frame->has_selection; y++) {
        struct wsap_vtcore_span span = wsap_vtcore_snapshot_row_selection(snap, y);
        frame->has_selection = span.end > span.begin;
    }
    if (cursor_visible && v->cursor.visible)
        frame->cursor_y = v->cursor.pos.y;
}

bool terminal_frame_valid(const struct terminal_frame *frame)
{
    return frame && frame->source != NULL;
}
