#ifndef INPUT_H
#define INPUT_H

#include "util/text_limit.h"
#include "input/pointer.h"
#include "util/geom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct input_keymap;

enum input_key {
    INPUT_KEY_NONE = 0,

    INPUT_KEY_RETURN,
    INPUT_KEY_BACKSPACE,
    INPUT_KEY_TAB,
    INPUT_KEY_ESCAPE,

    INPUT_KEY_UP,
    INPUT_KEY_DOWN,
    INPUT_KEY_RIGHT,
    INPUT_KEY_LEFT,
    INPUT_KEY_HOME,
    INPUT_KEY_END,
    INPUT_KEY_PAGE_UP,
    INPUT_KEY_PAGE_DOWN,
    INPUT_KEY_INSERT,
    INPUT_KEY_DELETE,

    INPUT_KEY_F1,  INPUT_KEY_F2,  INPUT_KEY_F3,  INPUT_KEY_F4,  INPUT_KEY_F5,
    INPUT_KEY_F6,  INPUT_KEY_F7,  INPUT_KEY_F8,  INPUT_KEY_F9,  INPUT_KEY_F10,
    INPUT_KEY_F11, INPUT_KEY_F12, INPUT_KEY_F13, INPUT_KEY_F14, INPUT_KEY_F15,
    INPUT_KEY_F16, INPUT_KEY_F17, INPUT_KEY_F18, INPUT_KEY_F19, INPUT_KEY_F20,
    INPUT_KEY_F21, INPUT_KEY_F22, INPUT_KEY_F23, INPUT_KEY_F24, INPUT_KEY_F25,
    INPUT_KEY_F26, INPUT_KEY_F27, INPUT_KEY_F28, INPUT_KEY_F29, INPUT_KEY_F30,
    INPUT_KEY_F31, INPUT_KEY_F32, INPUT_KEY_F33, INPUT_KEY_F34, INPUT_KEY_F35,

    INPUT_KEY_KP_0, INPUT_KEY_KP_1, INPUT_KEY_KP_2, INPUT_KEY_KP_3, INPUT_KEY_KP_4,
    INPUT_KEY_KP_5, INPUT_KEY_KP_6, INPUT_KEY_KP_7, INPUT_KEY_KP_8, INPUT_KEY_KP_9,
    INPUT_KEY_KP_DECIMAL,
    INPUT_KEY_KP_ENTER,
    INPUT_KEY_KP_ADD,
    INPUT_KEY_KP_SUBTRACT,
    INPUT_KEY_KP_MULTIPLY,
    INPUT_KEY_KP_DIVIDE,
    INPUT_KEY_KP_EQUAL,
    INPUT_KEY_KP_SEPARATOR,
    INPUT_KEY_KP_LEFT,
    INPUT_KEY_KP_RIGHT,
    INPUT_KEY_KP_UP,
    INPUT_KEY_KP_DOWN,
    INPUT_KEY_KP_PAGE_UP,
    INPUT_KEY_KP_PAGE_DOWN,
    INPUT_KEY_KP_HOME,
    INPUT_KEY_KP_END,
    INPUT_KEY_KP_INSERT,
    INPUT_KEY_KP_DELETE,
    INPUT_KEY_KP_BEGIN,

    INPUT_KEY_CAPS_LOCK,
    INPUT_KEY_SCROLL_LOCK,
    INPUT_KEY_NUM_LOCK,
    INPUT_KEY_PRINT_SCREEN,
    INPUT_KEY_PAUSE,
    INPUT_KEY_MENU,

    INPUT_KEY_MEDIA_PLAY,
    INPUT_KEY_MEDIA_PAUSE,
    INPUT_KEY_MEDIA_STOP,
    INPUT_KEY_MEDIA_FORWARD,
    INPUT_KEY_MEDIA_REWIND,
    INPUT_KEY_MEDIA_NEXT,
    INPUT_KEY_MEDIA_PREV,
    INPUT_KEY_MEDIA_RECORD,
    INPUT_KEY_VOLUME_DOWN,
    INPUT_KEY_VOLUME_UP,
    INPUT_KEY_MUTE,

    INPUT_KEY_LSHIFT,
    INPUT_KEY_LCTRL,
    INPUT_KEY_LALT,
    INPUT_KEY_LSUPER,
    INPUT_KEY_LHYPER,
    INPUT_KEY_LMETA,
    INPUT_KEY_RSHIFT,
    INPUT_KEY_RCTRL,
    INPUT_KEY_RALT,
    INPUT_KEY_RSUPER,
    INPUT_KEY_RHYPER,
    INPUT_KEY_RMETA,
    INPUT_KEY_ISO_LEVEL3_SHIFT,
    INPUT_KEY_ISO_LEVEL5_SHIFT,

    INPUT_KEY_COUNT
};

enum input_mod {
    INPUT_MOD_SHIFT = 1,
    INPUT_MOD_ALT   = 2,
    INPUT_MOD_CTRL  = 4,
    INPUT_MOD_SUPER = 8,
    INPUT_MOD_HYPER = 16,
    INPUT_MOD_META  = 32,
    INPUT_MOD_CAPS  = 64,
    INPUT_MOD_NUM   = 128,
};

enum input_action {
    INPUT_ACTION_NONE = 0,
    INPUT_PRESS       = 1,
    INPUT_REPEAT      = 2,
    INPUT_RELEASE     = 3,
};

struct input_key_event {
    enum input_key key;

    enum input_key keypad;

    enum input_action action;
    unsigned mods;

    uint32_t codepoint;
    uint32_t shifted;
    uint32_t base;
    uint32_t text;
    /* Queued events own these UTF-8 bytes. */
    char *text_utf8;
    size_t text_len;
    /* Keep the raw modifiers for shortcuts. */
    unsigned consumed_mods;
    bool text_deferred;
    bool ime_handled;
};

