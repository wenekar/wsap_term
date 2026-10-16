#include "internal.h"
#include "yasl/yasl.h"

#include <errno.h>

#define IME_TEXT_MAX (256u * 1024)

static void show_preedit(struct wayland_surface *w, char *text, int begin, int end)
{
    bool changed = (text || w->ime_shown) &&
                   (!text || !w->ime_shown || strcmp(text, w->ime_shown) ||
                    begin != w->ime_shown_begin || end != w->ime_shown_end);
    free(w->ime_shown);
    w->ime_shown = text;
    w->ime_shown_begin = begin;
    w->ime_shown_end = end;
    if (changed) {
        yasl_private_surface_notify_composition(&w->base, w->ime_shown,
                                        w->ime_shown_begin, w->ime_shown_end);
        wayland_surface_schedule_redraw(w);
    }
}

void ime_reset(struct wayland_surface *w)
{
    free(w->ime_preedit);
    w->ime_preedit = NULL;
    free(w->ime_commit);
    w->ime_commit = NULL;
    w->ime_delete_pending = false;
    show_preedit(w, NULL, -1, -1);
}

void ime_send_caret(struct wayland_surface *w)
{
    wayland_surface_update_text_input(w);
}

/* yasl's purposes are its own values. The protocol's values differ, so map
 * each one. Unknown values fall back to normal text. */
static uint32_t content_purpose(unsigned purpose)
{
    switch (purpose) {
    case YASL_TEXT_PASSWORD: return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
    case YASL_TEXT_NUMBER:   return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER;
    case YASL_TEXT_TERMINAL: return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL;
    default:                 return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
    }
}

bool wayland_surface_update_text_input(struct wayland_surface *w)
{
    if (!w || !w->conn) {
        errno = EINVAL;
        return false;
    }
    if (!w->conn->text_input_mgr) {
        errno = ENOTSUP;
        return false;
    }
    struct seat *seat = w->ime_seat;
    if (!seat || !seat->text_input || seat->ime_focus != w)
        return true;
    if (!w->base.text_input_active) {
        if (seat->ime_enabled) {
            zwp_text_input_v3_disable(seat->text_input);
            zwp_text_input_v3_commit(seat->text_input);
            seat->ime_enabled = false;
        }
        ime_reset(w);
        return true;
    }
    if (!seat->ime_enabled) {
        zwp_text_input_v3_enable(seat->text_input);
        seat->ime_enabled = true;
    }
    zwp_text_input_v3_set_content_type(seat->text_input,
            ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, content_purpose(w->base.text_purpose));
    const char *surrounding = w->base.text_surrounding ? w->base.text_surrounding : "";
    size_t length = w->base.text_surrounding ? w->base.text_length : 0;
    size_t cursor = w->base.text_surrounding ? w->base.text_end : 0;
    size_t anchor = w->base.text_surrounding ? w->base.text_begin : 0;
    if (cursor > length) cursor = length;
    if (anchor > length) anchor = length;
    if (length <= INT32_MAX && cursor <= INT32_MAX && anchor <= INT32_MAX)
        zwp_text_input_v3_set_surrounding_text(seat->text_input, surrounding,
                (int32_t)cursor, (int32_t)anchor);
    uint32_t scale = w->scale_120 ? w->scale_120 : YASL_PRIVATE_SCALE_UNIT;
    zwp_text_input_v3_set_cursor_rectangle(seat->text_input,
            yasl_private_scale_to_logical(w->ime_caret.x, scale),
            yasl_private_scale_to_logical(w->ime_caret.y, scale),
            yasl_private_scale_to_logical(w->ime_caret.width, scale),
            yasl_private_scale_to_logical(w->ime_caret.height, scale));
    zwp_text_input_v3_commit(seat->text_input);
    return true;
}

bool wayland_surface_ime_preedit_cursor(const struct wayland_surface *w, int *begin, int *end)
{
    if (!w || !w->ime_shown || w->ime_shown_begin < 0)
        return false;
    *begin = w->ime_shown_begin;
    *end = w->ime_shown_end;
    return true;
}

