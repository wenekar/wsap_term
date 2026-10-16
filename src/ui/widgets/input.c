#include "ui/widgets/input.h"

#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/text_edit.h"
#include "ui/surface.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <string.h>
#include <stdlib.h>

#define INPUT_FG_FALLBACK    0xFFCDD6F4u
#define INPUT_DIM_FALLBACK   0xFF7F849Cu
#define INPUT_SEL_FALLBACK   0xFF45475Au
#define INPUT_CARET_FALLBACK 0xFFF5E0DCu

static struct ui_input *input_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_input, widget);
}

static const struct css_scale_context *input_scale(const struct ui_input *in)
{
    return &in->widget.style_scale;
}

static int cell_w(const struct ui_input *in)
{
    return ui_widget_text_style(input_scale(in)).advance;
}

static int cell_h(const struct ui_input *in)
{
    return ui_widget_text_style(input_scale(in)).cell_h;
}

static int border_width(const struct ui_input *in)
{
    const struct css_style *s = &in->widget.style;
    return s->has_border_color
        ? ui_css_resolve_int(s->border_width, 0, 0, input_scale(in), true) : 0;
}

static struct rect2i content_rect(const struct ui_input *in)
{
    struct rect2i r = rect2i_apply_insets(
        (struct rect2i){ .size = in->widget.bounds.size },
        ui_ctx_insets(input_scale(in), in->widget.style.padding, 0));
    return rect2i_inset(r, border_width(in));
}

int ui_input_text_inset(const struct ui_input *in)
{
    return in ? border_width(in) +
                ui_ctx_inset(input_scale(in), in->widget.style.padding.left, 0)
              : 0;
}

static int cells_to(const struct ui_input *in, int pos)
{
    return ui_text_run_cells(in->impl.buf, (size_t)pos);
}

static void sel_range(const struct ui_input *in, int *lo, int *hi)
{
    *lo = in->impl.caret < in->impl.anchor ? in->impl.caret : in->impl.anchor;
    *hi = in->impl.caret < in->impl.anchor ? in->impl.anchor : in->impl.caret;
}

struct ui_input_history {
    struct ui_input_history *next;
    int len, caret, anchor;
    char text[];
};

static void history_free(struct ui_input_history **stack)
{
    while (*stack) {
        struct ui_input_history *entry = *stack;
        *stack = entry->next;
        free(entry);
    }
}

static struct ui_input_history *history_snapshot(const struct ui_input *in)
{
    struct ui_input_history *entry = malloc(sizeof(*entry) + (size_t)in->impl.len + 1);
    if (!entry) return NULL;
    *entry = (struct ui_input_history){ .len = in->impl.len,
        .caret = in->impl.caret, .anchor = in->impl.anchor };
    memcpy(entry->text, in->impl.buf, (size_t)in->impl.len + 1);
    return entry;
}

static void history_record(struct ui_input *in, int group)
{
    if (in->impl.editing) return;
    history_free(&in->impl.redo);
    if (!group || in->impl.edit_group != group || in->impl.caret != in->impl.edit_caret ||
        in->impl.caret != in->impl.anchor) {
        struct ui_input_history *entry = history_snapshot(in);
        if (entry) {
            entry->next = in->impl.undo;
            in->impl.undo = entry;
            in->impl.undo_count++;
            if (in->impl.undo_count > 128) {
                struct ui_input_history *last = in->impl.undo;
                for (int i = 1; i < 128; i++) last = last->next;
                history_free(&last->next);
                in->impl.undo_count = 128;
            }
        }
    }
    in->impl.edit_group = group;
}

static void update_scroll(struct ui_input *in)
{
    if (!in->widget.bounds.width)
        return;
    int cap = content_rect(in).width / cell_w(in);
    if (cap < 1)
        cap = 1;
    int max_scroll = cells_to(in, in->impl.len) - cap + 1;
    if (max_scroll < 0)
        max_scroll = 0;
    if (in->impl.scroll_cells > max_scroll)
        in->impl.scroll_cells = max_scroll;
    int caret = cells_to(in, in->impl.caret);
    if (caret - in->impl.scroll_cells >= cap)
        in->impl.scroll_cells = caret - cap + 1;
    if (caret < in->impl.scroll_cells)
        in->impl.scroll_cells = caret;
    if (in->impl.scroll_cells < 0)
        in->impl.scroll_cells = 0;
}

