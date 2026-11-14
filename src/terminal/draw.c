#include "terminal/internal.h"

struct ui_scroll_metrics terminal_scroll_metrics(const struct wsap_vtcore *term)
{
    struct wsap_vtcore_scrollback sb = wsap_vtcore_scrollback(term);
    return (struct ui_scroll_metrics){
        .history_lines = sb.total,
        .visible_lines = wsap_vtcore_size(term).y,
        .view_offset = sb.offset,
    };
}

bool terminal_cursor_visible(const struct terminal *t, uint64_t now_ms)
{
    if (!t || !t->session.term)
        return false;
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(t->session.term);
    if (!cursor.visible)
        return false;
    if (!t->focused || !cursor.blink)
        return true;
    uint64_t last_key = session_last_keystroke_ms(&t->session);
    uint64_t elapsed = last_key > 0 && now_ms >= last_key ? now_ms - last_key : now_ms;
    return (elapsed / 500) % 2 == 0;
}

void terminal_show_scrollbar(struct terminal *t)
{
    if (t)
        ui_scrollview_flash(&t->scrollview);
}

void terminal_update_ime(struct terminal *t, const struct wsap_vtcore *term)
{
    struct window *w = t->window;
    if (!w || !term)
        return;
    int ch = terminal_font_metrics(t)->cell_h;
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(term);
    int row = cursor.pos.y + wsap_vtcore_scrollback(term).offset;
    if (row < 0 || row >= wsap_vtcore_size(term).y)
        row = cursor.pos.y;
    int cw = terminal_row_cell_width(t, row);
    struct rect2i caret = {
        .pos = vector2i_add(terminal_view_rect(t).pos, (struct vector2i){
            .x = cursor.pos.x * cw, .y = row * ch,
        }),
        .size = { .x = cw, .y = ch },
    };
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(t->snapshot);
    int begin = -1, end = -1;
    struct yasl_surface *s = window_surface(w);
    if (s) {
        if (!rect2i_eq(t->ime_caret, caret)) {
            t->ime_caret = caret;
            double scale = window_scale_120(w) / 120.0;
            yasl_surface_set_text_input(s, &(struct yasl_text_input){
                .purpose = YASL_TEXT_NORMAL,
                .text = "",
                .caret_x = caret.x / scale,
                .caret_y = caret.y / scale,
                .caret_width = caret.width / scale,
                .caret_height = caret.height / scale,
            });
        }
        begin = window_ime_preedit_begin(w);
        end = window_ime_preedit_end(w);
    }
    terminal_view_set_preedit(t, window_ime_preedit(w), begin, end, caret,
                              view->palette[WSAP_VTCORE_PALETTE_FG],
                              view->palette[WSAP_VTCORE_PALETTE_BG]);
}

/* The blink scan reads every cell of the snapshot. Run it again only when
 * the snapshot view version changed. A new version covers new cells. */
void terminal_scan_blink(struct terminal_view *view,
                         const struct wsap_vtcore_snapshot *snap)
{
    const struct wsap_vtcore_view *v = wsap_vtcore_snapshot_view(snap);
    if (v->version == view->blink_version)
        return;
    view->blink_version = v->version;
    view->blink_cells = false;
    for (int y = 0; y < v->size.y; y++) {
        const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snap, y);
        for (int x = 0; cells && x < v->size.x; x++) {
            if (cells[x].attrs & WSAP_VTCORE_ATTR_BLINK) {
                view->blink_cells = true;
                return;
            }
        }
    }
}
