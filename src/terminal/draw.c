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

static bool cursor_visible(const struct window *w, const struct tab *tab, uint64_t now_ms)
{
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(tab->session.term);
    if (!cursor.visible)
        return false;
    struct terminal *tw = terminal_of(w);
    if (!tw || !tw->focused || !cursor.blink)
        return true;
    uint64_t last_key = session_last_keystroke_ms(&tab->session);
    uint64_t elapsed = last_key > 0 && now_ms >= last_key ? now_ms - last_key : now_ms;
    return (elapsed / 500) % 2 == 0;
}

void terminal_show_scrollbar(struct window *w)
{
    ui_scrollview_flash(&terminal_of(w)->scrollview);
}

static void update_ime(struct window *w, const struct wsap_vtcore *t)
{
    const struct font_metrics *m = terminal_metrics(w);
    int cw = m->cell_w, ch = m->cell_h;
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(t);
    int row = cursor.pos.y + wsap_vtcore_scrollback(t).offset;
    if (row < 0 || row >= wsap_vtcore_size(t).y)
        row = cursor.pos.y;
    struct rect2i caret = {
        .pos = vector2i_add(terminal_view_rect(w).pos, (struct vector2i){
            .x = cursor.pos.x * cw, .y = row * ch,
        }),
        .size = { .x = cw, .y = ch },
    };
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(terminal_of(w)->snapshot);
    int begin = -1, end = -1;
    struct yasl_surface *s = window_surface(w);
    if (s) {
        struct terminal *tw = terminal_of(w);
        if (!rect2i_eq(tw->ime_caret, caret)) {
            tw->ime_caret = caret;
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
    terminal_view_set_preedit(terminal_of(w),
                              window_ime_preedit(w),
                              begin, end, caret, view->palette[WSAP_VTCORE_PALETTE_FG],
                              view->palette[WSAP_VTCORE_PALETTE_BG]);
}

void terminal_prepare(struct window *w, uint64_t now_ms)
{
    (void)now_ms;
    if (!terminal_of(w))
        return;
    terminal_finish_closing_tab(w);
    struct wsap_vtcore *t = terminal_term(w);
    if (t && !window_closed(w) && wsap_vtcore_snapshot_update(terminal_of(w)->snapshot, t)) {
        update_ime(w, t);
    }
}

bool terminal_frame_source(struct window *w, uint64_t now_ms,
                           struct terminal_frame_source *out)
{
    struct tab *tab = terminal_active_tab(w);
    if (!tab || !out)
        return false;
    struct wsap_vtcore_snapshot *snap = terminal_of(w)->snapshot;
    if (!wsap_vtcore_snapshot_update(snap, tab->session.term))
        return false;
    struct rect2i rect = terminal_view_rect(w);
    *out = (struct terminal_frame_source){
        .snapshot = snap,
        .font = tab->font,
        .rect = rect,
        .cursor_visible = cursor_visible(w, tab, now_ms),
    };
    return true;
}