static void caret_moved(struct ui_input *in)
{
    in->impl.blink_on = true;
    in->impl.blink_at = 0;
    update_scroll(in);
    ui_widget_invalidate(&in->widget, UI_WIDGET_INVALIDATE_PAINT);
}

static void changed(struct ui_input *in)
{
    in->impl.edit_caret = in->impl.caret;
    caret_moved(in);
    ui_widget_notify(&in->widget, (struct ui_action){
        .kind = UI_ACTION_CHANGE, .id = in->impl.action_id, .index = -1,
    });
}

static void delete_range(struct ui_input *in, int lo, int hi)
{
    if (lo < 0)
        lo = 0;
    if (hi > in->impl.len)
        hi = in->impl.len;
    if (lo >= hi)
        return;
    history_record(in, 0);
    memmove(in->impl.buf + lo, in->impl.buf + hi, (size_t)(in->impl.len - hi));
    in->impl.len -= hi - lo;
    in->impl.buf[in->impl.len] = '\0';
    in->impl.caret = in->impl.anchor = lo;
    changed(in);
}

static void delete_selection(struct ui_input *in)
{
    int lo, hi;
    sel_range(in, &lo, &hi);
    if (lo != hi)
        delete_range(in, lo, hi);
}

static int clean_text(char *out, size_t cap, const char *utf8)
{
    char tmp[UI_INPUT_CAP];
    wsap_vtcore_utf8_sanitize(tmp, sizeof(tmp), utf8 ? utf8 : "");
    int w = 0;
    for (int r = 0; tmp[r] && w < (int)cap - 1; r++) {
        unsigned char c = (unsigned char)tmp[r];
        if (c >= 0x20 && c != 0x7f)
            out[w++] = (char)c;
    }
    out[w] = '\0';
    return w;
}

static void insert_bytes(struct ui_input *in, const char *data, int n)
{
    int room = UI_INPUT_CAP - 1 - in->impl.len;
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)data[n] & 0xC0) == 0x80)
            n--;
    }
    if (n <= 0)
        return;
    history_record(in, 1);
    memmove(in->impl.buf + in->impl.caret + n, in->impl.buf + in->impl.caret,
            (size_t)(in->impl.len - in->impl.caret));
    memcpy(in->impl.buf + in->impl.caret, data, (size_t)n);
    in->impl.len += n;
    in->impl.buf[in->impl.len] = '\0';
    in->impl.caret += n;
    in->impl.anchor = in->impl.caret;
    changed(in);
}

static void caret_move_to(struct ui_input *in, int pos, bool extend)
{
    in->impl.edit_group = 0;
    if (pos < 0)
        pos = 0;
    if (pos > in->impl.len)
        pos = in->impl.len;
    in->impl.caret = pos;
    if (!extend)
        in->impl.anchor = pos;
    caret_moved(in);
}

const char *ui_input_text(const struct ui_input *in)
{
    return in ? in->impl.buf : "";
}

int ui_input_caret(const struct ui_input *in)
{
    return in ? in->impl.caret : 0;
}

bool ui_input_selection(const struct ui_input *in, int *start, int *end)
{
    if (!in || in->impl.caret == in->impl.anchor)
        return false;
    int lo, hi;
    sel_range(in, &lo, &hi);
    if (start)
        *start = lo;
    if (end)
        *end = hi;
    return true;
}

static struct {
    ui_part_id text;
    ui_part_id empty;
    ui_part_id selection;
    ui_part_id caret;
    ui_part_id item;
} input_parts;

static void input_parts_init(void)
{
    if (input_parts.text)
        return;
    input_parts.text = ui_part_intern("text-input-text");
    input_parts.empty = ui_part_intern("text-input-text empty");
    input_parts.selection = ui_part_intern("text-input-selection");
    input_parts.caret = ui_part_intern("text-input-caret");
    input_parts.item = ui_part_intern("menu-item");
}

