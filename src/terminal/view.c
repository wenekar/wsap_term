#include "terminal/internal.h"

#include "input/input.h"
#include "platform/platform.h"
#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"
#include <wsap_vtcore/unicode/grapheme.h>
#include <wsap_vtcore/unicode/utf8.h>

#include <stdlib.h>
#include <string.h>

static struct terminal *view_terminal(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct terminal,
                           view.widget);
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

static struct vector2i cell_at(const struct tab *t, struct vector2i local)
{
    const struct font_metrics *metrics = tab_metrics(t);
    return (struct vector2i){
        .x = metrics->cell_w > 0 && local.x >= 0 ? local.x / metrics->cell_w : -1,
        .y = metrics->cell_h > 0 && local.y >= 0 ? local.y / metrics->cell_h : -1,
    };
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

static bool report_mouse(struct window *w, struct tab *t,
                         const struct ui_pointer_event *p, struct vector2i local)
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
    struct wsap_vtcore_modes modes = terminal_input_modes(w);
    char buf[64];
    struct wsap_vtcore_input_result r = wsap_vtcore_input_encode_mouse(&mev, &modes, buf, sizeof(buf));
    if (r.len)
        session_write_bytes(&t->session, buf, r.len);
    return r.status != WSAP_VTCORE_INPUT_UNHANDLED;
}

static void scroll_lines(struct window *w, struct tab *t, int lines)
{
    struct wsap_vtcore_modes modes_now = wsap_vtcore_modes(t->session.term);
    if (modes_now.alternate && modes_now.mouse == WSAP_VTCORE_MOUSE_OFF) {
        struct wsap_vtcore_modes modes = terminal_input_modes(w);
        struct wsap_vtcore_key_event key = {
            .key = lines > 0 ? WSAP_VTCORE_KEY_UP : WSAP_VTCORE_KEY_DOWN,
            .action = WSAP_VTCORE_KEY_PRESS,
        };
        char buf[64];
        struct wsap_vtcore_input_result r =
            wsap_vtcore_input_encode_key((const struct wsap_vtcore_keyboard_state *)window_keyboard(w), &key, &modes, buf, sizeof(buf));
        for (int i = 0; i < (lines > 0 ? lines : -lines); i++)
            session_write_bytes(&t->session, buf, r.len);
        return;
    }
    wsap_vtcore_scroll(t->session.term, lines);
    selection_extend_to_pointer(&t->selection, t->session.term);
    terminal_show_scrollbar(w);
}

static void select_text(struct window *w, struct tab *t,
                        const struct ui_pointer_event *p, struct vector2i local,
                        uint64_t now_ms)
{
    const struct font_metrics *m = tab_metrics(t);
    bool right_half = local.x >= 0 && m->cell_w > 0 &&
                      local.x % m->cell_w >= m->cell_w / 2;
    struct vector2i cell = cell_at(t, local);
    t->selection.double_click_ms = window_double_click_ms(w);
    selection_handle_pointer(&t->selection, t->session.term, cell,
                             right_half, pointer_kind(p->kind), now_ms);
    if (p->kind == UI_POINTER_RELEASE && wsap_vtcore_has_selection(t->session.term)) {
        char *text = wsap_vtcore_selection_text(t->session.term);
        if (text && window_surface(w))
            window_copy(w, YASL_PRIMARY, text);
        wsap_vtcore_text_free(text);
    }
}

static void secondary_press(struct window *w, struct tab *t,
                            const struct ui_pointer_event *p,
                            struct vector2i local)
{
    if (terminal_pointer_selects_text(t->session.term))
        terminal_open_ctx_menu(w, p->pos);
    else if (!t->selection.pressed)
        report_mouse(w, t, p, local);
}

static bool pointer_is_local(struct window *w, const struct tab *t)
{
    return (window_modifiers(w) & INPUT_MOD_SHIFT) ||
           terminal_pointer_selects_text(t->session.term);
}

