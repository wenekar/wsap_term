#include "terminal/internal.h"

#include "input/input.h"
#include "input/keybind.h"
#include "platform/platform.h"
#include "terminal/drop.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/css.h"
#include "ui/surface.h"
#include "ui/widget_ops.h"
#include <wsap_vtcore/unicode/grapheme.h>
#include <wsap_vtcore/unicode/utf8.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct terminal *view_terminal(const struct ui_widget *widget)
{
    return terminal_of_view(widget);
}

static struct terminal_view *view_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct terminal_view,
                           widget);
}

static enum pointer_kind pointer_kind(enum ui_pointer_kind k)
{
    switch (k) {
    case UI_POINTER_PRESS:   return PTR_PRESS;
    case UI_POINTER_RELEASE: return PTR_RELEASE;
    case UI_POINTER_MOTION:  return PTR_MOTION;
    case UI_POINTER_SCROLL:  return PTR_SCROLL;
    default:                 return PTR_LEAVE;
    }
}

static void changed(struct terminal_view *view)
{
    ui_widget_invalidate(&view->widget, UI_WIDGET_INVALIDATE_PAINT);
}

int terminal_row_cell_width(const struct terminal *t, int row)
{
    int cw = terminal_font_metrics(t)->cell_w;
    if (t && t->snapshot && row >= 0 &&
        wsap_vtcore_snapshot_row_size(t->snapshot, row) !=
            WSAP_VTCORE_LINE_SINGLE)
        cw *= 2;
    return cw;
}

int terminal_row_cols(const struct terminal *t, int row)
{
    int cols = t && t->session.term ? wsap_vtcore_size(t->session.term).x : 1;
    if (t && t->snapshot && row >= 0 &&
        wsap_vtcore_snapshot_row_size(t->snapshot, row) !=
            WSAP_VTCORE_LINE_SINGLE)
        cols /= 2;
    return cols > 0 ? cols : 1;
}

static struct vector2i cell_at(const struct terminal *t, struct vector2i local)
{
    int ch = terminal_font_metrics(t)->cell_h;
    int y = ch > 0 && local.y >= 0 ? local.y / ch : -1;
    int cw = terminal_row_cell_width(t, y);
    int x = cw > 0 && local.x >= 0 ? local.x / cw : -1;
    /* A wide row has half as many columns as the grid. */
    if (cw > terminal_font_metrics(t)->cell_w &&
        x >= terminal_row_cols(t, y))
        x = terminal_row_cols(t, y) - 1;
    return (struct vector2i){ .x = x, .y = y };
}

bool terminal_pointer_selects_text(const struct wsap_vtcore *t)
{
    if (!t)
        return true;
    struct wsap_vtcore_modes m = wsap_vtcore_modes(t);
    return m.mouse == WSAP_VTCORE_MOUSE_OFF && !m.alternate;
}

_Static_assert(POINTER_BUTTON_NONE == (int)WSAP_VTCORE_MOUSE_BUTTON_NONE &&
               POINTER_BUTTON_EXTRA_4 == (int)WSAP_VTCORE_MOUSE_BUTTON_EXTRA_4 &&
               POINTER_WHEEL_UP == (int)WSAP_VTCORE_MOUSE_WHEEL_UP &&
               POINTER_WHEEL_RIGHT == (int)WSAP_VTCORE_MOUSE_WHEEL_RIGHT,
               "pointer and term mouse buttons differ");

/* Mode 1007 sends one cursor key for each wheel line. Keep each encode
 * inside the 64-byte buffer. */
#define ALT_SCROLL_CHUNK_LINES 16

static bool alternate_scroll(const struct wsap_vtcore_mouse_event *mev,
                             const struct wsap_vtcore_modes *modes)
{
    return !modes->mouse && modes->alternate && modes->alternate_scroll &&
        (mev->kind == WSAP_VTCORE_MOUSE_SCROLL || mev->kind == WSAP_VTCORE_MOUSE_PRESS) &&
        (mev->button == WSAP_VTCORE_MOUSE_WHEEL_UP ||
         mev->button == WSAP_VTCORE_MOUSE_WHEEL_DOWN);
}

static void send_alternate_scroll(struct terminal *t,
                                  struct wsap_vtcore_mouse_event mev,
                                  const struct wsap_vtcore_modes *modes)
{
    int total = mev.scroll_lines;
    unsigned left = total < 0 ? 0u - (unsigned)total : (unsigned)total;
    if (!left)
        left = 1;
    char buf[64];
    while (left) {
        unsigned chunk = left > ALT_SCROLL_CHUNK_LINES ? ALT_SCROLL_CHUNK_LINES : left;
        mev.scroll_lines = total < 0 ? -(int)chunk : (int)chunk;
        struct wsap_vtcore_input_result r =
            wsap_vtcore_input_encode_mouse(&mev, modes, buf, sizeof(buf));
        if (r.len)
            session_write_bytes(&t->session, buf, r.len);
        left -= chunk;
    }
}