static const struct ui_widget_ops INPUT_OPS;

static void input_ensure_init(struct ui_input *in)
{
    if (!in || in->widget.ops)
        return;
    in->impl.blink_on = true;
    input_parts_init();
    ui_widget_init(&in->widget, &INPUT_OPS, NULL);
    ui_widget_set_style_identity(&in->widget, "text-input", NULL, "input");
    ui_widget_set_focusable(&in->widget, true);
}

void ui_input_set_text(struct ui_input *in, const char *utf8)
{
    if (!in)
        return;
    input_ensure_init(in);
    history_free(&in->impl.undo);
    history_free(&in->impl.redo);
    in->impl.undo_count = in->impl.edit_group = 0;
    in->impl.len = clean_text(in->impl.buf, sizeof(in->impl.buf), utf8);
    in->impl.caret = in->impl.anchor = in->impl.len;
    changed(in);
}

void ui_input_set_placeholder(struct ui_input *in, const char *utf8)
{
    if (!in)
        return;
    input_ensure_init(in);
    wsap_vtcore_utf8_sanitize(in->impl.placeholder, sizeof(in->impl.placeholder),
                  utf8 ? utf8 : "");
    ui_widget_invalidate(&in->widget, UI_WIDGET_INVALIDATE_PAINT);
}

void ui_input_set_action_id(struct ui_input *in, int id)
{
    if (!in)
        return;
    input_ensure_init(in);
    in->impl.action_id = id;
}

void ui_input_select_all(struct ui_input *in)
{
    if (!in)
        return;
    input_ensure_init(in);
    in->impl.anchor = 0;
    in->impl.caret = in->impl.len;
    caret_moved(in);
}

void ui_input_clear(struct ui_input *in)
{
    if (!in)
        return;
    input_ensure_init(in);
    history_record(in, 0);
    in->impl.len = 0;
    in->impl.buf[0] = '\0';
    in->impl.caret = in->impl.anchor = 0;
    in->impl.scroll_cells = 0;
    changed(in);
}

bool ui_input_insert_text(struct ui_input *in, const char *utf8)
{
    if (!in || !utf8)
        return false;
    input_ensure_init(in);
    char clean[UI_INPUT_CAP];
    int n = clean_text(clean, sizeof(clean), utf8);
    if (!n && in->impl.caret == in->impl.anchor)
        return false;
    int room = UI_INPUT_CAP - 1 - in->impl.len + abs(in->impl.caret - in->impl.anchor);
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)clean[n] & 0xc0) == 0x80) n--;
    }
    if (!n && in->impl.caret == in->impl.anchor) return false;
    history_record(in, n == 1 ? 1 : 0);
    in->impl.editing = true;
    int before = in->impl.len;
    delete_selection(in);
    insert_bytes(in, clean, n);
    in->impl.editing = false;
    return n > 0 || in->impl.len != before;
}

static int edit_back(const struct ui_input *in, enum ui_edit_unit unit)
{
    return ui_text_edit_move(in->impl.buf, in->impl.len, in->impl.caret, unit, true);
}

static int edit_forward(const struct ui_input *in, enum ui_edit_unit unit)
{
    return ui_text_edit_move(in->impl.buf, in->impl.len, in->impl.caret, unit, false);
}

bool ui_input_can_command(const struct ui_input *in, enum ui_edit_command command)
{
    if (!in) return false;
    struct ui_surface *surface = ui_widget_surface(&in->widget);
    switch (command) {
    case UI_EDIT_UNDO: return in->impl.undo != NULL;
    case UI_EDIT_REDO: return in->impl.redo != NULL;
    case UI_EDIT_COPY:
    case UI_EDIT_CUT: return in->impl.caret != in->impl.anchor && surface && surface->impl.host.copy_text;
    case UI_EDIT_PASTE: return surface && surface->impl.host.paste_text;
    case UI_EDIT_SELECT_ALL: return in->impl.len > 0;
    default: return true;
    }
}