static bool view_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct terminal_view *view = view_of(widget);
    struct terminal *tw = view_terminal(widget);
    struct window *w = tw->window;
    struct tab *t = terminal_active_tab(w);
    if (event->kind == UI_EVENT_CAPTURE_LOST) {
        bool opening_link = view->opening_link;
        view->opening_link = false;
        if (!opening_link && t)
            t->selection.pressed = false;
        return true;
    }
    if (event->kind == UI_EVENT_KEY) {
        const struct input_key_event *key = event->data.key.native;
        if (!key || !terminal_key(w, key))
            ui_surface_keep_key_text(ui_widget_surface(widget));
        return true;
    }
    if (event->kind == UI_EVENT_TEXT) {
        terminal_send_input(w, event->data.text.data, event->data.text.len);
        return true;
    }
    if (event->kind != UI_EVENT_POINTER || !t || !t->session.term)
        return false;
    const struct ui_pointer_event *p = &event->data.pointer;
    struct vector2i local = ui_widget_to_local(widget, p->pos);
    if (p->kind == UI_POINTER_LEAVE) {
        view->hover_link = false;
        return false;
    }
    if (p->kind == UI_POINTER_PRESS && p->button == 2) {
        secondary_press(w, t, p, local);
        return true;
    }

    char *url = wsap_vtcore_link_at(t->session.term, term_vec_from_geom(cell_at(t, local)));
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
        if (!t->selection.pressed && report_mouse(w, t, &step, local))
            return true;
        scroll_lines(w, t, lines);
        changed(view);
        return true;
    }
    if (!t->selection.pressed && report_mouse(w, t, p, local))
        return true;
    if (p->button == 0 && pointer_is_local(w, t)) {
        if (p->kind == UI_POINTER_PRESS)
            ui_widget_capture_pointer(widget);
        else if (p->kind == UI_POINTER_RELEASE)
            ui_widget_release_pointer(widget);
        select_text(w, t, p, local, event->time_ms);
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
    struct window *w = view_terminal(widget)->window;
    struct tab *t = terminal_active_tab(w);
    if (!t)
        return UI_CURSOR_DEFAULT;
    if (t->selection.pressed)
        return UI_CURSOR_TEXT;
    if (!pointer_is_local(w, t))
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
    struct rect2i box = { .size = widget->bounds.size };
    ui_paint_rect(paint, box, st.has_bg_color ? st.bg_color : 0x3389B4FA);
    int width = ui_css_resolve_int(st.border_width, 0, 2,
                                   &widget->style_scale, true);
    if (width <= 0)
        return;
    uint32_t color = st.has_border_color ? st.border_color : 0xFF89B4FA;
    struct rect2i edges[] = {
        { .x = 0, .y = 0, .width = box.width, .height = width },
        { .x = 0, .y = box.height - width, .width = box.width, .height = width },
        { .x = 0, .y = 0, .width = width, .height = box.height },
        { .x = box.width - width, .y = 0, .width = width, .height = box.height },
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
        ui_paint_rect(paint, edges[i], color);
}

static void view_paint(const struct ui_widget *widget,
                       struct ui_paint_context *paint)
{
    struct terminal_view *view = view_of(widget);
    if (view->drop_hover)
        paint_drop_hover(widget, paint);
    if (view->visual_bell_until_ms > yasl_time_ns() / 1000000)
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = 0, .y = 0 },
            .size = widget->bounds.size,
        }, 0x40FFFFFF);
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

static bool terminal_view_accessibility_text(struct terminal_view *view,
                                             struct terminal *terminal,
                                             struct wsap_vtcore *term)
{
    if (!terminal->snapshot || !term)
        return false;
    const struct wsap_vtcore_snapshot *snapshot = terminal->snapshot;
    const struct wsap_vtcore_view *term_view = wsap_vtcore_snapshot_view(snapshot);
    if (view->accessibility_text_valid && view->accessibility_term == term &&
        view->accessibility_version == term_view->version)
        return true;

