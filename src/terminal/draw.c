#include "terminal/internal.h"
#include "ui/surface.h"

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
            double scale = window_scale(w);
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

static uint64_t mix_hash(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

bool terminal_cursor_overlay_prepare(const struct terminal *t,
                                     const struct terminal_frame_source *source,
                                     struct vector2i canvas_size, uint64_t now_ms,
                                     struct terminal_cursor_overlay *out)
{
    if (!out)
        return false;
    *out = (struct terminal_cursor_overlay){0};
    /* The bell time stays set after the bell ends, so compare it. */
    if (!t || !t->focused || !source || !source->snapshot || !source->font ||
        t->view.drop_hover || t->view.preedit.text[0] ||
        t->view.visual_bell_until_ms > now_ms || t->view.blink_cells)
        return false;

    const struct wsap_vtcore_view *v = wsap_vtcore_snapshot_view(source->snapshot);
    if (!v || !v->cursor.visible || !v->cursor.blink)
        return false;

    const struct font_metrics *m = font_instance_metrics(source->font);
    int cw = m ? m->cell_w : 0, ch = m ? m->cell_h : 0;
    if (cw <= 0 || ch <= 0)
        return false;

    struct rect2i grid = source->rect;
    if (grid.x < 0 || grid.y < 0 || !rect2i_clip(&grid, canvas_size))
        return false;
    int cols = geom_min_i(grid.width / cw, v->size.x);
    int rows = geom_min_i(grid.height / ch, v->size.y);
    int cx = v->cursor.pos.x, cy = v->cursor.pos.y;
    if (cx < 0 || cx >= cols || cy < 0 || cy >= rows)
        return false;
    if (wsap_vtcore_snapshot_row_size(source->snapshot, cy) != WSAP_VTCORE_LINE_SINGLE)
        return false;

    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(source->snapshot, cy);
    int span_begin = cx, span_cols = 1;
    if (cells) {
        if (cells[cx].width == 2 && cx + 1 < cols && cells[cx + 1].width == 0)
            span_cols = 2;
        else if (cells[cx].width == 0 && cx > 0 && cells[cx - 1].width == 2) {
            span_begin = cx - 1;
            span_cols = 2;
        }
    }

    struct vector2i cell_pos = {
        .x = grid.x + span_begin * cw,
        .y = grid.y + cy * ch,
    };
    int t_px = (m->pixel_size * 3 + 32) / 64;
    if (t_px < 1)
        t_px = 1;
    if (t_px > ch)
        t_px = ch;

    struct rect2i rect = {0};
    bool solid_only = true;
    uint32_t color = v->palette[WSAP_VTCORE_PALETTE_CURSOR];
    switch (v->cursor.style) {
    case WSAP_VTCORE_CURSOR_BAR:
        rect = (struct rect2i){
            .pos = cell_pos,
            .size = { .x = geom_min_i(cw, 2 * t_px), .y = ch },
        };
        break;
    case WSAP_VTCORE_CURSOR_UNDERLINE: {
        int cu = ch < 2 * t_px ? ch : 2 * t_px;
        rect = (struct rect2i){
            .pos = { .x = cell_pos.x, .y = cell_pos.y + ch - cu },
            .size = { .x = span_cols * cw, .y = cu },
        };
        break;
    }
    case WSAP_VTCORE_CURSOR_BLOCK: {
        rect = (struct rect2i){
            .pos = cell_pos,
            .size = { .x = span_cols * cw, .y = ch },
        };
        /* A link draws an underline, also on a blank cell. */
        const uint32_t decor_mask = WSAP_VTCORE_ATTR_UNDERLINE_MASK |
            WSAP_VTCORE_ATTR_STRIKE | WSAP_VTCORE_ATTR_OVERLINE |
            WSAP_VTCORE_ATTR_LINK;
        if (cells) {
            for (int i = 0; i < span_cols; i++) {
                const struct wsap_vtcore_cell *c = &cells[span_begin + i];
                bool blank_glyph = !c->text || c->text == ' ' ||
                    (c->attrs & WSAP_VTCORE_ATTR_CONCEAL);
                if (!blank_glyph || (c->attrs & decor_mask)) {
                    solid_only = false;
                    break;
                }
            }
        }
        break;
    }
    default:
        return false;
    }

    if (rect.x + rect.width > grid.x + grid.width ||
        rect.y + rect.height > grid.y + grid.height)
        return false;

    /* The pixels of the cursor cell depend on the whole row: a neighbor can
     * change the glyph, and the underline colors are not in the cell. The
     * row key covers all of that. A row without a key gives key 0, and the
     * window then takes new pixels at each frame. */
    uint64_t key = 0;
    uint64_t row_key = solid_only ? 0 : wsap_vtcore_snapshot_row_key(source->snapshot, cy);
    if (row_key) {
        struct wsap_vtcore_span sel = wsap_vtcore_snapshot_row_selection(source->snapshot, cy);
        bool selected = sel.end > sel.begin &&
            span_begin < sel.end && span_begin + span_cols > sel.begin;
        key = mix_hash(0x123456789abcdef0ull, font_instance_version(source->font));
        key = mix_hash(key, v->palette_version);
        key = mix_hash(key, color);
        key = mix_hash(key, (uint64_t)rect.width << 32 | (uint32_t)rect.height);
        key = mix_hash(key, row_key);
        key = mix_hash(key, (uint64_t)(uint32_t)span_begin << 2 |
                            (uint64_t)v->reverse_video << 1 | (uint64_t)selected);
        if (!key)
            key = 1;
    }

    *out = (struct terminal_cursor_overlay){
        .visible = source->cursor_visible,
        .solid_only = solid_only,
        .rect = rect,
        .color = color,
        .span_begin = span_begin,
        .span_cols = span_cols,
        .cell_key = key,
    };
    return true;
}

bool terminal_cursor_blink_only(struct terminal *t, uint64_t now_ms,
                                bool *out_visible, uint64_t *out_next_ms)
{
    if (!t || !t->window || !out_visible || !out_next_ms ||
        t->view.visual_bell_until_ms > now_ms || t->view.blink_cells)
        return false;
    uint64_t next_blink = terminal_next_blink_ms(t, now_ms);
    if (!next_blink)
        return false;

    struct ui_surface *ui = window_ui_surface(t->window);
    t->view.blink_suppressed = true;
    bool due = ui_surface_tick(ui, now_ms);
    t->view.blink_suppressed = false;
    if (due || ui_surface_frame_pending(ui))
        return false;

    uint64_t ui_next = ui_surface_next_deadline(ui);
    *out_visible = terminal_cursor_visible(t, now_ms);
    *out_next_ms = (ui_next && ui_next < next_blink) ? ui_next : next_blink;
    return true;
}