static bool history_restore(struct ui_input *in, bool redo)
{
    struct ui_input_history **from = redo ? &in->impl.redo : &in->impl.undo;
    struct ui_input_history **to = redo ? &in->impl.undo : &in->impl.redo;
    if (!*from) return true;
    struct ui_input_history *current = history_snapshot(in);
    if (!current) return true;
    current->next = *to;
    *to = current;
    struct ui_input_history *entry = *from;
    *from = entry->next;
    in->impl.len = entry->len;
    in->impl.caret = entry->caret;
    in->impl.anchor = entry->anchor;
    memcpy(in->impl.buf, entry->text, (size_t)entry->len + 1);
    free(entry);
    in->impl.undo_count += redo ? 1 : -1;
    in->impl.edit_group = 0;
    changed(in);
    return true;
}

bool ui_input_command(struct ui_input *in, enum ui_edit_command command)
{
    if (!in) return false;
    input_ensure_init(in);
    struct ui_surface *surface = ui_widget_surface(&in->widget);
    int lo, hi;
    sel_range(in, &lo, &hi);
    switch (command) {
    case UI_EDIT_UNDO: return history_restore(in, false);
    case UI_EDIT_REDO: return history_restore(in, true);
    case UI_EDIT_SELECT_ALL: ui_input_select_all(in); return true;
    case UI_EDIT_COPY:
    case UI_EDIT_CUT: {
        if (lo == hi) return true;
        char text[UI_INPUT_CAP];
        memcpy(text, in->impl.buf + lo, (size_t)(hi - lo));
        text[hi - lo] = '\0';
        if (ui_surface_copy_text(surface, text) && command == UI_EDIT_CUT)
            delete_range(in, lo, hi);
        return true;
    }
    case UI_EDIT_PASTE: ui_surface_paste_text(surface); return true;
    case UI_EDIT_YANK: ui_input_insert_text(in, in->impl.kill); return true;
    case UI_EDIT_DELETE_BACK:
    case UI_EDIT_DELETE_FORWARD:
    case UI_EDIT_KILL_START:
    case UI_EDIT_KILL_END:
    case UI_EDIT_KILL_WORD:
        if (lo == hi) {
            if (command == UI_EDIT_DELETE_BACK) lo = edit_back(in, UI_EDIT_CHARACTER);
            if (command == UI_EDIT_DELETE_FORWARD) hi = edit_forward(in, UI_EDIT_CHARACTER);
            if (command == UI_EDIT_KILL_START) lo = 0;
            if (command == UI_EDIT_KILL_END) hi = in->impl.len;
            if (command == UI_EDIT_KILL_WORD) lo = edit_back(in, UI_EDIT_WORD_END);
        }
        if (command == UI_EDIT_KILL_START || command == UI_EDIT_KILL_END || command == UI_EDIT_KILL_WORD) {
            memcpy(in->impl.kill, in->impl.buf + lo, (size_t)(hi - lo));
            in->impl.kill[hi - lo] = '\0';
        }
        delete_range(in, lo, hi);
        return true;
    case UI_EDIT_TRANSPOSE: {
        if (lo != hi || in->impl.len == 0 || in->impl.caret == 0) return true;
        int end = in->impl.caret == in->impl.len ? in->impl.len : edit_forward(in, UI_EDIT_CHARACTER);
        int middle = ui_text_edit_move(in->impl.buf, in->impl.len, end, UI_EDIT_CHARACTER, true);
        int start = ui_text_edit_move(in->impl.buf, in->impl.len, middle, UI_EDIT_CHARACTER, true);
        if (start == middle) return true;
        history_record(in, 0);
        char text[UI_INPUT_CAP];
        int first = middle - start, second = end - middle;
        memcpy(text, in->impl.buf + start, (size_t)first);
        memmove(in->impl.buf + start, in->impl.buf + middle, (size_t)second);
        memcpy(in->impl.buf + start + second, text, (size_t)first);
        in->impl.caret = in->impl.anchor = end;
        changed(in);
        return true;
    }
    default: return false;
    }
}