    int rows = term_view->size.y;
    int cols = term_view->size.x;
    int last_row = -1;
    for (int y = rows - 1; y >= 0; y--) {
        struct wsap_vtcore_span selection = wsap_vtcore_snapshot_row_selection(snapshot, y);
        if (selection.end > selection.begin) {
            last_row = y;
            break;
        }
        const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
        for (int x = cols - 1; cells && x >= 0; x--) {
            if (!accessibility_cell_blank(snapshot, &cells[x])) {
                last_row = y;
                break;
            }
        }
        if (last_row >= 0)
            break;
    }
    if (term_view->cursor.visible && term_view->cursor.pos.y >= 0 &&
        term_view->cursor.pos.y < rows && term_view->cursor.pos.y > last_row)
        last_row = term_view->cursor.pos.y;

    struct accessibility_text_builder builder = {0};
    size_t caret_offset = 0;
    size_t selection_start = 0;
    size_t selection_end = 0;
    bool has_selection = false;
    int cursor_y = term_view->cursor.pos.y;
    int cursor_x = term_view->cursor.pos.x;

    char *history = wsap_vtcore_history_text(term, 500);
    if (!history)
        return false;
    bool appended = accessibility_text_append(&builder, history, strlen(history));
    wsap_vtcore_text_free(history);
    if (!appended) {
        free(builder.text);
        return false;
    }

    for (int y = 0; y <= last_row; y++) {
        const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
        int last_col = -1;
        for (int x = cols - 1; cells && x >= 0; x--) {
            if (!accessibility_cell_blank(snapshot, &cells[x])) {
                last_col = x;
                break;
            }
        }
        struct wsap_vtcore_span selection = wsap_vtcore_snapshot_row_selection(snapshot, y);
        int end_col = last_col + 1;
        if (selection.end > end_col)
            end_col = selection.end;
        if (end_col < 0)
            end_col = 0;
        if (end_col > cols)
            end_col = cols;

        int selection_begin = selection.begin;
        if (selection_begin < 0) selection_begin = 0;
        if (selection_begin > end_col) selection_begin = end_col;
        int selection_limit = selection.end;
        if (selection_limit < selection_begin)
            selection_limit = selection_begin;
        if (selection_limit > end_col)
            selection_limit = end_col;

        for (int x = 0; x <= end_col; x++) {
            int caret_col = cursor_x < 0 ? 0 : cursor_x > end_col
                ? end_col : cursor_x;
            if (term_view->cursor.visible && y == cursor_y && x == caret_col)
                caret_offset = builder.len;
            if (selection.end > selection.begin && x == selection_begin) {
                if (!has_selection)
                    selection_start = builder.len;
                has_selection = true;
            }
            if (selection.end > selection.begin && x == selection_limit)
                selection_end = builder.len;
            if (x == end_col)
                break;
            if (!cells || !accessibility_append_cell(&builder, snapshot,
                                                       &cells[x])) {
                free(builder.text);
                return false;
            }
        }
        if (y < last_row &&
            !accessibility_text_append(&builder, "\n", 1)) {
            free(builder.text);
            return false;
        }
    }

    if (!builder.text) {
        builder.text = calloc(1, 1);
        if (!builder.text)
            return false;
        builder.capacity = 1;
    }
    free(view->accessibility_text);
    view->accessibility_text = builder.text;
    view->accessibility_caret_offset = caret_offset;
    view->accessibility_selection_start = has_selection ? selection_start : 0;
    view->accessibility_selection_end = has_selection ? selection_end : 0;
    view->accessibility_version = term_view->version;
    view->accessibility_term = term;
    view->accessibility_text_valid = true;
    return true;
}