static bool report_mouse(struct terminal *t, const struct ui_pointer_event *p,
                         struct vector2i local)
{
    struct wsap_vtcore_mouse_event mev = {
        .pos = term_vec_from_geom(cell_at(t, local)),
        .pixel = term_vec_from_geom(local),
        .button = p->button,
        .mods = p->mods,
        .scroll_lines = p->scroll_lines,
        .scroll_delta = p->scroll_delta,
        .kind = (enum wsap_vtcore_mouse_event_kind)pointer_kind(p->kind),
    };
    struct wsap_vtcore_modes modes = terminal_input_modes(t);
    if (alternate_scroll(&mev, &modes)) {
        send_alternate_scroll(t, mev, &modes);
        return true;
    }
    char buf[64];
    struct wsap_vtcore_input_result r = wsap_vtcore_input_encode_mouse(&mev, &modes, buf, sizeof(buf));
    if (r.len)
        session_write_bytes(&t->session, buf, r.len);
    return r.status != WSAP_VTCORE_INPUT_UNHANDLED;
}

static void scroll_lines(struct terminal *t, int lines)
{
    wsap_vtcore_scroll(t->session.term, lines);
    selection_extend_to_pointer(&t->selection, t->session.term);
    terminal_show_scrollbar(t);
}

static void select_text(struct terminal *t, const struct ui_pointer_event *p,
                        struct vector2i local, uint64_t now_ms)
{
    struct vector2i cell = cell_at(t, local);
    int cw = terminal_row_cell_width(t, cell.y);
    /* A wide row uses two grid columns for one cell. */
    bool double_w = cw > terminal_font_metrics(t)->cell_w;
    bool right_half;
    if (double_w && local.x >= terminal_row_cols(t, cell.y) * cw)
        right_half = true;
    else
        right_half = local.x >= 0 && cw > 0 && local.x % cw >= cw / 2;
    struct window *w = t->window;
    t->selection.double_click_ms = w ? window_double_click_ms(w) : 0;
    selection_handle_pointer(&t->selection, t->session.term, cell,
                             right_half, pointer_kind(p->kind), now_ms);
    if (p->kind == UI_POINTER_RELEASE &&
        wsap_vtcore_has_selection(t->session.term)) {
        char *text = wsap_vtcore_selection_text(t->session.term);
        if (text && w && window_surface(w))
            window_copy(w, YASL_PRIMARY, text);
        wsap_vtcore_text_free(text);
    }
}

static void secondary_press(struct terminal *t,
                            const struct ui_pointer_event *p,
                            struct vector2i local)
{
    if (terminal_pointer_selects_text(t->session.term))
        terminal_open_ctx_menu(t, p->pos);
    else if (!t->selection.pressed)
        report_mouse(t, p, local);
}

static bool pointer_is_local(const struct terminal *t)
{
    struct window *w = t->window;
    return w && ((window_modifiers(w) & INPUT_MOD_SHIFT) ||
                 terminal_pointer_selects_text(t->session.term));
}

