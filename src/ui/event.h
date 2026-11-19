#ifndef WSAP_UI_EVENT_H
#define WSAP_UI_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "input/pointer.h"
#include "ui/keys.h"
#include "util/geom.h"

enum ui_pointer_kind : uint8_t {
    UI_POINTER_NONE = 0,
    UI_POINTER_PRESS,
    UI_POINTER_RELEASE,
    UI_POINTER_MOTION,
    UI_POINTER_SCROLL,
    UI_POINTER_LEAVE,
};

#define UI_POINTER_WHEEL_BUTTON POINTER_WHEEL_UP

struct ui_pointer_event {
    struct vector2i pos;
    int button;
    int scroll_lines;
    float scroll_delta;
    unsigned mods;
    struct pointer_pen pen;
    enum ui_pointer_kind kind;
    enum pointer_device device;
};

static inline bool ui_pointer_is_button_press(const struct ui_pointer_event *p)
{
    return p->kind == UI_POINTER_PRESS && p->button < UI_POINTER_WHEEL_BUTTON;
}

static inline bool ui_pointer_is_horizontal_wheel(const struct ui_pointer_event *p)
{
    return p->button == UI_POINTER_WHEEL_BUTTON + 2 ||
           p->button == UI_POINTER_WHEEL_BUTTON + 3;
}

enum ui_event_kind : uint8_t {
    UI_EVENT_NONE = 0,
    UI_EVENT_POINTER,
    UI_EVENT_KEY,
    UI_EVENT_TEXT,
    UI_EVENT_PASTE,
    UI_EVENT_FOCUS_IN,
    UI_EVENT_FOCUS_OUT,
    UI_EVENT_CAPTURE_LOST,
    UI_EVENT_SHORTCUT,
    UI_EVENT_DROP_FILES,
    UI_EVENT_WINDOW_FOCUS,
    /* A drag came over a widget, moved over it, or left it. The event holds
     * surface coordinates and the offered types. */
    UI_EVENT_DRAG_ENTER,
    UI_EVENT_DRAG_OVER,
    UI_EVENT_DRAG_LEAVE,
    /* The drag ended on the widget. The event holds the type that the target
     * asked for, and its payload. */
    UI_EVENT_DROP,
    /* Input method preedit. `begin` and `end` are the byte offsets of its
     * cursor, and `revision` names the text that the host sent. */
    UI_EVENT_COMPOSITION,
    /* The input method replaced the bytes `begin`..`end` of the text that
     * the host sent. `revision` is the revision of the replaced text. */
    UI_EVENT_TEXT_EDIT,
};

/* The preedit or the text edit of an input method. `data` is UTF-8 and is
 * not NUL terminated. */
struct ui_ime_text_event {
    const char *data;
    size_t len;
    int begin;
    int end;
    uint64_t revision;
};

/* The longest type that a drag carries. yasl uses the same limit, so a type
 * that fits here fits a native drop too. */
#define UI_DRAG_MIME_MAX 64

/* The source of a drag. A target asks it for a type, and it answers with the
 * data of that type or with a refusal. A drag from outside the app has no
 * source: the native layer already holds the data of the one type that the
 * surface accepts. */
struct ui_drop_source {
    /* Returns true and sets `data` and `len` when the source has type
     * `mime`. Returns false to refuse the type. The data stays valid until
     * the drag ends. */
    bool (*read)(void *userdata, const char *mime, const char **data, size_t *len);
    void *userdata;
};

/* One drag over one surface. `pos` is the pointer position in surface
 * coordinates. `mimes` lists the offered types, best first, and `mime_count`
 * counts them. The target writes `mime` to ask for a type. `data` is the
 * payload of that type, or NULL when the source refused it. */
struct ui_drag_event {
    const char *const *mimes;
    size_t mime_count;
    const char *mime;
    const char *data;
    size_t len;
    const struct ui_drop_source *source;
    struct vector2i pos;
};

/* Returns true when the drag offers type `mime`. An offer with a star for
 * its subtype, such as "image/" "*", matches every type with that major
 * type. An offer of "*" matches every type. */
static inline bool ui_drag_offers(const struct ui_drag_event *drag, const char *mime)
{
    if (!drag || !mime)
        return false;
    for (size_t i = 0; i < drag->mime_count; i++) {
        const char *offer = drag->mimes[i];
        if (!offer)
            continue;
        if (!strcmp(offer, mime))
            return true;
        if (offer[0] == '*' && !offer[1])
            return true;
        size_t m = strlen(offer);
        if (m >= 2 && offer[m - 1] == '*' && offer[m - 2] == '/') {
            size_t n = strlen(mime);
            if (n > m - 1 && !strncmp(offer, mime, m - 1))
                return true;
        }
    }
    return false;
}