static bool handle_key(struct ui_input *in, const struct ui_key_event *ev)
{
    bool shift = (ev->mods & UI_MOD_SHIFT) != 0;
    if (ev->edit_command != UI_EDIT_NONE) {
        switch (ev->edit_command) {
        case UI_EDIT_START: caret_move_to(in, 0, shift); return true;
        case UI_EDIT_END: caret_move_to(in, in->impl.len, shift); return true;
        case UI_EDIT_LEFT: caret_move_to(in, edit_back(in, UI_EDIT_CHARACTER), shift); return true;
        case UI_EDIT_RIGHT: caret_move_to(in, edit_forward(in, UI_EDIT_CHARACTER), shift); return true;
        default: return ui_input_command(in, ev->edit_command);
        }
    }
    bool ctrl = (ev->mods & (UI_MOD_CTRL | UI_MOD_SUPER)) != 0;
    int lo, hi;
    switch (ev->key) {
    case UI_KEY_LEFT:
        if (!shift && ev->edit_unit == UI_EDIT_CHARACTER && in->impl.caret != in->impl.anchor)
            caret_move_to(in, in->impl.caret < in->impl.anchor ? in->impl.caret : in->impl.anchor, false);
        else
            caret_move_to(in, edit_back(in, ev->edit_unit), shift);
        return true;
    case UI_KEY_RIGHT:
        if (!shift && ev->edit_unit == UI_EDIT_CHARACTER && in->impl.caret != in->impl.anchor)
            caret_move_to(in, in->impl.caret > in->impl.anchor ? in->impl.caret : in->impl.anchor, false);
        else
            caret_move_to(in, edit_forward(in, ev->edit_unit), shift);
        return true;
    case UI_KEY_HOME:
        caret_move_to(in, 0, shift);
        return true;
    case UI_KEY_END:
        caret_move_to(in, in->impl.len, shift);
        return true;
    case UI_KEY_BACKSPACE:
        sel_range(in, &lo, &hi);
        if (lo != hi)
            delete_range(in, lo, hi);
        else if (in->impl.caret > 0)
            delete_range(in, edit_back(in, ev->edit_unit),
                         in->impl.caret);
        return true;
    case UI_KEY_DELETE:
        sel_range(in, &lo, &hi);
        if (lo != hi)
            delete_range(in, lo, hi);
        else if (in->impl.caret < in->impl.len)
            delete_range(in, in->impl.caret,
                         edit_forward(in, ev->edit_unit));
        return true;
    case UI_KEY_CHAR: {
        if (ctrl) {
            if (ev->text != 'a' && ev->text != 'A')
                return false;
            ui_input_select_all(in);
            return true;
        }
        if (ev->text_deferred || (ev->mods & UI_MOD_ALT))
            return false;
        if (ev->text < 0x20 || ev->text == 0x7f)
            return false;
        char enc[WSAP_VTCORE_UTF8_MAX_BYTES + 1];
        int n = wsap_vtcore_utf8_encode(ev->text, enc);
        if (n <= 0)
            return false;
        enc[n] = '\0';
        return ui_input_insert_text(in, enc);
    }
    default:
        return false;
    }
}

static int offset_at_x(const struct ui_input *in, int x)
{
    int cw = cell_w(in);
    int target = in->impl.scroll_cells +
                 (x - ui_input_text_inset(in) + cw / 2) / cw;
    int cells = 0, pos = 0;
    while (pos < in->impl.len) {
        uint32_t cp;
        int w;
        int n = ui_text_run_next(in->impl.buf + pos,
                                 (size_t)(in->impl.len - pos), &cp, &w);
        if (cells + w > target)
            break;
        cells += w;
        pos += n;
    }
    return pos;
}