struct input_mouse_event {
    struct vector2i pos;

    int button;
    unsigned mods;
    int scroll_lines;
    float scroll_delta;
    enum pointer_kind kind;
    enum pointer_device device;
    struct pointer_pen pen;
    int popup;
};

enum input_shortcut {
    INPUT_SHORTCUT_NONE = 0,
    INPUT_SHORTCUT_FONT_LARGER,
    INPUT_SHORTCUT_FONT_SMALLER,
    INPUT_SHORTCUT_COPY,
    INPUT_SHORTCUT_PASTE,
    INPUT_SHORTCUT_SELECT_ALL,
    INPUT_SHORTCUT_SCROLL_UP,
    INPUT_SHORTCUT_SCROLL_DOWN,
    INPUT_SHORTCUT_NEW_TAB,
    INPUT_SHORTCUT_CLOSE_TAB,
    INPUT_SHORTCUT_NEXT_TAB,
    INPUT_SHORTCUT_PREV_TAB,
    INPUT_SHORTCUT_LAST_TAB,

    INPUT_SHORTCUT_TAB_1,
    INPUT_SHORTCUT_TAB_2,
    INPUT_SHORTCUT_TAB_3,
    INPUT_SHORTCUT_TAB_4,
    INPUT_SHORTCUT_TAB_5,
    INPUT_SHORTCUT_TAB_6,
    INPUT_SHORTCUT_TAB_7,
    INPUT_SHORTCUT_TAB_8,
    INPUT_SHORTCUT_KILL_LINE,
    INPUT_SHORTCUT_COMMAND_PALETTE,
    INPUT_SHORTCUT_DEBUG_OVERLAY,
    INPUT_SHORTCUT_SETTINGS,
    INPUT_SHORTCUT_FOCUS_TABS,
    INPUT_SHORTCUT_COUNT,
};

enum input_text_source : uint8_t {
    INPUT_TEXT_COMMIT = 0,
    INPUT_TEXT_KEY,
    INPUT_TEXT_SHORTCUT,
};

enum input_kind {
    INPUT_NONE = 0,
    INPUT_KEY,
    INPUT_TEXT,
    INPUT_MOUSE,
    INPUT_FOCUS,
    INPUT_PASTE,
    INPUT_DROP_FILES,
    INPUT_SHORTCUT,
};

struct input_data {
    char *data;
    size_t len;
};

struct input_event {
    union {
        struct input_key_event key;
        struct input_mouse_event mouse;
        struct { bool focused; } focus;

        struct {
            char utf8[48];
            uint8_t len;
            enum input_text_source source;
        } text;

        struct input_data paste;
        struct input_data drop;
        struct { enum input_shortcut action; } shortcut;
    };
    enum input_kind kind;
};

_Static_assert(sizeof(struct input_event) == 64,
               "input_event should be 64 bytes (single cacheline)");

#define INPUT_QUEUE_SIZE 256

struct input_queue {
    struct input_event ev[INPUT_QUEUE_SIZE];
    unsigned head, tail;
    size_t text_bytes;
};

enum input_push_result {
    INPUT_PUSH_OK = 0,
    INPUT_PUSH_DROPPED,
    INPUT_PUSH_EVICTED_MOTION,
};

enum input_push_result input_queue_add(struct input_queue *q,
                                        const struct input_event *ev);

size_t input_queue_push_drop_files(struct input_queue *q, const char *uris,
                                  size_t len);
size_t input_queue_push_paste(struct input_queue *q, const char *data,
                              size_t len);

#define INPUT_SCROLL_MAX_LINES 15

static inline int input_scroll_clamp(int lines)
{
    if (lines > INPUT_SCROLL_MAX_LINES) return INPUT_SCROLL_MAX_LINES;
    if (lines < -INPUT_SCROLL_MAX_LINES) return -INPUT_SCROLL_MAX_LINES;
    return lines;
}

size_t input_queue_push_text(struct input_queue *q, const char *text,
                           size_t len, enum input_text_source source);

bool input_queue_take(struct input_queue *q, struct input_event *out);

void input_queue_reset(struct input_queue *q);

void input_event_release(struct input_event *ev);

struct input_keyboard_state {
    uint64_t down[(INPUT_KEY_COUNT + 63) / 64];
};

void input_keyboard_reset(struct input_keyboard_state *s);
void input_keyboard_update(struct input_keyboard_state *s,
                           const struct input_key_event *ev);

static inline bool input_keyboard_key_down(
        const struct input_keyboard_state *s, enum input_key key)
{
    return s && key > INPUT_KEY_NONE && (unsigned)key < INPUT_KEY_COUNT &&
           ((s->down[key >> 6] >> (key & 63)) & 1);
}

struct input_shortcut_policy {
    unsigned accel;
    unsigned zoom;
    unsigned accel_alt;
    unsigned tab_accel;
};

extern const struct input_shortcut_policy input_platform_shortcuts;

#define INPUT_SHORTCUT_KEY_COUNT 768

struct input_shortcut_state {
    uint8_t routes[(INPUT_SHORTCUT_KEY_COUNT + 1) / 2];
};

enum input_key_route {
    INPUT_KEY_SWALLOWED = 0,
    INPUT_KEY_SHORTCUT,
    INPUT_KEY_PASSTHROUGH,
    INPUT_KEY_DROPPED,
};

enum input_key_route input_shortcut_route_key(const struct input_keymap *km,
        struct input_queue *q,
        struct input_shortcut_state *st,
        const struct input_key_event *key, unsigned key_id,
        enum input_shortcut *out_sc);

#endif