/* Asks the source of the drag for the type `mime`. A target calls it from a
 * DRAG_ENTER, DRAG_OVER, or DROP handler, and writes the type to `mime` so the
 * payload travels with the DROP. Returns true when the source has the type;
 * then `data` and `len` hold its payload. Returns false on a refusal, and the
 * target then takes no drop. */
static inline bool ui_drag_event_read(const struct ui_drag_event *drag,
                                      const char *mime, const char **data,
                                      size_t *len)
{
    if (!drag || !mime || !drag->source || !drag->source->read)
        return false;
    const char *out = NULL;
    size_t size = 0;
    if (!drag->source->read(drag->source->userdata, mime, &out, &size) || !out)
        return false;
    if (data)
        *data = out;
    if (len)
        *len = size;
    return true;
}

struct ui_event {
    uint64_t time_ms;
    union {
        struct ui_pointer_event pointer;
        struct ui_key_event key;
        struct { const char *data; size_t len; } text;
        struct { int action; enum ui_edit_command edit; } shortcut;
        struct ui_ime_text_event ime;
        struct ui_drag_event drag;
        bool window_focused;
    } data;
    enum ui_event_kind kind;
};

/* How a widget receives an event.
 *
 * The surface gives an event to one widget at a time. The event op of the
 * widget runs first. The handler that ui_widget_set_event_handler sets runs
 * next, when the op returns false. A widget is thus one widget to both ways of
 * handling an event: an event op and a handler see the same events.
 *
 * An event bubbles to the parents of its target while each widget returns
 * false. A hover leave is the exception. It goes to the one widget that lost
 * the pointer and does not bubble. So a parent gets no hover state for a child
 * that the pointer left.
 */

static inline struct ui_event ui_event_pointer(struct ui_pointer_event event,
                                                uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_POINTER,
        .time_ms = time_ms,
        .data.pointer = event,
    };
}

static inline struct ui_event ui_event_key(struct ui_key_event event,
                                            uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_KEY,
        .time_ms = time_ms,
        .data.key = event,
    };
}

static inline struct ui_event ui_event_focus(bool focused, uint64_t time_ms)
{
    return (struct ui_event){
        .kind = focused ? UI_EVENT_FOCUS_IN : UI_EVENT_FOCUS_OUT,
        .time_ms = time_ms,
    };
}

static inline struct ui_event ui_event_text(const char *data, size_t len,
                                            uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_TEXT,
        .time_ms = time_ms,
        .data.text = { .data = data, .len = len },
    };
}

static inline struct ui_event ui_event_paste(const char *data, size_t len,
                                             uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_PASTE,
        .time_ms = time_ms,
        .data.text = { .data = data, .len = len },
    };
}

static inline struct ui_event ui_event_shortcut(int action,
                                                enum ui_edit_command edit,
                                                uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_SHORTCUT,
        .time_ms = time_ms,
        .data.shortcut = { .action = action, .edit = edit },
    };
}

static inline struct ui_event ui_event_drop_files(const char *data, size_t len,
                                                  uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_DROP_FILES,
        .time_ms = time_ms,
        .data.text = { .data = data, .len = len },
    };
}

static inline struct ui_event ui_event_composition(
    const struct ui_ime_text_event *text, uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_COMPOSITION,
        .time_ms = time_ms,
        .data.ime = *text,
    };
}

static inline struct ui_event ui_event_text_edit(
    const struct ui_ime_text_event *text, uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_TEXT_EDIT,
        .time_ms = time_ms,
        .data.ime = *text,
    };
}

static inline struct ui_event ui_event_drag(enum ui_event_kind kind,
                                            const struct ui_drag_event *drag,
                                            uint64_t time_ms)
{
    return (struct ui_event){
        .kind = kind,
        .time_ms = time_ms,
        .data.drag = *drag,
    };
}

static inline struct ui_event ui_event_window_focus(bool focused,
                                                    uint64_t time_ms)
{
    return (struct ui_event){
        .kind = UI_EVENT_WINDOW_FOCUS,
        .time_ms = time_ms,
        .data.window_focused = focused,
    };
}

#endif