static bool view_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct terminal_view *view = view_of(widget);
    struct terminal *t = view_terminal(widget);
    struct window *w = t->window;
    struct wsap_vtcore *term = t->session.term;
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        bool opening_link = view->opening_link;
        view->opening_link = false;
        if (!opening_link)
            t->selection.pressed = false;
        return true;
    }
    if (event->kind == UI_EVENT_KEY) {
        const struct input_key_event *key = event->data.key.native;
        /* A key that the app binds belongs to the app command. The terminal
         * declines it, so the keymap of the window in step 3 runs the
         * command. */
        if (key && input_keymap_match(window_keymap(w), key))
            return false;
        if (!key || !terminal_key(t, key))
            ui_surface_keep_key_text(ui_widget_surface(widget));
        return true;
    }
    if (event->kind == UI_EVENT_TEXT) {
        terminal_text(t, event->data.text.data, event->data.text.len);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER || !term || !w)
        return false;
    const struct ui_pointer_event *p = &event->data.pointer;
    struct vector2i local = ui_widget_to_local(widget, p->pos);
    if (p->kind == UI_POINTER_LEAVE) {
        view->hover_link = false;
        return false;
    }
    if (p->kind == UI_POINTER_PRESS && p->button == 2) {
        secondary_press(t, p, local);
        return true;
    }

    char *url = wsap_vtcore_link_at(term, term_vec_from_geom(cell_at(t, local)));
    view->hover_link = url != NULL;
    if (view->opening_link && p->button == 0 && p->kind == UI_POINTER_RELEASE) {
        view->opening_link = false;
        ui_widget_release_pointer(widget);
        wsap_vtcore_text_free(url);
        return true;
    }
    if (url && p->button == 0 && p->kind == UI_POINTER_PRESS &&
        (p->mods & (INPUT_MOD_CTRL | INPUT_MOD_SUPER))) {
        platform_open_url(url);
        view->opening_link = true;
        ui_widget_capture_pointer(widget);
        wsap_vtcore_text_free(url);
        return true;
    }
    wsap_vtcore_text_free(url);

    if (p->kind == UI_POINTER_SCROLL && !ui_pointer_is_horizontal_wheel(p) &&
        (p->scroll_lines || p->scroll_delta != 0.0f)) {
        int lines = p->scroll_lines;
        if (p->scroll_delta != 0.0f) {
            float total = view->scroll_remainder + p->scroll_delta;
            lines = (int)total;
            view->scroll_remainder = total - (float)lines;
        }
        if (!lines)
            return true;
        struct ui_pointer_event step = *p;
        step.scroll_lines = lines;
        step.scroll_delta = 0.0f;
        step.button = lines > 0 ? UI_POINTER_WHEEL_BUTTON : UI_POINTER_WHEEL_BUTTON + 1;
        if (!t->selection.pressed && report_mouse(t, &step, local))
            return true;
        scroll_lines(t, lines);
        changed(view);
        return true;
    }
    if (!t->selection.pressed && report_mouse(t, p, local))
        return true;
    if (p->button == 0 && pointer_is_local(t)) {
        if (p->kind == UI_POINTER_PRESS)
            ui_widget_capture_pointer(widget);
        else if (p->kind == UI_POINTER_RELEASE)
            ui_widget_release_pointer(widget);
        select_text(t, p, local, event->time_ms);
        changed(view);
        return true;
    }
    if (p->button == 1 && p->kind == UI_POINTER_PRESS) {
        if (window_surface(w))
            window_paste(w, YASL_PRIMARY);
        return true;
    }
    if (p->button >= UI_POINTER_WHEEL_BUTTON)
        return true;
    if (p->kind == UI_POINTER_MOTION && p->button == 3)
        t->selection.pressed = false;
    return false;
}

static enum ui_cursor view_cursor(const struct ui_widget *widget,
                                  struct vector2i pos)
{
    (void)pos;
    const struct terminal_view *view = view_of(widget);
    struct terminal *t = view_terminal(widget);
    if (!t->session.term)
        return UI_CURSOR_DEFAULT;
    if (t->selection.pressed)
        return UI_CURSOR_TEXT;
    if (!pointer_is_local(t))
        return UI_CURSOR_DEFAULT;
    if (view->hover_link)
        return UI_CURSOR_POINTER;
    char *shape = wsap_vtcore_pointer_shape(t->session.term);
    enum ui_cursor cursor = UI_CURSOR_TEXT;
    if (shape && !strcmp(shape, "pointer"))
        cursor = UI_CURSOR_POINTER;
    else if (shape && !strcmp(shape, "default"))
        cursor = UI_CURSOR_DEFAULT;
    wsap_vtcore_text_free(shape);
    return cursor;
}

static struct {
    ui_part_id drop_target;
} view_parts;

static void view_parts_init(void)
{
    if (view_parts.drop_target)
        return;
    view_parts.drop_target = ui_part_intern("terminal-drop-target");
}

static void paint_drop_hover(const struct ui_widget *widget,
                             struct ui_paint_context *paint)
{
    struct css_style st = ui_widget_part(widget, view_parts.drop_target, 0);
    float widget_opacity = css_style_effective_opacity(&widget->style);
    float opacity = widget_opacity * css_style_effective_opacity(&st);
    struct ui_paint_box_style box = ui_paint_box_style_of(
        &st, &widget->style_scale);
    box.bg_color = ui_color_opacity(
        st.has_bg_color ? st.bg_color : 0x3389B4FAu, opacity);
    box.border_color = ui_color_opacity(
        st.has_border_color ? st.border_color : 0xFF89B4FAu, opacity);
    box.border_width = ui_css_resolve_int(st.border_width, 0, 2,
                                          &widget->style_scale, true);
    ui_paint_box(paint, ui_paint_bounds(paint), &box, NULL);
}