static bool handle_pointer(struct ui_input *in, const struct ui_pointer_event *p,
                           uint64_t time_ms)
{
    struct ui_widget *widget = &in->widget;
    int x = ui_widget_to_local(widget, p->pos).x;
    switch (p->kind) {
    case UI_POINTER_PRESS:
        if (!rect2i_contains(widget->bounds, p->pos))
            return false;
        if (p->button == 2) {
            ui_widget_focus(widget);
            ui_widget_notify(widget, (struct ui_action){
                .kind = UI_ACTION_MENU, .index = -1, .pos = p->pos,
            });
            return true;
        }
        if (p->button != 0)
            return false;
        ui_widget_focus(widget);
        ui_widget_capture_pointer(widget);
        struct vector2i delta = vector2i_sub(p->pos, in->impl.click_pos);
        int distance = ui_px(widget->style_scale.dpi_scale, 4);
        uint32_t interval = ui_surface_double_click_interval_ms(
            ui_widget_surface(widget));
        bool repeated = in->impl.clicks && time_ms >= in->impl.click_at &&
            interval && time_ms - in->impl.click_at <= interval &&
            llabs((long long)delta.x) <= distance &&
            llabs((long long)delta.y) <= distance;
        in->impl.clicks = repeated ? 2 : 1;
        in->impl.click_at = time_ms;
        in->impl.click_pos = p->pos;
        in->impl.press_pos = p->pos;
        in->impl.dragging = true;
        if (repeated)
            ui_input_select_all(in);
        else
            caret_move_to(in, offset_at_x(in, x), false);
        return true;
    case UI_POINTER_MOTION:
        if (!in->impl.dragging)
            return false;
        if (in->impl.clicks > 1)
            return true;
        if (in->impl.clicks == 1) {
            struct vector2i drag = vector2i_sub(p->pos, in->impl.press_pos);
            int distance = ui_px(widget->style_scale.dpi_scale, 4);
            if (llabs((long long)drag.x) > distance ||
                llabs((long long)drag.y) > distance)
                in->impl.clicks = 0;
        }
        caret_move_to(in, offset_at_x(in, x), true);
        return true;
    case UI_POINTER_RELEASE:
        if (!in->impl.dragging)
            return false;
        in->impl.dragging = false;
        if (in->impl.clicks > 1)
            in->impl.clicks = 0;
        ui_widget_release_pointer(widget);
        return true;
    default:
        return false;
    }
}

static bool input_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_input *in = input_of(widget);
    switch (event->kind) {
    case UI_EVENT_FOCUS_IN:
    case UI_EVENT_FOCUS_OUT:
        in->impl.edit_group = 0;
        if (event->kind == UI_EVENT_FOCUS_OUT)
            in->impl.clicks = 0;
        ui_widget_set_style_pseudo(widget,
            event->kind == UI_EVENT_FOCUS_IN ? CSS_PSEUDO_FOCUS : 0);
        caret_moved(in);
        return true;
    case UI_EVENT_CAPTURE_LOST:
        in->impl.dragging = false;
        in->impl.clicks = 0;
        return true;
    case UI_EVENT_TEXT:
    case UI_EVENT_PASTE: {
        size_t len = event->data.text.len;
        if (!event->data.text.data || !len)
            return false;
        char text[UI_INPUT_CAP];
        if (len >= sizeof(text))
            len = sizeof(text) - 1;
        memcpy(text, event->data.text.data, len);
        text[len] = '\0';
        return ui_input_insert_text(in, text);
    }
    case UI_EVENT_KEY:
        switch (event->data.key.key) {
        case UI_KEY_ESCAPE:
        case UI_KEY_ENTER:
        case UI_KEY_TAB:
        case UI_KEY_UP:
        case UI_KEY_DOWN:
        case UI_KEY_PAGE_UP:
        case UI_KEY_PAGE_DOWN:
            return false;
        default:
            return handle_key(in, &event->data.key);
        }
    case UI_EVENT_POINTER:
        return handle_pointer(in, &event->data.pointer, event->time_ms);
    case UI_EVENT_SHORTCUT:
        switch (event->data.shortcut.edit) {
        case UI_EDIT_COPY:
        case UI_EDIT_PASTE:
        case UI_EDIT_SELECT_ALL:
            return ui_input_command(in, event->data.shortcut.edit);
        default:
            return false;
        }
    default:
        return false;
    }
}