void wayland_surface_set_ime_caret(struct wayland_surface *w, struct yasl_private_rect2i caret)
{
    if (!w || (w->ime_caret.x == caret.x && w->ime_caret.y == caret.y &&
               w->ime_caret.width == caret.width && w->ime_caret.height == caret.height))
        return;
    w->ime_caret = caret;
    ime_send_caret(w);
}

static void text_input_enter(void *data, struct zwp_text_input_v3 *text_input,
                             struct wl_surface *surface)
{
    (void)text_input;
    struct seat *s = data;
    struct wayland_surface *w = surface_root(surface ?
        (struct wayland_surface *)wl_surface_get_user_data(surface) : NULL);
    s->ime_focus = w;
    if (!w) return;
    w->ime_seat = s;
    wayland_surface_update_text_input(w);
}

static void text_input_leave(void *data, struct zwp_text_input_v3 *text_input,
                             struct wl_surface *surface)
{
    (void)surface;
    struct seat *s = data;
    struct wayland_surface *w = s->ime_focus;
    s->ime_focus = NULL;
    if (!w) return;
    if (s->ime_enabled) {
        zwp_text_input_v3_disable(text_input);
        zwp_text_input_v3_commit(text_input);
        s->ime_enabled = false;
    }
    if (w->ime_seat == s)
        w->ime_seat = NULL;
    ime_reset(w);
}

/* Copy protocol text, which libwayland always NUL-terminates, up to
 * IME_TEXT_MAX bytes. Measuring with strlen keeps GCC from flagging a
 * bounded read past a short caller buffer. */