static void view_paint(const struct ui_widget *widget,
                       struct ui_paint_context *paint)
{
    struct terminal_view *view = view_of(widget);
    if (view->drop_hover)
        paint_drop_hover(widget, paint);
    if (view->visual_bell_until_ms > yasl_time_ns() / 1000000)
        ui_paint_rect(paint, ui_paint_bounds(paint),
                      ui_color_opacity(0x40FFFFFFu,
                                       css_style_effective_opacity(&widget->style)));
    const struct terminal_preedit *p = &view->preedit;
    if (!p->text[0])
        return;
    struct ui_text_style style = {
        .advance = p->caret.width, .cell_h = p->caret.height,
    };
    size_t len = strlen(p->text);
    struct rect2i box = {
        .pos = ui_widget_to_local(widget, p->caret.pos),
        .size = {
            .x = geom_sat_i((int64_t)ui_text_run_cells(p->text, len) *
                            style.advance),
            .y = style.cell_h,
        },
    };
    int line = style.cell_h / 16 > 1 ? style.cell_h / 16 : 1;
    ui_paint_rect(paint, box, p->bg);
    ui_paint_text(paint, box, style, p->fg, p->text, len);
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = box.x, .y = box.y + box.height - line },
        .size = { .x = box.width, .y = line },
    }, p->fg);
    if (p->cursor_begin < 0)
        return;
    int x0 = box.x + geom_sat_i((int64_t)ui_text_run_cells(
                 p->text, (size_t)p->cursor_begin) * style.advance);
    int x1 = box.x + geom_sat_i((int64_t)ui_text_run_cells(
                 p->text, (size_t)p->cursor_end) * style.advance);
    if (x1 > x0)
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = x0, .y = box.y + box.height - 3 * line },
            .size = { .x = x1 - x0, .y = 3 * line },
        }, p->fg);
    else
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = x0, .y = box.y },
            .size = { .x = line, .y = box.height },
        }, p->fg);
}

struct accessibility_text_builder {
    char *text;
    size_t len;
    size_t capacity;
};

static bool accessibility_text_append(struct accessibility_text_builder *b,
                                      const char *text, size_t len)
{
    if (len > SIZE_MAX - b->len - 1)
        return false;
    size_t need = b->len + len + 1;
    if (need > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 256;
        while (capacity < need) {
            if (capacity > SIZE_MAX / 2) {
                capacity = need;
                break;
            }
            capacity *= 2;
        }
        char *next = realloc(b->text, capacity);
        if (!next)
            return false;
        b->text = next;
        b->capacity = capacity;
    }
    memcpy(b->text + b->len, text, len);
    b->len += len;
    b->text[b->len] = '\0';
    return true;
}

static int accessibility_cell_codepoints(const struct wsap_vtcore_snapshot *snapshot,
                                         const struct wsap_vtcore_cell *cell,
                                         uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN])
{
    if (!cell->text) {
        cps[0] = ' ';
        return 1;
    }
    return wsap_vtcore_snapshot_cluster(snapshot, cell->text, cps,
                                 WSAP_VTCORE_GRAPHEME_MAX_LEN);
}

static bool accessibility_cell_blank(const struct wsap_vtcore_snapshot *snapshot,
                                     const struct wsap_vtcore_cell *cell)
{
    if (!cell || !cell->width || !cell->text || cell->text == ' ')
        return true;
    if (!(cell->text & WSAP_VTCORE_TEXT_CLUSTER))
        return false;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = accessibility_cell_codepoints(snapshot, cell, cps);
    if (n <= 0)
        return true;
    for (int i = 0; i < n; i++)
        if (cps[i] != ' ')
            return false;
    return true;
}

static bool accessibility_append_cell(struct accessibility_text_builder *b,
                                      const struct wsap_vtcore_snapshot *snapshot,
                                      const struct wsap_vtcore_cell *cell)
{
    if (!cell || !cell->width)
        return true;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = accessibility_cell_codepoints(snapshot, cell, cps);
    if (n <= 0)
        return accessibility_text_append(b, " ", 1);
    for (int i = 0; i < n; i++) {
        char encoded[4];
        int bytes = wsap_vtcore_utf8_encode(cps[i], encoded);
        if (bytes <= 0 ||
            !accessibility_text_append(b, encoded, (size_t)bytes))
            return false;
    }
    return true;
}

/* The accessibility document of the terminal is the text of the history
 * rows above the view, then the text of the view rows. The provider keeps
 * the byte offset of each history row and no text. A read builds the rows
 * of its range only. */

static bool accessibility_line_add(struct terminal_view *view, size_t offset,
                                   size_t row)
{
    if (view->accessibility_line_count == view->accessibility_line_capacity) {
        size_t capacity = view->accessibility_line_capacity
            ? view->accessibility_line_capacity * 2 : 256;
        void *lines = realloc(view->accessibility_lines,
                              capacity * sizeof(*view->accessibility_lines));
        if (!lines)
            return false;
        view->accessibility_lines = lines;
        view->accessibility_line_capacity = capacity;
    }
    view->accessibility_lines[view->accessibility_line_count++] =
        (struct terminal_text_line){ .offset = offset, .row = row };
    return true;
}