static int input_clamp_offset(const struct ui_input *in, size_t offset)
{
    int pos = offset > (size_t)in->impl.len ? in->impl.len : (int)offset;
    while (pos > 0 && pos < in->impl.len &&
           ((unsigned char)in->impl.buf[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static bool input_accessibility(const struct ui_widget *widget,
                                struct ui_accessibility_node_spec *out)
{
    const struct ui_input *in = input_of(widget);
    int start, end;
    sel_range(in, &start, &end);
    out->role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD;
    out->value = in->impl.buf;
    out->caret_offset = (size_t)in->impl.caret;
    out->selection_start = (size_t)start;
    out->selection_end = (size_t)end;
    out->states |= UI_ACCESSIBILITY_STATE_EDITABLE;
    out->actions |= UI_ACCESSIBILITY_ACTION_SET_VALUE |
                    UI_ACCESSIBILITY_ACTION_SET_SELECTION;
    return true;
}

static bool input_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_input *in = input_of(widget);
    if (node_id != ui_widget_accessibility_id(widget))
        return false;
    if (request->action == UI_ACCESSIBILITY_ACTION_SET_VALUE) {
        if (!request->value)
            return false;
        history_free(&in->impl.undo);
        history_free(&in->impl.redo);
        in->impl.undo_count = in->impl.edit_group = 0;
        in->impl.len = clean_text(in->impl.buf, sizeof(in->impl.buf),
                                  request->value);
        in->impl.caret = input_clamp_offset(in, request->caret_offset);
        in->impl.anchor = in->impl.caret;
        if (request->selection_start != request->selection_end) {
            in->impl.anchor = input_clamp_offset(in,
                                                  request->selection_start);
            in->impl.caret = input_clamp_offset(in,
                                                 request->selection_end);
        }
        changed(in);
        return true;
    }
    if (request->action == UI_ACCESSIBILITY_ACTION_SET_SELECTION) {
        if (request->selection_start == request->selection_end) {
            in->impl.caret = input_clamp_offset(in, request->caret_offset);
            in->impl.anchor = in->impl.caret;
        } else {
            in->impl.anchor = input_clamp_offset(in,
                                                  request->selection_start);
            in->impl.caret = input_clamp_offset(in,
                                                 request->selection_end);
        }
        in->impl.edit_group = 0;
        caret_moved(in);
        return true;
    }
    return false;
}

static uint64_t input_tick(struct ui_widget *widget, uint64_t now_ms)
{
    struct ui_input *in = input_of(widget);
    if (!ui_widget_has_focus(widget))
        return 0;
    if (!in->impl.blink_at) {
        in->impl.blink_at = now_ms + UI_INPUT_BLINK_MS;
    } else if (now_ms >= in->impl.blink_at) {
        in->impl.blink_on = !in->impl.blink_on;
        while (now_ms >= in->impl.blink_at)
            in->impl.blink_at += UI_INPUT_BLINK_MS;
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
    }
    return in->impl.blink_at;
}

static struct vector2i input_measure(const struct ui_widget *widget,
                                     struct ui_constraints constraints)
{
    const struct ui_input *in = input_of(widget);
    struct insets2i pad = ui_ctx_insets(input_scale(in), widget->style.padding, 0);
    int border = border_width(in);
    struct vector2i size = {
        .x = 8 * cell_w(in) + pad.left + pad.right + 2 * border,
        .y = cell_h(in) + pad.top + pad.bottom + 2 * border,
    };
    return ui_constraints_clamp(constraints, size);
}

static void input_layout(struct ui_widget *widget, struct rect2i bounds)
{
    (void)bounds;
    update_scroll(input_of(widget));
}

static uint32_t part_text_color(const struct ui_widget *widget,
                                ui_part_id part, ui_part_id fallback,
                                uint32_t color)
{
    struct css_style s = ui_widget_part(widget, part, 0);
    if (s.has_text_color)
        return s.text_color;
    struct css_style f = ui_widget_part(widget, fallback, 0);
    return f.has_text_color ? f.text_color : color;
}

static void input_paint(const struct ui_widget *widget,
                        struct ui_paint_context *paint)
{
    const struct ui_input *in = input_of(widget);
    const struct css_style *rs = &widget->style;
    const struct css_scale_context *scale = input_scale(in);
    int border = border_width(in);
    int radius = ui_css_resolve_int(rs->border_radius, 0, 0, scale, true);
    struct rect2i outer = ui_paint_bounds(paint);
    if (rs->has_border_color && border > 0) {
        if (radius > 0)
            ui_paint_rounded_rect(paint, outer, radius, rs->border_color);
        else
            ui_paint_rect(paint, outer, rs->border_color);
    }
    if (rs->has_bg_color && rs->bg_color >> 24) {
        struct rect2i fill = border > 0 ? rect2i_inset(outer, border) : outer;
        int fill_radius = radius > border ? radius - border : 0;
        if (fill_radius > 0)
            ui_paint_rounded_rect(paint, fill, fill_radius, rs->bg_color);
        else
            ui_paint_rect(paint, fill, rs->bg_color);
    }

    struct rect2i content = content_rect(in);
    if (rect2i_is_empty(content) || !ui_paint_push_clip(paint, content))
        return;
    int cw = cell_w(in), ch = cell_h(in);
    int text_y = content.y + (content.height - ch) / 2;
    int lo, hi;
    sel_range(in, &lo, &hi);
    if (lo != hi) {
        struct css_style ss = ui_widget_part(widget, input_parts.selection, 0);
        uint32_t color = ss.has_bg_color ? ss.bg_color : INPUT_SEL_FALLBACK;
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = content.x +
                          (cells_to(in, lo) - in->impl.scroll_cells) * cw,
                     .y = content.y },
            .size = { .x = (cells_to(in, hi) - cells_to(in, lo)) * cw,
                      .y = content.height },
        }, color);
    }

    const char *text = in->impl.buf;
    int len = in->impl.len;
    uint32_t fg;
    if (!len && in->impl.placeholder[0]) {
        text = in->impl.placeholder;
        len = (int)strlen(text);
        fg = part_text_color(widget, input_parts.empty, input_parts.empty,
                             INPUT_DIM_FALLBACK);
    } else {
        fg = part_text_color(widget, input_parts.text, input_parts.item,
                             INPUT_FG_FALLBACK);
    }
    if (len > 0) {
        int offset = in->impl.scroll_cells * cw;
        ui_paint_text(paint, (struct rect2i){
            .pos = { .x = content.x - offset, .y = text_y },
            .size = { .x = offset + ui_text_run_cells(text, (size_t)len) * cw,
                      .y = ch },
        }, (struct ui_text_style){ .advance = cw, .cell_h = ch }, fg,
           text, (size_t)len);
    }

    if (in->impl.blink_on && ui_widget_has_focus(widget)) {
        int caret_w = ui_ctx_px(scale, 2);
        struct css_style cs = ui_widget_part(widget, input_parts.caret, 0);
        uint32_t color = cs.has_bg_color ? cs.bg_color
            : part_text_color(widget, input_parts.item, input_parts.item,
                              INPUT_CARET_FALLBACK);
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = content.x +
                          (cells_to(in, in->impl.caret) - in->impl.scroll_cells) * cw,
                     .y = text_y },
            .size = { .x = caret_w > 0 ? caret_w : 1, .y = ch },
        }, color);
    }
    ui_paint_pop_clip(paint);
}

static void input_destroy(struct ui_widget *widget)
{
    struct ui_input *in = input_of(widget);
    history_free(&in->impl.undo);
    history_free(&in->impl.redo);
}

static const struct ui_widget_ops INPUT_OPS = {
    .name = "input",
    .destroy = input_destroy,
    .measure = input_measure,
    .layout = input_layout,
    .event = input_event,
    .tick = input_tick,
    .paint = input_paint,
    .accessibility = input_accessibility,
    .accessibility_action = input_accessibility_action,
};

void ui_input_init(struct ui_input *in)
{
    if (!in)
        return;
    *in = (struct ui_input){0};
    input_ensure_init(in);
}

struct ui_input *ui_input_of(struct ui_widget *widget)
{
    return widget && widget->ops == &INPUT_OPS ? input_of(widget) : NULL;
}