static bool view_accessibility(const struct ui_widget *widget,
                               struct ui_accessibility_node_spec *out)
{
    struct terminal_view *view = view_of(widget);
    struct terminal *terminal = view_terminal(widget);
    struct tab *tab = terminal_active_tab(terminal->window);
    struct wsap_vtcore *term = tab ? tab->session.term : NULL;
    out->role = UI_ACCESSIBILITY_ROLE_TERMINAL;
    out->states |= UI_ACCESSIBILITY_STATE_READ_ONLY |
                   UI_ACCESSIBILITY_STATE_MULTILINE;
    if (!term || !terminal->snapshot)
        return true;
    wsap_vtcore_lock(term);
    bool captured = wsap_vtcore_snapshot_update(terminal->snapshot, term) &&
        terminal_view_accessibility_text(view, terminal, term);
    wsap_vtcore_unlock(term);
    if (!captured)
        return true;
    out->value = view->accessibility_text;
    out->caret_offset = view->accessibility_caret_offset;
    out->selection_start = view->accessibility_selection_start;
    out->selection_end = view->accessibility_selection_end;
    return true;
}

static uint64_t view_tick(struct ui_widget *widget, uint64_t now_ms)
{
    const struct terminal_view *view = view_of(widget);
    uint64_t next = view->visual_bell_until_ms > now_ms ? view->visual_bell_until_ms : 0;
    uint64_t flip = terminal_next_blink_ms(view_terminal(widget)->window, now_ms);
    if (flip && (!next || flip < next))
        next = flip;
    return next;
}

static const struct ui_widget_ops VIEW_OPS = {
    .name = "terminal-view",
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
    struct window *w = userdata;
    struct tab *t = terminal_active_tab(w);
    if (action->kind != UI_ACTION_CHANGE || !t)
        return false;
    wsap_vtcore_scroll_to(t->session.term, action->index);
    terminal_show_scrollbar(w);
    return true;
}

void terminal_view_init(struct terminal *tw)
{
    view_parts_init();
    tw->view = (struct terminal_view){0};
    ui_widget_init(&tw->view.widget, &VIEW_OPS, NULL);
    ui_widget_set_accessible_name(&tw->view.widget, "Terminal");
    ui_widget_set_style_identity(&tw->view.widget, "terminal-view", NULL,
                                 "terminal-view");
    ui_scrollview_init(&tw->scrollview);
    ui_scrollview_set_overlay(&tw->scrollview, true);
    ui_scrollview_set_metrics(&tw->scrollview, (struct ui_scroll_metrics){0});
    ui_widget_add_child(ui_scrollview_widget(&tw->scrollview), &tw->view.widget);
}

void terminal_view_attach(struct window *w)
{
    struct terminal *tw = terminal_of(w);
    ui_widget_set_action_handler(ui_scrollview_widget(&tw->scrollview),
                                 scrollview_action, w);
}

void terminal_view_drop_hover(struct window *w, bool over, void *ud)
{
    (void)ud;
    struct terminal_view *view = &terminal_of(w)->view;
    if (view->drop_hover == over)
        return;
    view->drop_hover = over;
    changed(view);
}

void terminal_view_set_preedit(struct terminal *tw, const char *text,
                               int cursor_begin, int cursor_end,
                               struct rect2i caret, uint32_t fg, uint32_t bg)
{
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
    const struct terminal_preedit *cur = &tw->view.preedit;
    if (!strcmp(next.text, cur->text) && rect2i_eq(next.caret, cur->caret) &&
        next.cursor_begin == cur->cursor_begin &&
        next.cursor_end == cur->cursor_end &&
        next.fg == cur->fg && next.bg == cur->bg)
        return;
    tw->view.preedit = next;
    changed(&tw->view);
}

struct rect2i terminal_view_rect(const struct window *w)
{
    return terminal_of(w)->view.widget.bounds;
}

struct vector2i terminal_cell_at(const struct window *w, struct vector2i pos)
{
    return cell_at(terminal_active_tab(w),
                   ui_widget_to_local(&terminal_of(w)->view.widget, pos));
}
