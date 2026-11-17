#include "ui/widgets/input.h"

#include "ui/paint.h"
#include "paint/text_run.h"
#include "ui/text_break.h"
#include "ui/surface.h"
#include "ui/css.h"
#include "ui/widget.h"
#include "ui/widget_ops.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

static struct ui_input *input_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_input, widget);
}

static const struct css_scale_context *input_scale(const struct ui_input *in)
{
    return &in->widget.style_scale;
}

static struct rect2i content_rect(const struct ui_input *in)
{
    struct rect2i fill = rect2i_inset(
        (struct rect2i){ .size = in->widget.bounds.size },
        ui_widget_border_width(&in->widget));
    return rect2i_apply_insets(
        fill, ui_ctx_insets(input_scale(in), in->widget.style.padding, 0));
}

int ui_input_text_inset(const struct ui_input *in)
{
    return in ? ui_widget_border_width(&in->widget) +
                ui_ctx_inset(input_scale(in), in->widget.style.padding.left, 0)
              : 0;
}

static int cells_to(const struct ui_input *in, size_t pos)
{
    return ui_text_run_cells(ui_input_text(in), pos);
}

static void sel_range(const struct ui_input *in, size_t *lo, size_t *hi)
{
    if (ui_text_select_range(&in->impl.select, lo, hi))
        return;
    *lo = *hi = in->impl.select.caret;
}

struct ui_input_history {
    struct ui_input_history *next;
    size_t len, caret, anchor;
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
    struct ui_input_history *entry =
        malloc(sizeof(*entry) + in->impl.len + 1);
    if (!entry) return NULL;
    *entry = (struct ui_input_history){ .len = in->impl.len,
        .caret = in->impl.select.caret, .anchor = in->impl.select.anchor };
    memcpy(entry->text, ui_input_text(in), in->impl.len + 1);
    return entry;
}

/* Drop the edit history. An edit that cannot record history still applies. */
static void history_drop(struct ui_input *in)
{
    history_free(&in->impl.undo);
    history_free(&in->impl.redo);
    in->impl.undo_count = 0;
    in->impl.edit_group = 0;
}

