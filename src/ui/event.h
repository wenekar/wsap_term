#ifndef WSAP_UI_EVENT_H
#define WSAP_UI_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    enum ui_pointer_kind kind;
    struct vector2i pos;
    int button;
    int scroll_lines;
    float scroll_delta;
    unsigned mods;
    enum pointer_device device;
    struct pointer_pen pen;
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
};

struct ui_event {
    enum ui_event_kind kind;
    uint64_t time_ms;
    union {
        struct ui_pointer_event pointer;
        struct ui_key_event key;
        struct { const char *data; size_t len; } text;
        struct { int action; enum ui_edit_command edit; } shortcut;
        bool window_focused;
    } data;
};

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