/* Adds the logical lines of `text` to the index. A wrapped row makes no
 * newline, so a line can start before the first row of `text`. `base` is
 * the first history row of `text`. */
static bool accessibility_lines_add(struct terminal_view *view,
                                    const char *text, size_t base)
{
    if (!text[0])
        return true;
    size_t row = base;
    bool open = view->accessibility_open_line;
    const char *line = text;
    for (;;) {
        const char *newline = strchr(line, '\n');
        size_t bytes = newline ? (size_t)(newline - line) + 1 : strlen(line);
        if (!open &&
            !accessibility_line_add(view, view->accessibility_history_length,
                                    row))
            return false;
        view->accessibility_history_length += bytes;
        if (!newline) {
            open = true;
            break;
        }
        open = false;
        row++;
        line = newline + 1;
        if (!line[0])
            break;
    }
    view->accessibility_open_line = open;
    return true;
}

/* Counts the history rows from the document. A new row above the view adds
 * its own text to the index. A shrink of the history rebuilds the index. */
static bool accessibility_rows_sync(struct terminal_view *view,
                                    struct wsap_vtcore *term)
{
    int rows = wsap_vtcore_history_rows(term);
    if (rows < 0)
        rows = 0;
    size_t count = (size_t)rows;
    if (view->accessibility_valid && count == view->accessibility_rows)
        return true;
    size_t first = 0;
    bool rebuild = !view->accessibility_valid || count < view->accessibility_rows;
    if (rebuild) {
        view->accessibility_line_count = 0;
        view->accessibility_history_length = 0;
        view->accessibility_open_line = false;
        first = count;
    } else {
        first = count - view->accessibility_rows;
    }
    char *history = wsap_vtcore_history_text(term, (int)first);
    if (!history)
        return false;
    bool ok = accessibility_lines_add(view, history,
                                      view->accessibility_rows);
    wsap_vtcore_text_free(history);
    if (ok)
        view->accessibility_rows = count;
    view->accessibility_valid = ok;
    return ok;
}

static size_t accessibility_cell_bytes(const struct wsap_vtcore_snapshot *snapshot,
                                       const struct wsap_vtcore_cell *cell)
{
    if (!cell || !cell->width)
        return 0;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int count = accessibility_cell_codepoints(snapshot, cell, cps);
    if (count <= 0)
        return 1;
    size_t bytes = 0;
    for (int i = 0; i < count; i++) {
        char encoded[4];
        int written = wsap_vtcore_utf8_encode(cps[i], encoded);
        if (written > 0)
            bytes += (size_t)written;
    }
    return bytes;
}

/* The first column after the text and the selection of row y. */
static int accessibility_row_end_col(const struct wsap_vtcore_snapshot *snapshot,
                                     int y, int cols)
{
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
    int last_col = -1;
    for (int x = cols - 1; cells && x >= 0; x--) {
        if (!accessibility_cell_blank(snapshot, &cells[x])) {
            last_col = x;
            break;
        }
    }
    struct wsap_vtcore_span selection =
        wsap_vtcore_snapshot_row_selection(snapshot, y);
    int end_col = last_col + 1;
    if (selection.end > end_col)
        end_col = selection.end;
    if (end_col < 0)
        end_col = 0;
    if (end_col > cols)
        end_col = cols;
    return end_col;
}

/* The byte length of the text of row y, without its newline. */
static size_t accessibility_row_bytes(const struct wsap_vtcore_snapshot *snapshot,
                                      int y, int end_col)
{
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
    size_t bytes = 0;
    for (int x = 0; cells && x < end_col; x++)
        bytes += accessibility_cell_bytes(snapshot, &cells[x]);
    return bytes;
}

/* The last row that holds text, or the cursor row. */
static int accessibility_last_row(const struct wsap_vtcore_snapshot *snapshot)
{
    const struct wsap_vtcore_view *term_view = wsap_vtcore_snapshot_view(snapshot);
    int rows = term_view->size.y;
    int cols = term_view->size.x;
    int last_row = -1;
    for (int y = rows - 1; y >= 0; y--) {
        if (accessibility_row_end_col(snapshot, y, cols) > 0) {
            last_row = y;
            break;
        }
    }
    if (term_view->cursor.visible && term_view->cursor.pos.y >= 0 &&
        term_view->cursor.pos.y < rows &&
        term_view->cursor.pos.y > last_row)
        last_row = term_view->cursor.pos.y;
    return last_row;
}

