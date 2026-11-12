#include "internal.h"
#include "yasl/yasl.h"

#include <errno.h>

#define IME_TEXT_MAX (256u * 1024)

static void show_preedit(struct wayland_surface *w, char *text, int begin, int end,
                         uint64_t revision, uint64_t generation)
{
    bool changed = (text || w->ime_shown) &&
                   (!text || !w->ime_shown || strcmp(text, w->ime_shown) ||
                    begin != w->ime_shown_begin || end != w->ime_shown_end ||
                    revision != w->ime_shown_revision || generation != w->ime_shown_generation);
    free(w->ime_shown);
    w->ime_shown = text;
    w->ime_shown_begin = begin;
    w->ime_shown_end = end;
    w->ime_shown_revision = revision;
    w->ime_shown_generation = generation;
    if (changed) {
        yasl_private_surface_notify_composition_at(&w->base, w->ime_shown,
                w->ime_shown_begin, w->ime_shown_end, revision, generation);
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
    show_preedit(w, NULL, -1, -1, w->base.text_revision, w->base.text_generation);
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

static void ime_barrier_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)serial;
    struct seat *seat = data;
    wl_callback_destroy(callback);
    seat->ime_barrier = NULL;
    uint32_t retained = seat->ime_serial - seat->ime_barrier_serial;
    for (size_t i = 0; i < IME_SNAPSHOT_COUNT; i++) {
        struct ime_snapshot *snapshot = &seat->ime_snapshots[i];
        if (snapshot->valid && snapshot->serial - seat->ime_barrier_serial > retained)
            snapshot->valid = false;
    }
    seat->ime_history_count = retained + 1;
    if (seat->ime_deferred && !seat->ime_wait_matching && seat->ime_focus) {
        seat->ime_deferred = false;
        wayland_surface_update_text_input(seat->ime_focus);
    }
}

static const struct wl_callback_listener ime_barrier_listener = {
    .done = ime_barrier_done,
};