static char *copy_text(const char *text)
{
    size_t len = strlen(text);
    if (len > IME_TEXT_MAX) len = IME_TEXT_MAX;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static void text_input_preedit_string(void *data,
        struct zwp_text_input_v3 *text_input, const char *text,
        int32_t cursor_begin, int32_t cursor_end)
{
    (void)text_input;
    struct wayland_surface *w = ((struct seat *)data)->ime_focus;
    if (!w || !w->base.text_input_active) return;
    keyboard_text_reset((struct seat *)data);
    free(w->ime_preedit);
    w->ime_preedit = text ? copy_text(text) : NULL;
    int len = w->ime_preedit ? (int)strlen(w->ime_preedit) : 0;
    bool shown = cursor_begin >= 0 && cursor_end >= cursor_begin &&
                 cursor_end <= len;
    w->ime_cursor_begin = shown ? cursor_begin : -1;
    w->ime_cursor_end = shown ? cursor_end : -1;
}

static void text_input_commit_string(void *data,
        struct zwp_text_input_v3 *text_input, const char *text)
{
    (void)text_input;
    struct wayland_surface *w = ((struct seat *)data)->ime_focus;
    if (!w || !w->base.text_input_active) return;
    keyboard_text_reset((struct seat *)data);
    free(w->ime_commit);
    w->ime_commit = (text && text[0]) ? copy_text(text) : NULL;
}

static void text_input_delete_surrounding_text(void *data,
        struct zwp_text_input_v3 *text_input, uint32_t before_length,
        uint32_t after_length)
{
    (void)text_input;
    struct seat *seat = data;
    struct wayland_surface *w = seat->ime_focus;
    if (!w || !w->base.text_input_active || !w->base.text_surrounding)
        return;
    size_t length = w->base.text_length;
    size_t cursor = w->base.text_end <= length ? w->base.text_end : length;
    while (cursor && cursor < length &&
           (((unsigned char)w->base.text_surrounding[cursor] & 0xc0) == 0x80))
        cursor--;
    size_t begin = before_length > cursor ? 0 : cursor - before_length;
    size_t end = after_length > length - cursor ? length : cursor + after_length;
    while (begin && begin < length &&
           (((unsigned char)w->base.text_surrounding[begin] & 0xc0) == 0x80))
        begin--;
    while (end < length && (((unsigned char)w->base.text_surrounding[end] & 0xc0) == 0x80))
        end++;
    /* text_input_v3 batches deletion and commit into one `done` group. Keep
     * the replacement range paired with that group's committed text and
     * enqueue both together below, preserving snapshot metadata. */
    w->ime_delete_pending = true;
    w->ime_delete_begin = begin;
    w->ime_delete_end = end;
    w->ime_delete_revision = w->base.text_revision;
    w->ime_delete_generation = w->base.text_generation;
}

static void text_input_done(void *data, struct zwp_text_input_v3 *text_input,
                            uint32_t serial)
{
    (void)text_input; (void)serial;
    struct wayland_surface *w = ((struct seat *)data)->ime_focus;
    if (!w || !w->base.text_input_active) return;
    bool has_snapshot = w->base.text_surrounding != NULL &&
                        w->base.text_length <= INT32_MAX;
    bool has_commit = w->ime_commit && w->ime_commit[0];
    bool edit = w->ime_delete_pending || (has_commit && has_snapshot);
    if (edit) {
        size_t begin = w->ime_delete_pending ? w->ime_delete_begin : w->base.text_begin;
        size_t end = w->ime_delete_pending ? w->ime_delete_end : w->base.text_end;
        if (begin > w->base.text_length) begin = w->base.text_length;
        if (end > w->base.text_length) end = w->base.text_length;
        if (end < begin) end = begin;
        struct yasl_private_event ev = {
            .type = YASL_PRIVATE_EVENT_TEXT_EDIT,
            .surface = &w->base,
            .text_edit = {
                .data = has_commit ? w->ime_commit : "",
                .len = has_commit ? strlen(w->ime_commit) : 0,
                .revision = w->ime_delete_revision,
                .generation = w->ime_delete_generation,
                .begin = begin,
                .end = end,
            },
        };
        if (!w->ime_delete_pending) {
            ev.text_edit.revision = w->base.text_revision;
            ev.text_edit.generation = w->base.text_generation;
        }
        input_queue_push(&w->input, &ev);
        w->ime_delete_pending = false;
    }
    if (w->ime_commit && !edit) {
        struct yasl_private_event ev = {
            .type = YASL_PRIVATE_EVENT_TEXT,
            .surface = &w->base,
            .text = { .data = w->ime_commit, .len = strlen(w->ime_commit) },
        };
        input_queue_push(&w->input, &ev);
    }
    free(w->ime_commit);
    w->ime_commit = NULL;
    char *preedit = w->ime_preedit && w->ime_preedit[0] ? w->ime_preedit : NULL;
    if (!preedit)
        free(w->ime_preedit);
    w->ime_preedit = NULL;
    show_preedit(w, preedit, preedit ? w->ime_cursor_begin : -1,
                 preedit ? w->ime_cursor_end : -1);
    w->ime_cursor_begin = w->ime_cursor_end = -1;
}

static void text_input_action(void *data, struct zwp_text_input_v3 *text_input,
        uint32_t action, uint32_t serial)
{ (void)data; (void)text_input; (void)action; (void)serial; }

static void text_input_language(void *data, struct zwp_text_input_v3 *text_input,
        const char *language)
{ (void)data; (void)text_input; (void)language; }

static void text_input_preedit_hint(void *data, struct zwp_text_input_v3 *text_input,
        uint32_t start, uint32_t end, uint32_t hint)
{ (void)data; (void)text_input; (void)start; (void)end; (void)hint; }

const struct zwp_text_input_v3_listener text_input_listener = {
    .action = text_input_action,
    .language = text_input_language,
    .preedit_hint = text_input_preedit_hint,
    .enter = text_input_enter,
    .leave = text_input_leave,
    .preedit_string = text_input_preedit_string,
    .commit_string = text_input_commit_string,
    .delete_surrounding_text = text_input_delete_surrounding_text,
    .done = text_input_done,
};