/* The byte lengths and the offsets of the document. A node reports the
 * whole document, and a read starts at its own range. */
struct accessibility_document {
    size_t history_length;
    size_t length;
    size_t caret_offset;
    size_t selection_start;
    size_t selection_end;
    bool has_selection;
};

static bool accessibility_document_measure(struct terminal_view *view,
                                           struct terminal *terminal,
                                           struct accessibility_document *doc)
{
    struct wsap_vtcore *term = terminal->session.term;
    const struct wsap_vtcore_snapshot *snapshot = terminal->snapshot;
    const struct wsap_vtcore_view *term_view = wsap_vtcore_snapshot_view(snapshot);
    if (!accessibility_rows_sync(view, term))
        return false;
    *doc = (struct accessibility_document){
        .history_length = view->accessibility_history_length,
    };
    int cols = term_view->size.x;
    int last_row = accessibility_last_row(snapshot);
    int cursor_y = term_view->cursor.pos.y;
    int cursor_x = term_view->cursor.pos.x;
    size_t offset = doc->history_length;
    for (int y = 0; y <= last_row; y++) {
        int end_col = accessibility_row_end_col(snapshot, y, cols);
        const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
        if (term_view->cursor.visible && y == cursor_y) {
            int caret_col = cursor_x < 0 ? 0 : cursor_x > end_col
                ? end_col : cursor_x;
            doc->caret_offset = offset;
            for (int x = 0; cells && x < caret_col; x++)
                doc->caret_offset += accessibility_cell_bytes(snapshot, &cells[x]);
        }
        struct wsap_vtcore_span selection =
            wsap_vtcore_snapshot_row_selection(snapshot, y);
        if (selection.end > selection.begin) {
            int begin = selection.begin < 0 ? 0 : selection.begin;
            if (begin > end_col)
                begin = end_col;
            int limit = selection.end < begin ? begin : selection.end;
            if (limit > end_col)
                limit = end_col;
            if (!doc->has_selection) {
                doc->selection_start = offset;
                for (int x = 0; cells && x < begin; x++)
                    doc->selection_start +=
                        accessibility_cell_bytes(snapshot, &cells[x]);
                doc->has_selection = true;
            }
            doc->selection_end = offset;
            for (int x = 0; cells && x < limit; x++)
                doc->selection_end +=
                    accessibility_cell_bytes(snapshot, &cells[x]);
        }
        offset += accessibility_row_bytes(snapshot, y, end_col);
        if (y < last_row)
            offset += 1;
    }
    doc->length = offset;
    return true;
}

/* The line that holds byte `offset` of the history part. */
static size_t accessibility_history_line(const struct terminal_view *view,
                                         size_t offset)
{
    size_t low = 0, high = view->accessibility_line_count;
    while (low + 1 < high) {
        size_t mid = low + (high - low) / 2;
        if (view->accessibility_lines[mid].offset <= offset)
            low = mid;
        else
            high = mid;
    }
    return low;
}

static size_t accessibility_provider_length(void *instance)
{
    struct terminal *terminal = instance;
    if (!terminal || !terminal->session.term || !terminal->snapshot)
        return 0;
    wsap_vtcore_lock(terminal->session.term);
    struct accessibility_document doc = {0};
    bool measured = wsap_vtcore_snapshot_update(terminal->snapshot,
                                                terminal->session.term) &&
                    accessibility_document_measure(&terminal->view, terminal,
                                                    &doc);
    wsap_vtcore_unlock(terminal->session.term);
    return measured ? doc.length : 0;
}