static void history_record(struct ui_input *in, int group)
{
    if (!group || in->impl.edit_group != group || in->impl.select.caret != in->impl.edit_caret ||
        in->impl.select.caret != in->impl.select.anchor) {
        struct ui_input_history *entry = history_snapshot(in);
        if (!entry) {
            history_drop(in);
            return;
        }
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
    history_free(&in->impl.redo);
    in->impl.edit_group = group;
}

static void update_scroll(struct ui_input *in)
{
    if (!in->widget.bounds.width)
        return;
    int cap = content_rect(in).width / ui_widget_cell_w(&in->widget);
    if (cap < 1)
        cap = 1;
    int max_scroll = cells_to(in, in->impl.len) - cap + 1;
    if (max_scroll < 0)
        max_scroll = 0;
    if (in->impl.scroll_cells > max_scroll)
        in->impl.scroll_cells = max_scroll;
    int caret = cells_to(in, in->impl.select.caret);
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
    in->impl.edit_caret = in->impl.select.caret;
    caret_moved(in);
    ui_widget_notify(&in->widget, (struct ui_action){
        .kind = UI_ACTION_CHANGE, .id = in->impl.action_id, .index = -1,
    });
}

static void delete_range(struct ui_input *in, size_t lo, size_t hi)
{
    if (hi > in->impl.len)
        hi = in->impl.len;
    if (lo >= hi)
        return;
    history_record(in, 0);
    memmove(in->impl.buf + lo, in->impl.buf + hi, in->impl.len - hi + 1);
    in->impl.len -= hi - lo;
    in->impl.select.caret = in->impl.select.anchor = lo;
    changed(in);
}

/* Copy valid UTF-8. Input values omit ASCII control characters. */
static bool copy_text(const char *text, size_t len, bool value,
                      char **out, size_t *out_len)
{
    if (len > INT_MAX)
        return false;
    if (!len) {
        *out = NULL;
        if (out_len)
            *out_len = 0;
        return true;
    }
    char *copy = malloc(len + 1);
    if (!copy)
        return false;
    size_t offset = 0, used = 0;
    while (offset < len && text[offset]) {
        uint32_t cp;
        int n = wsap_vtcore_utf8_decode_one(text + offset, len - offset, &cp);
        if (n <= 0) {
            offset++;
            continue;
        }
        if (!value || (cp >= 0x20 && cp != 0x7f)) {
            memcpy(copy + used, text + offset, (size_t)n);
            used += (size_t)n;
        }
        offset += (size_t)n;
    }
    copy[used] = '\0';
    *out = copy;
    if (out_len)
        *out_len = used;
    return true;
}

static bool reserve_text(struct ui_input *in, size_t len)
{
    if (len > INT_MAX)
        return false;
    size_t need = len + 1;
    if (need <= in->impl.capacity)
        return true;
    size_t capacity = in->impl.capacity ? in->impl.capacity : 32;
    while (capacity < need) {
        size_t next = capacity * 2;
        if (next > (size_t)INT_MAX + 1 || next < capacity) {
            capacity = need;
            break;
        }
        capacity = next;
    }
    bool empty = !in->impl.buf;
    char *buf = realloc(in->impl.buf, capacity);
    if (!buf)
        return false;
    if (empty)
        buf[0] = '\0';
    in->impl.buf = buf;
    in->impl.capacity = capacity;
    return true;
}

static void caret_move_to(struct ui_input *in, size_t pos, bool extend)
{
    in->impl.edit_group = 0;
    if (pos > in->impl.len)
        pos = in->impl.len;
    ui_text_select_to(&in->impl.select, pos, extend);
    caret_moved(in);
}

static void caret_move(struct ui_input *in, enum ui_edit_unit unit,
                       bool backward, bool extend)
{
    in->impl.edit_group = 0;
    ui_text_select_move(&in->impl.select, ui_input_text(in),
                        in->impl.len, unit, backward, extend);
    caret_moved(in);
}

const char *ui_input_text(const struct ui_input *in)
{
    return in && in->impl.buf ? in->impl.buf : "";
}

size_t ui_input_caret(const struct ui_input *in)
{
    return in ? in->impl.select.caret : 0;
}

bool ui_input_selection(const struct ui_input *in, size_t *start, size_t *end)
{
    return ui_text_select_range(in ? &in->impl.select : NULL, start, end);
}

static struct {
    ui_part_id text;
    ui_part_id empty;
    ui_part_id selection;
    ui_part_id caret;
} input_parts;

static void input_parts_init(void)
{
    if (input_parts.text)
        return;
    input_parts.text = ui_part_intern("text-input-text");
    input_parts.empty = ui_part_intern("text-input-text empty");
    input_parts.selection = ui_part_intern("text-input-selection");
    input_parts.caret = ui_part_intern("text-input-caret");
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

bool ui_input_set_text(struct ui_input *in, const char *utf8)
{
    if (!in)
        return false;
    input_ensure_init(in);
    if (!utf8)
        utf8 = "";
    char *copy;
    size_t len;
    if (!copy_text(utf8, strlen(utf8), true, &copy, &len))
        return false;
    history_drop(in);
    free(in->impl.buf);
    in->impl.buf = copy;
    in->impl.len = len;
    in->impl.capacity = copy ? len + 1 : 0;
    in->impl.select.caret = in->impl.select.anchor = in->impl.len;
    in->impl.edit_caret = in->impl.select.caret;
    caret_moved(in);
    return true;
}

bool ui_input_set_placeholder(struct ui_input *in, const char *utf8)
{
    if (!in)
        return false;
    input_ensure_init(in);
    if (!utf8)
        utf8 = "";
    char *copy;
    if (!copy_text(utf8, strlen(utf8), false, &copy, NULL))
        return false;
    free(in->impl.placeholder);
    in->impl.placeholder = copy;
    ui_widget_invalidate(&in->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
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
    ui_text_select_all(&in->impl.select, in->impl.len);
    caret_moved(in);
}

void ui_input_clear(struct ui_input *in)
{
    if (!in)
        return;
    input_ensure_init(in);
    history_record(in, 0);
    in->impl.len = 0;
    if (in->impl.buf)
        in->impl.buf[0] = '\0';
    in->impl.select.caret = in->impl.select.anchor = 0;
    in->impl.scroll_cells = 0;
    changed(in);
}

/* `group` joins a typing burst into one undo entry. Use 0 for other input. */
static bool input_insert_text(struct ui_input *in, const char *utf8, size_t len,
                              int group)
{
    if (!in || !utf8)
        return false;
    input_ensure_init(in);
    char *clean;
    size_t n;
    if (!copy_text(utf8, len, true, &clean, &n))
        return false;
    size_t lo, hi;
    sel_range(in, &lo, &hi);
    size_t result_len = in->impl.len - (hi - lo) + n;
    if ((!n && lo == hi) || !reserve_text(in, result_len)) {
        free(clean);
        return false;
    }
    history_record(in, group);
    memmove(in->impl.buf + lo + n, in->impl.buf + hi,
            in->impl.len - hi + 1);
    if (n)
        memcpy(in->impl.buf + lo, clean, n);
    free(clean);
    in->impl.len = result_len;
    in->impl.select.caret = in->impl.select.anchor = lo + n;
    changed(in);
    return true;
}

bool ui_input_insert_text(struct ui_input *in, const char *utf8)
{
    return utf8 && input_insert_text(in, utf8, strlen(utf8), 0);
}

static char *dup_slice(const struct ui_input *in, size_t lo, size_t hi)
{
    size_t len = hi - lo;
    char *text = malloc(len + 1);
    if (!text)
        return NULL;
    if (len)
        memcpy(text, ui_input_text(in) + lo, len);
    text[len] = '\0';
    return text;
}

static size_t edit_move(const struct ui_input *in, enum ui_edit_unit unit,
                        bool backward)
{
    return ui_text_move(ui_input_text(in), in->impl.len,
                        in->impl.select.caret, unit, backward);
}

static void edit_range(const struct ui_input *in, enum ui_edit_unit unit,
                       bool backward, bool to_edge, size_t *lo, size_t *hi)
{
    sel_range(in, lo, hi);
    if (*lo != *hi)
        return;
    if (backward)
        *lo = to_edge ? 0 : edit_move(in, unit, true);
    else
        *hi = to_edge ? in->impl.len : edit_move(in, unit, false);
}

bool ui_input_can_command(const struct ui_input *in, enum ui_edit_command command)
{
    if (!in) return false;
    struct ui_surface *surface = ui_widget_surface(&in->widget);
    switch (command) {
    case UI_EDIT_UNDO: return in->impl.undo != NULL;
    case UI_EDIT_REDO: return in->impl.redo != NULL;
    case UI_EDIT_COPY:
    case UI_EDIT_CUT:
        return in->impl.select.caret != in->impl.select.anchor &&
               ui_surface_can_copy(surface);
    case UI_EDIT_PASTE: return ui_surface_can_paste(surface);
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
    if (!current) return false;
    if (!reserve_text(in, (*from)->len)) {
        free(current);
        return false;
    }
    current->next = *to;
    *to = current;
    struct ui_input_history *entry = *from;
    *from = entry->next;
    in->impl.len = entry->len;
    in->impl.select.caret = entry->caret;
    in->impl.select.anchor = entry->anchor;
    memcpy(in->impl.buf, entry->text, entry->len + 1);
    free(entry);
    in->impl.undo_count += redo ? 1 : -1;
    in->impl.edit_group = 0;
    changed(in);
    return true;
}

enum input_cmd_flag {
    CMD_BACKWARD = 1 << 0,
    CMD_EDGE     = 1 << 1,
    CMD_KILL     = 1 << 2,
    CMD_CUT      = 1 << 3,
    CMD_REDO     = 1 << 4,
};

/* Maps an edit command to its handler and range behavior. */
struct input_cmd {
    bool (*run)(struct ui_input *in, const struct input_cmd *cmd);
    enum ui_edit_command command;
    enum ui_edit_unit unit;
    uint8_t flags;
};

static bool cmd_history(struct ui_input *in, const struct input_cmd *cmd)
{
    return history_restore(in, cmd->flags & CMD_REDO);
}

static bool cmd_select_all(struct ui_input *in, const struct input_cmd *cmd)
{
    (void)cmd;
    ui_input_select_all(in);
    return true;
}

static bool cmd_clipboard(struct ui_input *in, const struct input_cmd *cmd)
{
    size_t lo, hi;
    sel_range(in, &lo, &hi);
    if (lo == hi)
        return true;
    char *text = dup_slice(in, lo, hi);
    if (!text)
        return true;
    bool copied = ui_surface_copy_text(ui_widget_surface(&in->widget), text);
    free(text);
    if (copied && (cmd->flags & CMD_CUT))
        delete_range(in, lo, hi);
    return true;
}

static bool cmd_paste(struct ui_input *in, const struct input_cmd *cmd)
{
    (void)cmd;
    ui_surface_paste_text(ui_widget_surface(&in->widget));
    return true;
}

static bool cmd_yank(struct ui_input *in, const struct input_cmd *cmd)
{
    (void)cmd;
    ui_input_insert_text(in, in->impl.kill);
    return true;
}

static bool cmd_delete(struct ui_input *in, const struct input_cmd *cmd)
{
    size_t lo, hi;
    edit_range(in, cmd->unit, cmd->flags & CMD_BACKWARD,
               cmd->flags & CMD_EDGE, &lo, &hi);
    if (cmd->flags & CMD_KILL) {
        char *kill = dup_slice(in, lo, hi);
        if (!kill)
            return true;
        free(in->impl.kill);
        in->impl.kill = kill;
    }
    delete_range(in, lo, hi);
    return true;
}

static bool cmd_transpose(struct ui_input *in, const struct input_cmd *cmd)
{
    (void)cmd;
    size_t lo, hi;
    sel_range(in, &lo, &hi);
    if (lo != hi || in->impl.len == 0 || in->impl.select.caret == 0)
        return true;
    size_t end = in->impl.select.caret == in->impl.len
        ? in->impl.len
        : edit_move(in, UI_EDIT_CHARACTER, false);
    size_t middle = ui_text_move(in->impl.buf, in->impl.len, end,
                                 UI_EDIT_CHARACTER, true);
    size_t start = ui_text_move(in->impl.buf, in->impl.len, middle,
                                UI_EDIT_CHARACTER, true);
    if (start == middle)
        return true;
    size_t first = middle - start;
    size_t second = end - middle;
    char *text = malloc(first);
    if (!text)
        return true;
    history_record(in, 0);
    memcpy(text, in->impl.buf + start, first);
    memmove(in->impl.buf + start, in->impl.buf + middle, second);
    memcpy(in->impl.buf + start + second, text, first);
    free(text);
    in->impl.select.caret = in->impl.select.anchor = end;
    changed(in);
    return true;
}

static const struct input_cmd INPUT_CMDS[] = {
    { .command = UI_EDIT_UNDO,           .run = cmd_history },
    { .command = UI_EDIT_REDO,           .run = cmd_history,    .flags = CMD_REDO },
    { .command = UI_EDIT_SELECT_ALL,     .run = cmd_select_all },
    { .command = UI_EDIT_COPY,           .run = cmd_clipboard },
    { .command = UI_EDIT_CUT,            .run = cmd_clipboard,  .flags = CMD_CUT },
    { .command = UI_EDIT_PASTE,          .run = cmd_paste },
    { .command = UI_EDIT_YANK,           .run = cmd_yank },
    { .command = UI_EDIT_DELETE_BACK,    .run = cmd_delete,
      .unit = UI_EDIT_CHARACTER, .flags = CMD_BACKWARD },
    { .command = UI_EDIT_DELETE_FORWARD, .run = cmd_delete,
      .unit = UI_EDIT_CHARACTER },
    { .command = UI_EDIT_KILL_START,     .run = cmd_delete,
      .flags = CMD_BACKWARD | CMD_EDGE | CMD_KILL },
    { .command = UI_EDIT_KILL_END,       .run = cmd_delete,
      .flags = CMD_EDGE | CMD_KILL },
    { .command = UI_EDIT_KILL_WORD,      .run = cmd_delete,
      .unit = UI_EDIT_WORD_END, .flags = CMD_BACKWARD | CMD_KILL },
    { .command = UI_EDIT_TRANSPOSE,      .run = cmd_transpose },
};

bool ui_input_command(struct ui_input *in, enum ui_edit_command command)
{
    if (!in)
        return false;
    input_ensure_init(in);
    for (size_t i = 0; i < sizeof(INPUT_CMDS) / sizeof(INPUT_CMDS[0]); i++) {
        if (INPUT_CMDS[i].command == command)
            return INPUT_CMDS[i].run(in, &INPUT_CMDS[i]);
    }
    return false;
}

/* Runs one edit command. Both event fields of a key call it.
 * `extend` keeps the selection on a caret move. */
static bool input_edit(struct ui_input *in, enum ui_edit_command command,
                       bool extend)
{
    switch (command) {
    case UI_EDIT_NONE:
        return false;
    case UI_EDIT_START:
        caret_move_to(in, 0, extend);
        return true;
    case UI_EDIT_END:
        caret_move_to(in, in->impl.len, extend);
        return true;
    case UI_EDIT_LEFT:
    case UI_EDIT_RIGHT:
        caret_move(in, UI_EDIT_CHARACTER, command == UI_EDIT_LEFT, extend);
        return true;
    default:
        return ui_input_command(in, command);
    }
}

static bool handle_key(struct ui_input *in, const struct ui_key_event *ev)
{
    bool shift = (ev->mods & UI_MOD_SHIFT) != 0;
    if (input_edit(in, ev->edit_command, shift))
        return true;
    size_t lo, hi;
    switch (ev->key) {
    case UI_KEY_LEFT:
    case UI_KEY_RIGHT:
        caret_move(in, ev->edit_unit, ev->key == UI_KEY_LEFT, shift);
        return true;
    case UI_KEY_HOME:
    case UI_KEY_END:
        caret_move_to(in, ev->key == UI_KEY_HOME ? 0 : in->impl.len, shift);
        return true;
    case UI_KEY_BACKSPACE:
    case UI_KEY_DELETE:
        edit_range(in, ev->edit_unit, ev->key == UI_KEY_BACKSPACE, false,
                   &lo, &hi);
        delete_range(in, lo, hi);
        return true;
    case UI_KEY_CHAR: {
        if (ev->mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SUPER))
            return false;
        if (ev->text_deferred || ev->text < 0x20 || ev->text == 0x7f)
            return false;
        char enc[WSAP_VTCORE_UTF8_MAX_BYTES + 1];
        int n = wsap_vtcore_utf8_encode(ev->text, enc);
        if (n <= 0)
            return false;
        enc[n] = '\0';
        return input_insert_text(in, enc, (size_t)n, 1);
    }
    default:
        return false;
    }
}

static size_t offset_at_x(const struct ui_input *in, int x)
{
    int cw = ui_widget_cell_w(&in->widget);
    int64_t rel = (int64_t)x - ui_input_text_inset(in) +
                  (int64_t)in->impl.scroll_cells * cw;
    return ui_text_offset_at_x(ui_input_text(in), in->impl.len, rel, cw);
}

static bool handle_pointer(struct ui_input *in, const struct ui_pointer_event *p,
                           uint64_t time_ms)
{
    struct ui_widget *widget = &in->widget;
    int x = ui_widget_to_local(widget, p->pos).x;
    int threshold = ui_px(widget->style_scale.dpi_scale, UI_TEXT_SELECT_DRAG_PX);
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
        in->impl.edit_group = 0;
        ui_text_select_press(&in->impl.select, ui_input_text(in),
            in->impl.len, &(struct ui_text_select_click){
                .pos = p->pos,
                .time_ms = time_ms,
                .offset = offset_at_x(in, x),
                .interval_ms = ui_surface_double_click_interval_ms(
                    ui_widget_surface(widget)),
                .threshold_px = threshold,
                .mods = p->mods,
                .selects = true,
            });
        caret_moved(in);
        return true;
    case UI_POINTER_MOTION:
        if (!ui_text_select_drag(&in->impl.select, ui_input_text(in),
                                 in->impl.len, offset_at_x(in, x), p->pos,
                                 threshold))
            return in->impl.select.pressed;
        caret_moved(in);
        return true;
    case UI_POINTER_RELEASE:
        if (!in->impl.select.pressed)
            return false;
        ui_text_select_release(&in->impl.select);
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
        ui_text_select_end(&in->impl.select);
        ui_widget_set_style_pseudo(widget,
            event->kind == UI_EVENT_FOCUS_IN ? CSS_PSEUDO_FOCUS : 0);
        caret_moved(in);
        return true;
    case UI_EVENT_CAPTURE_LOST:
        ui_text_select_end(&in->impl.select);
        return true;
    case UI_EVENT_TEXT:
    case UI_EVENT_PASTE: {
        size_t len = event->data.text.len;
        if (!event->data.text.data || !len)
            return false;
        return input_insert_text(in, event->data.text.data, len,
                                 event->kind == UI_EVENT_TEXT);
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
        return input_edit(in, event->data.shortcut.edit, false);
    default:
        return false;
    }
}

static bool input_accessibility(const struct ui_widget *widget,
                                struct ui_accessibility_node_spec *out)
{
    const struct ui_input *in = input_of(widget);
    size_t start, end;
    sel_range(in, &start, &end);
    out->role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD;
    out->value = ui_input_text(in);
    out->caret_offset = in->impl.select.caret;
    out->selection_start = start;
    out->selection_end = end;
    out->states |= UI_ACCESSIBILITY_STATE_EDITABLE;
    out->actions |= UI_ACCESSIBILITY_ACTION_SET_VALUE |
                    UI_ACCESSIBILITY_ACTION_SET_SELECTION;
    return true;
}

static void apply_accessible_selection(
    struct ui_input *in,
    const struct ui_accessibility_action_request *request)
{
    ui_text_select_apply(&in->impl.select, ui_input_text(in), in->impl.len,
                         request->selection_start, request->selection_end,
                         request->caret_offset);
}

static bool input_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_input *in = input_of(widget);
    if (node_id != ui_widget_accessibility_id(widget))
        return false;
    if (request->action == UI_ACCESSIBILITY_ACTION_SET_VALUE) {
        if (!request->value || !ui_input_set_text(in, request->value))
            return false;
        apply_accessible_selection(in, request);
        changed(in);
        return true;
    }
    if (request->action == UI_ACCESSIBILITY_ACTION_SET_SELECTION) {
        apply_accessible_selection(in, request);
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
    int border = ui_widget_border_width(&in->widget);
    struct vector2i size = {
        .x = 8 * ui_widget_cell_w(&in->widget) + pad.left + pad.right + 2 * border,
        .y = ui_widget_cell_h(&in->widget) + pad.top + pad.bottom + 2 * border,
    };
    return ui_constraints_clamp(constraints, size);
}

static void input_layout(struct ui_widget *widget, struct rect2i bounds)
{
    (void)bounds;
    update_scroll(input_of(widget));
}

/* Returns the color of a part. A part with no color falls back to
 * `fallback`. */
static uint32_t part_color(const struct ui_widget *widget, ui_part_id part,
                           bool bg, uint32_t fallback)
{
    struct css_style s = ui_widget_part(widget, part, 0);
    uint32_t color = bg ? (s.has_bg_color ? s.bg_color : fallback)
                        : (s.has_text_color ? s.text_color : fallback);
    return ui_color_opacity(color,
                            css_style_effective_opacity(&widget->style) *
                                css_style_effective_opacity(&s));
}

static void input_paint(const struct ui_widget *widget,
                        struct ui_paint_context *paint)
{
    const struct ui_input *in = input_of(widget);
    const struct css_style *rs = &widget->style;
    const struct css_scale_context *scale = input_scale(in);
    struct ui_paint_box_style box = ui_paint_box_style_of(rs, scale);
    struct rect2i fill;
    if (!ui_paint_box(paint, ui_paint_bounds(paint), &box, &fill))
        return;

    struct rect2i content = rect2i_apply_insets(
        fill, ui_ctx_insets(scale, rs->padding, 0));
    if (rect2i_is_empty(content) || !ui_paint_push_clip(paint, content))
        return;
    int cw = ui_widget_cell_w(&in->widget), ch = ui_widget_cell_h(&in->widget);
    int text_y = content.y + (content.height - ch) / 2;
    size_t lo, hi;
    sel_range(in, &lo, &hi);
    if (lo != hi) {
        /* A selection with no color is transparent. The default sheet
         * supplies the color. */
        uint32_t color = part_color(widget, input_parts.selection, true, 0);
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = geom_sat_i((int64_t)content.x +
                          ((int64_t)cells_to(in, lo) - in->impl.scroll_cells) * cw),
                     .y = content.y },
            .size = { .x = geom_sat_i(((int64_t)cells_to(in, hi) - cells_to(in, lo)) * cw),
                      .y = content.height },
        }, color);
    }

    const char *text = ui_input_text(in);
    size_t len = in->impl.len;
    uint32_t fg;
    if (!len && in->impl.placeholder && in->impl.placeholder[0]) {
        text = in->impl.placeholder;
        len = strlen(text);
        fg = part_color(widget, input_parts.empty, false,
                        rs->has_text_color ? rs->text_color : 0);
    } else {
        fg = part_color(widget, input_parts.text, false,
                        rs->has_text_color ? rs->text_color : 0);
    }
    if (len > 0) {
        int64_t offset = (int64_t)in->impl.scroll_cells * cw;
        ui_paint_text(paint, (struct rect2i){
            .pos = { .x = geom_sat_i((int64_t)content.x - offset), .y = text_y },
            .size = { .x = geom_sat_i(offset + (int64_t)ui_text_run_cells(text, len) * cw),
                      .y = ch },
        }, (struct ui_text_style){ .advance = cw, .cell_h = ch }, fg,
           text, len);
    }

    if (in->impl.blink_on && ui_widget_has_focus(widget)) {
        int caret_w = ui_ctx_px(scale, 2);
        uint32_t color = part_color(widget, input_parts.caret, true, 0);
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = geom_sat_i((int64_t)content.x +
                          ((int64_t)cells_to(in, in->impl.select.caret) - in->impl.scroll_cells) * cw),
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
    free(in->impl.buf);
    free(in->impl.placeholder);
    free(in->impl.kill);
    in->impl.buf = NULL;
    in->impl.placeholder = NULL;
    in->impl.kill = NULL;
    in->impl.capacity = 0;
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