static void ime_record_commit(struct seat *seat, struct wayland_surface *w,
                              const char *text, size_t start, size_t length)
{
    uint32_t serial = ++seat->ime_serial;
    seat->ime_history_count++;
    if (!seat->ime_snapshots) return;
    struct ime_snapshot *snapshot = &seat->ime_snapshots[serial % IME_SNAPSHOT_COUNT];
    *snapshot = (struct ime_snapshot){.serial = serial};
    if (!w || !w->base.text_surrounding) return;
    snapshot->valid = true;
    snapshot->surface = w;
    snapshot->revision = w->base.text_revision;
    snapshot->generation = w->base.text_generation;
    snapshot->begin = w->base.text_begin;
    snapshot->end = w->base.text_end;
    if (text) {
        snapshot->surrounding = true;
        snapshot->offset = start;
        snapshot->length = length;
        snapshot->cursor = w->base.text_end - start;
        memcpy(snapshot->text, text, length);
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
    if (!seat->ime_snapshots) {
        seat->ime_snapshots = calloc(IME_SNAPSHOT_COUNT, sizeof(*seat->ime_snapshots));
        if (!seat->ime_snapshots) { errno = ENOMEM; return false; }
    }
    if (!w->base.text_input_active) {
        seat->ime_wait_matching = false;
        seat->ime_deferred = false;
        if (seat->ime_enabled) {
            zwp_text_input_v3_disable(seat->text_input);
            zwp_text_input_v3_commit(seat->text_input);
            ime_record_commit(seat, NULL, NULL, 0, 0);
            seat->ime_enabled = false;
        }
        ime_reset(w);
        return true;
    }
    if (!seat->ime_barrier && seat->ime_history_count >= IME_SNAPSHOT_COUNT - 4) {
        seat->ime_barrier = wl_display_sync(seat->conn->wl);
        if (!seat->ime_barrier) { errno = ENOMEM; return false; }
        seat->ime_barrier_serial = seat->ime_serial;
        if (wl_callback_add_listener(seat->ime_barrier, &ime_barrier_listener, seat) < 0) {
            wl_callback_destroy(seat->ime_barrier);
            seat->ime_barrier = NULL;
            errno = ENOMEM;
            return false;
        }
    }
    /* The protocol requires a matching done before pending state is sent. */
    if (seat->ime_barrier || seat->ime_wait_matching) {
        seat->ime_deferred = true;
        return true;
    }

    size_t selection = w->base.text_end - w->base.text_begin;
    if (selection > 4000 && seat->ime_enabled && !seat->ime_no_surrounding) {
        zwp_text_input_v3_disable(seat->text_input);
        zwp_text_input_v3_commit(seat->text_input);
        ime_record_commit(seat, NULL, NULL, 0, 0);
        seat->ime_enabled = false;
    }
    seat->ime_no_surrounding = selection > 4000;
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
    size_t window_start = 0, window_length = 0;
    char window[4001];
    /* The protocol limits surrounding text to 4000 bytes. */
    if (selection <= 4000) {
        size_t first = cursor < anchor ? cursor : anchor;
        size_t last = cursor > anchor ? cursor : anchor;
        size_t extra = (4000 - (last - first)) / 2;
        size_t start = first > extra ? first - extra : 0;
        while (start < first && ((unsigned char)surrounding[start] & 0xc0) == 0x80)
            start++;
        size_t end = length - start > 4000 ? start + 4000 : length;
        while (end > last && end < length &&
               ((unsigned char)surrounding[end] & 0xc0) == 0x80)
            end--;
        window_start = start;
        window_length = end - start;
        memcpy(window, surrounding + start, end - start);
        window[end - start] = '\0';
        zwp_text_input_v3_set_surrounding_text(seat->text_input, window,
                (int32_t)(cursor - start), (int32_t)(anchor - start));
    }
    uint32_t scale = w->scale_120 ? w->scale_120 : YASL_PRIVATE_SCALE_UNIT;
    zwp_text_input_v3_set_cursor_rectangle(seat->text_input,
            yasl_private_scale_to_logical(w->ime_caret.x, scale),
            yasl_private_scale_to_logical(w->ime_caret.y, scale),
            yasl_private_scale_to_logical(w->ime_caret.width, scale),
            yasl_private_scale_to_logical(w->ime_caret.height, scale));
    zwp_text_input_v3_commit(seat->text_input);
    ime_record_commit(seat, w, selection <= 4000 ? window : NULL,
                      window_start, window_length);
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
    wayland_surface_update_text_input(w);
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
    s->ime_wait_matching = false;
    s->ime_deferred = false;
    if (!w) return;
    if (s->ime_enabled) {
        zwp_text_input_v3_disable(text_input);
        zwp_text_input_v3_commit(text_input);
        ime_record_commit(s, NULL, NULL, 0, 0);
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
    bool shown = cursor_begin >= 0 && cursor_end >= 0 &&
                 cursor_begin <= len && cursor_end <= len;
    if (shown && cursor_end < cursor_begin) {
        int32_t tmp = cursor_begin;
        cursor_begin = cursor_end;
        cursor_end = tmp;
    }
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
    if (!w || !w->base.text_input_active)
        return;
    w->ime_delete_pending = true;
    w->ime_delete_before = before_length;
    w->ime_delete_after = after_length;
}

static void text_input_done(void *data, struct zwp_text_input_v3 *text_input,
                            uint32_t serial)
{
    (void)text_input;
    struct seat *seat = data;
    struct wayland_surface *w = seat->ime_focus;
    if (!w || !w->base.text_input_active) return;
    if (seat->text_input) seat->ime_wait_matching = serial != seat->ime_serial;
    struct ime_snapshot *snapshot = NULL;
    if (seat->ime_snapshots) {
        struct ime_snapshot *candidate = &seat->ime_snapshots[serial % IME_SNAPSHOT_COUNT];
        if (candidate->serial == serial && candidate->valid && candidate->surface == w)
            snapshot = candidate;
    }

    bool has_commit = w->ime_commit && w->ime_commit[0];
    bool edit = snapshot && (w->ime_delete_pending || has_commit);
    if (edit) {
        size_t begin = snapshot->begin, end = snapshot->end;
        if (w->ime_delete_pending && snapshot->surrounding) {
            size_t cursor = snapshot->cursor, length = snapshot->length;
            begin = w->ime_delete_before > cursor ? 0 : cursor - w->ime_delete_before;
            end = w->ime_delete_after > length - cursor ? length : cursor + w->ime_delete_after;
            while (begin && ((unsigned char)snapshot->text[begin] & 0xc0) == 0x80) begin--;
            while (end < length && ((unsigned char)snapshot->text[end] & 0xc0) == 0x80) end++;
            begin += snapshot->offset;
            end += snapshot->offset;
        }
        struct yasl_private_event ev = {
            .type = YASL_PRIVATE_EVENT_TEXT_EDIT,
            .surface = &w->base,
            .text_edit = {
                .data = has_commit ? w->ime_commit : "",
                .len = has_commit ? strlen(w->ime_commit) : 0,
                .revision = snapshot->revision,
                .generation = snapshot->generation,
                .begin = begin,
                .end = end,
            },
        };
        input_queue_push(&w->input, &ev);
    }
    w->ime_delete_pending = false;
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
                 preedit ? w->ime_cursor_end : -1,
                 snapshot ? snapshot->revision : w->base.text_revision,
                 snapshot ? snapshot->generation : w->base.text_generation);
    w->ime_cursor_begin = w->ime_cursor_end = -1;
    if (!seat->ime_wait_matching && !seat->ime_barrier && seat->ime_deferred) {
        seat->ime_deferred = false;
        wayland_surface_update_text_input(w);
    }
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