static size_t accessibility_provider_read(void *instance, size_t offset,
                                          char *out, size_t capacity)
{
    struct terminal *terminal = instance;
    if (!terminal || !terminal->session.term || !terminal->snapshot || !out ||
        !capacity)
        return 0;
    struct terminal_view *view = &terminal->view;
    struct wsap_vtcore *term = terminal->session.term;
    const struct wsap_vtcore_snapshot *snapshot = terminal->snapshot;
    const struct wsap_vtcore_view *term_view = wsap_vtcore_snapshot_view(snapshot);
    size_t copied = 0;
    wsap_vtcore_lock(term);
    struct accessibility_document doc = {0};
    if (!wsap_vtcore_snapshot_update(terminal->snapshot, term) ||
        !accessibility_document_measure(view, terminal, &doc) ||
        offset >= doc.length)
        goto done;
    if (offset < doc.history_length) {
        if (!view->accessibility_line_count)
            goto done;
        size_t line = accessibility_history_line(view, offset);
        size_t within = offset - view->accessibility_lines[line].offset;
        size_t row = view->accessibility_lines[line].row;
        if (row >= view->accessibility_rows)
            goto done;
        char *history = wsap_vtcore_history_text(
            term, (int)(view->accessibility_rows - row));
        if (!history)
            goto done;
        const char *newline = strchr(history, '\n');
        size_t available = newline ? (size_t)(newline - history) + 1
                                   : strlen(history);
        if (within < available) {
            copied = available - within;
            if (copied > capacity)
                copied = capacity;
            memcpy(out, history + within, copied);
        }
        wsap_vtcore_text_free(history);
        goto done;
    }
    size_t base = doc.history_length;
    int last_row = accessibility_last_row(snapshot);
    int cols = term_view->size.x;
    for (int y = 0; y <= last_row; y++) {
        int end_col = accessibility_row_end_col(snapshot, y, cols);
        size_t bytes = accessibility_row_bytes(snapshot, y, end_col) +
                       (y < last_row ? 1 : 0);
        if (offset < base + bytes) {
            struct accessibility_text_builder builder = {0};
            const struct wsap_vtcore_cell *cells =
                wsap_vtcore_snapshot_row(snapshot, y);
            bool ok = true;
            for (int x = 0; cells && x < end_col && ok; x++)
                ok = accessibility_append_cell(&builder, snapshot, &cells[x]);
            if (ok && y < last_row)
                ok = accessibility_text_append(&builder, "\n", 1);
            if (ok) {
                size_t within = offset - base;
                copied = bytes - within;
                if (copied > capacity)
                    copied = capacity;
                memcpy(out, builder.text + within, copied);
            }
            free(builder.text);
            goto done;
        }
        base += bytes;
    }
done:
    wsap_vtcore_unlock(term);
    return copied;
}

static bool terminal_view_accessibility_text(struct terminal_view *view,
                                             struct terminal *terminal,
                                             struct ui_accessibility_node_spec *out)
{
    if (!terminal->snapshot || !terminal->session.term)
        return false;
    struct accessibility_document doc = {0};
    if (!wsap_vtcore_snapshot_update(terminal->snapshot, terminal->session.term) ||
        !accessibility_document_measure(view, terminal, &doc))
        return false;
    view->accessibility_provider = (struct ui_accessibility_text_provider){
        .instance = terminal,
        .length = accessibility_provider_length,
        .read = accessibility_provider_read,
    };
    out->text = &view->accessibility_provider;
    out->text_offset = 0;
    out->text_length = SIZE_MAX;
    out->text_direction = UI_ACCESSIBILITY_TEXT_DIRECTION_LEFT_TO_RIGHT;
    out->caret_offset = doc.caret_offset;
    out->selection_start = doc.selection_start;
    out->selection_end = doc.selection_end;
    return true;
}

static bool view_accessibility(const struct ui_widget *widget,
                               struct ui_accessibility_node_spec *out)
{
    struct terminal_view *view = view_of(widget);
    struct terminal *terminal = view_terminal(widget);
    struct wsap_vtcore *term = terminal->session.term;
    out->role = UI_ACCESSIBILITY_ROLE_TERMINAL;
    out->states |= UI_ACCESSIBILITY_STATE_READ_ONLY |
                   UI_ACCESSIBILITY_STATE_MULTILINE;
    if (!term || !terminal->snapshot || !terminal->window)
        return true;
    wsap_vtcore_lock(term);
    terminal_view_accessibility_text(view, terminal, out);
    wsap_vtcore_unlock(term);
    return true;
}

static uint64_t view_tick(struct ui_widget *widget, uint64_t now_ms)
{
    const struct terminal_view *view = view_of(widget);
    struct terminal *t = view_terminal(widget);
    uint64_t next = view->visual_bell_until_ms > now_ms
        ? view->visual_bell_until_ms : 0;
    uint64_t flip = view->blink_suppressed ? 0 : terminal_next_blink_ms(t, now_ms);
    if (flip && (!next || flip < next))
        next = flip;
    uint64_t text_flip = terminal_next_text_blink_ms(t, now_ms);
    if (text_flip && (!next || text_flip < next))
        next = text_flip;
    return next;
}

static void view_prepare(struct ui_widget *widget)
{
    struct terminal *t = view_terminal(widget);
    ui_scrollview_set_metrics(&t->scrollview, t->session.term
        ? terminal_scroll_metrics(t->session.term)
        : (struct ui_scroll_metrics){0});
}

/* Pastes the payload of a typed drop. The window keeps the type negotiation
 * and sends the type that it picked. An image saves to a file first, and the
 * paste holds its path. A uri list becomes one text line. Other types are
 * left to the framework. */
