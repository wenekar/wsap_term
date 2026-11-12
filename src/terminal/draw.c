#include "terminal/internal.h"
#include "ui/yasl/stats.h"

struct ui_scroll_metrics terminal_scroll_metrics(const struct wsap_vtcore *term)
{
    wsap_vtcore_lock((struct wsap_vtcore *)term);
    struct wsap_vtcore_scrollback sb = wsap_vtcore_scrollback(term);
    struct ui_scroll_metrics m = {
        .history_lines = sb.total,
        .visible_lines = wsap_vtcore_size(term).y,
        .view_offset = sb.offset,
    };
    wsap_vtcore_unlock((struct wsap_vtcore *)term);
    return m;
}

static bool cursor_visible(const struct window *w, const struct tab *tab, uint64_t now_ms)
{
    struct terminal *tw = terminal_of(w);
    if (!tw)
        return false;
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(tw->snapshot);
    struct wsap_vtcore_cursor cursor = view->size.x
        ? view->cursor : wsap_vtcore_cursor(tab->session.term);
    if (!cursor.visible)
        return false;
    if (!tw->focused || !cursor.blink)
        return true;
    uint64_t last_key = session_last_keystroke_ms(&tab->session);
    uint64_t elapsed = last_key > 0 && now_ms >= last_key ? now_ms - last_key : now_ms;
    return (elapsed / 500) % 2 == 0;
}

void terminal_show_scrollbar(struct window *w)
{
    ui_scrollview_flash(&terminal_of(w)->scrollview);
}

static void update_ime(struct window *w)
{
    const struct font_metrics *m = terminal_metrics(w);
    int cw = m->cell_w, ch = m->cell_h;
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(terminal_of(w)->snapshot);
    struct wsap_vtcore_cursor cursor = view->cursor;
    int row = cursor.pos.y;
    if (row < 0 || row >= view->size.y)
        row = cursor.pos.y - view->scroll_offset;
    struct rect2i caret = {
        .pos = vector2i_add(terminal_view_rect(w).pos, (struct vector2i){
            .x = cursor.pos.x * cw, .y = row * ch,
        }),
        .size = { .x = cw, .y = ch },
    };
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
    struct terminal *tw = terminal_of(w);
    if (!tw)
        return;
    tw->snapshot_prepared = false;
    terminal_finish_closing_tab(w);
    struct wsap_vtcore *t = terminal_term(w);
    if (t && !window_closed(w)) {
        const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(tw->snapshot);
        bool was_blinking = view->size.x && view->cursor.visible && view->cursor.blink;
        uint64_t t0 = yasl_time_ns();
        if (wsap_vtcore_snapshot_update(tw->snapshot, t)) {
            uint64_t dt_ns = yasl_time_ns() - t0;
            frame_stats_snapshot_wait(window_stats(w), (double)dt_ns / 1e3);
            tw->snapshot_prepared = true;
            update_ime(w);
            bool now_blinking = view->cursor.visible && view->cursor.blink;
            if (now_blinking != was_blinking)
                (void)ui_surface_tick(window_ui_surface(w), now_ms);
        }
    }
}

bool terminal_frame_source(struct window *w, uint64_t now_ms,
                           struct terminal_frame_source *out)
{
    struct tab *tab = terminal_active_tab(w);
    if (!tab || !out)
        return false;
    struct terminal *tw = terminal_of(w);
    struct wsap_vtcore_snapshot *snap = tw->snapshot;
    if (!tw->snapshot_prepared) {
        uint64_t t0 = yasl_time_ns();
        if (!wsap_vtcore_snapshot_update(snap, tab->session.term))
            return false;
        uint64_t dt_ns = yasl_time_ns() - t0;
        frame_stats_snapshot_wait(window_stats(w), (double)dt_ns / 1e3);
    }
    tw->snapshot_prepared = false;
    struct rect2i rect = terminal_view_rect(w);
    *out = (struct terminal_frame_source){
        .snapshot = snap,
        .font = tab->font,
        .rect = rect,
        .cursor_visible = cursor_visible(w, tab, now_ms),
    };
    return true;
}