static bool paste_drop(struct terminal *t, const struct ui_drag_event *drag)
{
    if (!drag || !drag->data || !drag->len)
        return false;
    if (terminal_mime_is_image(drag->mime)) {
        char *path = terminal_save_dropped_image(drag->mime, drag->data,
                                                 drag->len);
        char *text = terminal_quote_path(path);
        free(path);
        if (text) {
            terminal_paste(t, text, strlen(text));
            free(text);
        }
        return true;
    }
    if (drag->mime && (!drag->mime[0] ||
                       !strcasecmp(drag->mime, "text/uri-list"))) {
        char *text = terminal_join_uri_lines(drag->data, drag->len);
        if (text) {
            terminal_paste(t, text, strlen(text));
            free(text);
        }
        return true;
    }
    return false;
}

/* Drops on any part of the terminal content arrive here. The view and the
 * scrollbar leave the event unhandled, so it bubbles to the content root. */
static bool view_drop_event(struct ui_widget *widget,
                            const struct ui_event *event, void *userdata)
{
    (void)widget;
    if (event->kind != UI_EVENT_DROP)
        return false;
    return paste_drop(userdata, &event->data.drag);
}

static const struct ui_widget_ops VIEW_OPS = {
    .name = "terminal-view",
    .prepare = view_prepare,
    .event = view_event,
    .cursor = view_cursor,
    .tick = view_tick,
    .paint = view_paint,
    .accessibility = view_accessibility,
    .key_release = true,
};

static bool scrollview_action(struct ui_widget *source,
                              const struct ui_action *action, void *userdata)
{
    (void)source;
    struct terminal *t = userdata;
    if (!t || action->kind != UI_ACTION_CHANGE || !t->session.term)
        return false;
    wsap_vtcore_scroll_to(t->session.term, action->index);
    terminal_show_scrollbar(t);
    return true;
}

void terminal_view_init(struct terminal *t)
{
    view_parts_init();
    t->view = (struct terminal_view){0};
    ui_widget_init(&t->view.widget, &VIEW_OPS, NULL);
    ui_widget_set_accessible_name(&t->view.widget, "Terminal");
    ui_widget_set_style_identity(&t->view.widget, "terminal-view", NULL,
                                 "terminal-view");
    ui_scrollview_init(&t->scrollview);
    ui_scrollview_set_overlay(&t->scrollview, true);
    ui_scrollview_set_metrics(&t->scrollview, (struct ui_scroll_metrics){0});
    ui_widget_add_child(ui_scrollview_widget(&t->scrollview), &t->view.widget);
}

void terminal_view_attach(struct terminal *t)
{
    ui_widget_set_action_handler(ui_scrollview_widget(&t->scrollview),
                                 scrollview_action, t);
    ui_widget_set_event_handler(&t->scrollview.widget, view_drop_event, t);
}

void terminal_view_drop_hover(struct terminal *t, bool over, void *ud)
{
    (void)ud;
    if (!t || t->view.drop_hover == over)
        return;
    t->view.drop_hover = over;
    changed(&t->view);
}

void terminal_view_set_preedit(struct terminal *t, const char *text,
                               int cursor_begin, int cursor_end,
                               struct rect2i caret, uint32_t fg, uint32_t bg)
{
    if (!t)
        return;
    struct terminal_preedit next = { .caret = caret, .fg = fg, .bg = bg };
    size_t len = text ? strlen(text) : 0;
    size_t off = 0;
    while (off < len) {
        uint32_t cp;
        int cells;
        int n = ui_text_run_next(text + off, len - off, &cp, &cells);
        if (n <= 0 || off + (size_t)n >= sizeof(next.text))
            break;
        off += (size_t)n;
    }
    if (off)
        memcpy(next.text, text, off);
    else
        next = (struct terminal_preedit){0};
    bool shown = off && cursor_begin >= 0;
    next.cursor_begin = shown ? (cursor_begin < (int)off ? cursor_begin : (int)off) : -1;
    next.cursor_end = shown ? (cursor_end < (int)off ? cursor_end : (int)off) : -1;
    const struct terminal_preedit *cur = &t->view.preedit;
    if (!strcmp(next.text, cur->text) && rect2i_eq(next.caret, cur->caret) &&
        next.cursor_begin == cur->cursor_begin &&
        next.cursor_end == cur->cursor_end &&
        next.fg == cur->fg && next.bg == cur->bg)
        return;
    t->view.preedit = next;
    changed(&t->view);
}

struct rect2i terminal_view_rect(const struct terminal *t)
{
    return t ? t->view.widget.bounds : (struct rect2i){0};
}

struct vector2i terminal_cell_at(const struct terminal *t, struct vector2i pos)
{
    if (!t)
        return (struct vector2i){ .x = -1, .y = -1 };
    return cell_at(t, ui_widget_to_local(&t->view.widget, pos));
}
