#define _GNU_SOURCE
#include "ui/yasl/native_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input/keybind.h"
#include "loop/latency_probe.h"
#include "ui/yasl/internal.h"

static enum input_key native_key(enum yasl_key key)
{
    if (key >= YASL_KEY_F1 && key <= YASL_KEY_F25)
        return (enum input_key)(INPUT_KEY_F1 + (key - YASL_KEY_F1));
    if (key >= YASL_KEY_KP_0 && key <= YASL_KEY_KP_9)
        return (enum input_key)(INPUT_KEY_KP_0 + (key - YASL_KEY_KP_0));

    switch (key) {
    case YASL_KEY_ENTER:         return INPUT_KEY_RETURN;
    case YASL_KEY_TAB:           return INPUT_KEY_TAB;
    case YASL_KEY_BACKSPACE:     return INPUT_KEY_BACKSPACE;
    case YASL_KEY_ESCAPE:        return INPUT_KEY_ESCAPE;
    case YASL_KEY_INSERT:        return INPUT_KEY_INSERT;
    case YASL_KEY_DELETE:        return INPUT_KEY_DELETE;
    case YASL_KEY_UP:            return INPUT_KEY_UP;
    case YASL_KEY_DOWN:          return INPUT_KEY_DOWN;
    case YASL_KEY_RIGHT:         return INPUT_KEY_RIGHT;
    case YASL_KEY_LEFT:          return INPUT_KEY_LEFT;
    case YASL_KEY_HOME:          return INPUT_KEY_HOME;
    case YASL_KEY_END:           return INPUT_KEY_END;
    case YASL_KEY_PAGE_UP:       return INPUT_KEY_PAGE_UP;
    case YASL_KEY_PAGE_DOWN:     return INPUT_KEY_PAGE_DOWN;

    case YASL_KEY_KP_DECIMAL:    return INPUT_KEY_KP_DECIMAL;
    case YASL_KEY_KP_DIVIDE:     return INPUT_KEY_KP_DIVIDE;
    case YASL_KEY_KP_MULTIPLY:   return INPUT_KEY_KP_MULTIPLY;
    case YASL_KEY_KP_SUBTRACT:   return INPUT_KEY_KP_SUBTRACT;
    case YASL_KEY_KP_ADD:        return INPUT_KEY_KP_ADD;
    case YASL_KEY_KP_ENTER:      return INPUT_KEY_KP_ENTER;
    case YASL_KEY_KP_EQUAL:      return INPUT_KEY_KP_EQUAL;
    case YASL_KEY_KP_COMMA:      return INPUT_KEY_KP_SEPARATOR;

    case YASL_KEY_CAPS_LOCK:     return INPUT_KEY_CAPS_LOCK;
    case YASL_KEY_SCROLL_LOCK:   return INPUT_KEY_SCROLL_LOCK;
    case YASL_KEY_NUM_LOCK:      return INPUT_KEY_NUM_LOCK;
    case YASL_KEY_PRINT_SCREEN:  return INPUT_KEY_PRINT_SCREEN;
    case YASL_KEY_PAUSE:         return INPUT_KEY_PAUSE;
    case YASL_KEY_MENU:          return INPUT_KEY_MENU;
    case YASL_KEY_VOLUME_UP:     return INPUT_KEY_VOLUME_UP;
    case YASL_KEY_VOLUME_DOWN:   return INPUT_KEY_VOLUME_DOWN;
    case YASL_KEY_MUTE:          return INPUT_KEY_MUTE;

    case YASL_KEY_LEFT_SHIFT:    return INPUT_KEY_LSHIFT;
    case YASL_KEY_LEFT_CONTROL:  return INPUT_KEY_LCTRL;
    case YASL_KEY_LEFT_ALT:      return INPUT_KEY_LALT;
    case YASL_KEY_LEFT_SUPER:    return INPUT_KEY_LSUPER;
    case YASL_KEY_RIGHT_SHIFT:   return INPUT_KEY_RSHIFT;
    case YASL_KEY_RIGHT_CONTROL: return INPUT_KEY_RCTRL;
    case YASL_KEY_RIGHT_ALT:     return INPUT_KEY_RALT;
    case YASL_KEY_RIGHT_SUPER:   return INPUT_KEY_RSUPER;

    default: return INPUT_KEY_NONE;
    }
}

static bool is_keypad(enum yasl_key key)
{
    return key >= YASL_KEY_KP_0 && key <= YASL_KEY_KP_CLEAR;
}

static uint32_t us_base(enum yasl_key key)
{
    if (key >= YASL_KEY_A && key <= YASL_KEY_Z)
        return 'a' + (key - YASL_KEY_A);
    if (key >= YASL_KEY_0 && key <= YASL_KEY_9)
        return '0' + (key - YASL_KEY_0);
    switch (key) {
    case YASL_KEY_SPACE:         return ' ';
    case YASL_KEY_APOSTROPHE:    return '\'';
    case YASL_KEY_COMMA:         return ',';
    case YASL_KEY_MINUS:         return '-';
    case YASL_KEY_PERIOD:        return '.';
    case YASL_KEY_SLASH:         return '/';
    case YASL_KEY_SEMICOLON:     return ';';
    case YASL_KEY_EQUAL:         return '=';
    case YASL_KEY_LEFT_BRACKET:  return '[';
    case YASL_KEY_BACKSLASH:     return '\\';
    case YASL_KEY_RIGHT_BRACKET: return ']';
    case YASL_KEY_GRAVE:         return '`';
    default:                     return 0;
    }
}

static uint32_t typed_label(const struct yasl_event *in)
{
    bool letter = in->key.key >= YASL_KEY_A && in->key.key <= YASL_KEY_Z;
    bool caps = letter && (in->mods & YASL_MOD_CAPS);
    bool shift = (in->mods & YASL_MOD_SHIFT) != 0;
    return (shift != caps) && in->key.shifted ? in->key.shifted : in->key.codepoint;
}

static enum input_action action_of(unsigned action)
{
    switch (action) {
    case YASL_KEY_PRESS:   return INPUT_PRESS;
    case YASL_KEY_REPEAT:  return INPUT_REPEAT;
    case YASL_KEY_RELEASE: return INPUT_RELEASE;
    }
    return INPUT_ACTION_NONE;
}

int input_modifiers_from_yasl(uint32_t mods)
{
    unsigned out = mods & (YASL_MOD_SHIFT | YASL_MOD_ALT | YASL_MOD_CTRL | YASL_MOD_SUPER);
    if (mods & YASL_MOD_CAPS)
        out |= INPUT_MOD_CAPS;
    return (int)out;
}

static enum pointer_kind pointer_kind_of(unsigned action)
{
    switch (action) {
    case YASL_POINTER_DOWN:  return PTR_PRESS;
    case YASL_POINTER_UP:    return PTR_RELEASE;
    case YASL_POINTER_MOVE:  return PTR_MOTION;
    case YASL_POINTER_LEAVE: return PTR_LEAVE;
    }
    return PTR_MOTION;
}

static enum pointer_button button_of(unsigned button)
{
    switch (button) {
    case YASL_BUTTON_PRIMARY:   return POINTER_BUTTON_PRIMARY;
    case YASL_BUTTON_MIDDLE:    return POINTER_BUTTON_MIDDLE;
    case YASL_BUTTON_SECONDARY: return POINTER_BUTTON_SECONDARY;
    case YASL_BUTTON_BACK:      return POINTER_BUTTON_EXTRA_1;
    case YASL_BUTTON_FORWARD:   return POINTER_BUTTON_EXTRA_2;
    }
    return POINTER_BUTTON_NONE;
}

static enum pointer_button event_button(struct window *w, const struct yasl_event *in)
{
    if (in->action == YASL_POINTER_DOWN) {
        w->button_held = true;
        w->held_button = button_of(in->pointer.button);
        return w->held_button;
    }
    if (in->action == YASL_POINTER_UP) {
        enum pointer_button released = button_of(in->pointer.button);
        if (w->button_held && w->held_button == released)
            w->button_held = false;
        return released;
    }
    return w->button_held ? w->held_button : POINTER_BUTTON_NONE;
}

static void queue_key(struct window *w, const struct yasl_event *in)
{
    uint32_t label = typed_label(in);
    struct input_key_event ev = {
        .key = native_key(in->key.key),
        .keypad = is_keypad(in->key.key) ? native_key(in->key.key) : INPUT_KEY_NONE,
        .action = action_of(in->action),
        .mods = (unsigned)input_modifiers_from_yasl(in->mods),
        .codepoint = in->key.codepoint,
        .shifted = in->key.shifted,
        .base = us_base(in->key.key),
        .text = label,
        .text_deferred = label != 0 && !(in->mods & YASL_MOD_ALT),
    };

    unsigned key_id = ev.key != INPUT_KEY_NONE ? (unsigned)ev.key
                                               : 512u + (ev.base < 256 ? ev.base : 0);
    enum input_shortcut sc;
    switch (input_shortcut_route_key(input_keymap_active(), &w->input, &w->sc_state,
                                     &ev, key_id, &sc)) {
    case INPUT_KEY_PASSTHROUGH:
        break;
    case INPUT_KEY_DROPPED:
        fprintf(stderr, "shortcut queue is full\n");
        return;
    default:
        return;
    }
    input_queue_add(&w->input, &(struct input_event){ .kind = INPUT_KEY, .key = ev });
}

static struct vector2i device_pos(const struct window *w, const struct yasl_event *in)
{
    double scale = window_scale_120(w) / 120.0;
    return (struct vector2i){ .x = (int)lround(in->x * scale), .y = (int)lround(in->y * scale) };
}

static int popup_of(const struct window *w, const struct yasl_surface *s)
{
    const struct window_native *native = s ? yasl_surface_get_userdata(s) : NULL;
    return native && native->window == w ? native->id : 0;
}

static void queue_mouse(struct window *w, const struct yasl_event *in)
{
    struct input_event ev = {
        .kind = INPUT_MOUSE,
        .mouse = {
            .pos = device_pos(w, in),
            .button = event_button(w, in),
            .mods = (unsigned)input_modifiers_from_yasl(in->mods),
            .kind = pointer_kind_of(in->action),
            .device = POINTER_DEVICE_MOUSE,
        },
        .popup = popup_of(w, in->surface),
    };
    input_queue_add(&w->input, &ev);
}

static void queue_pen(struct window *w, const struct yasl_event *in)
{
    enum pointer_kind kind = PTR_MOTION;
    enum pointer_button btn = POINTER_BUTTON_NONE;
    if (in->action == YASL_PEN_DOWN) {
        kind = PTR_PRESS;
        btn = POINTER_BUTTON_PRIMARY;
        w->button_held = true;
        w->held_button = btn;
    } else if (in->action == YASL_PEN_UP) {
        kind = PTR_RELEASE;
        btn = POINTER_BUTTON_PRIMARY;
        if (w->button_held && w->held_button == btn)
            w->button_held = false;
    } else if (in->action == YASL_PEN_LEAVE) {
        kind = PTR_LEAVE;
    } else {
        kind = PTR_MOTION;
        btn = w->button_held ? w->held_button : POINTER_BUTTON_NONE;
    }

    struct input_event ev = {
        .kind = INPUT_MOUSE,
        .mouse = {
            .pos = device_pos(w, in),
            .button = btn,
            .kind = kind,
            .device = (in->pen.tool == YASL_PEN_ERASER) ? POINTER_DEVICE_ERASER : POINTER_DEVICE_PEN,
            .pen = {
                .pressure = (float)in->pen.pressure,
                .tilt_x = (float)in->pen.tilt_x,
                .tilt_y = (float)in->pen.tilt_y,
            },
        },
        .popup = popup_of(w, in->surface),
    };
    input_queue_add(&w->input, &ev);
}

static float wheel_lines(double offset)
{
    if (offset > INPUT_SCROLL_MAX_LINES) return INPUT_SCROLL_MAX_LINES;
    if (offset < -INPUT_SCROLL_MAX_LINES) return -INPUT_SCROLL_MAX_LINES;
    return (float)offset;
}

static void queue_wheel(struct window *w, const struct yasl_event *in,
                        float lines, enum pointer_button button)
{
    input_queue_add(&w->input, &(struct input_event){
        .kind = INPUT_MOUSE,
        .mouse = {
            .pos = device_pos(w, in),
            .button = button,
            .mods = (unsigned)input_modifiers_from_yasl(in->mods),
            .scroll_delta = lines,
            .kind = PTR_SCROLL,
            .device = POINTER_DEVICE_MOUSE,
        },
        .popup = popup_of(w, in->surface),
    });
}

static void queue_scroll(struct window *w, const struct yasl_event *in)
{
    double lines_x = in->scroll.lines_x;
    double lines_y = in->scroll.lines_y;
    if (lines_y == 0.0 && lines_x == 0.0) {
        lines_x = in->scroll.pixels_x / 20.0;
        lines_y = in->scroll.pixels_y / 20.0;
    }
    double up = -lines_y;
    double right = lines_x;
    if (up != 0.0)
        queue_wheel(w, in, wheel_lines(up), up > 0 ? POINTER_WHEEL_UP : POINTER_WHEEL_DOWN);
    if (right != 0.0)
        queue_wheel(w, in, wheel_lines(right), right > 0 ? POINTER_WHEEL_RIGHT : POINTER_WHEEL_LEFT);
}

void window_native_input(struct window *w, const struct yasl_event *in)
{
    bool key_text = w->key_text_pending;
    w->key_text_pending = false;
    switch (in->type) {
    case YASL_EVENT_KEY:
        if (in->action == YASL_KEY_PRESS)
            latency_probe_stage(LATENCY_STAGE_KEY_IN);
        w->modifiers = input_modifiers_from_yasl(in->mods);
        w->key_text_pending = in->action != YASL_KEY_RELEASE && typed_label(in) != 0;
        queue_key(w, in);
        break;
    case YASL_EVENT_POINTER:
        w->modifiers = input_modifiers_from_yasl(in->mods);
        queue_mouse(w, in);
        break;
    case YASL_EVENT_PEN:
        queue_pen(w, in);
        break;
    case YASL_EVENT_SCROLL:
        w->modifiers = input_modifiers_from_yasl(in->mods);
        queue_scroll(w, in);
        break;
    case YASL_EVENT_TEXT:
        input_queue_push_text(&w->input, in->text, in->length,
                              key_text ? INPUT_TEXT_KEY : INPUT_TEXT_COMMIT);
        break;
    case YASL_EVENT_TEXT_EDIT:
        input_queue_push_text(&w->input, in->text, in->length, INPUT_TEXT_COMMIT);
        break;
    case YASL_EVENT_CLIPBOARD:
        if (in->action == YASL_TRANSFER_OK && in->text) {
            input_queue_push_paste(&w->input, in->text, in->length);
        }
        break;
    case YASL_EVENT_DROP:
        if (in->action == YASL_DROP_RELEASE && in->text) {
            input_queue_push_drop_files(&w->input, in->text, in->length);
        } else if (w->drag_target && w->drag_target->drop_hover) {
            w->drag_target->drop_hover(w, in->action == YASL_DROP_OVER, w->drag_ud);
        }
        break;
    case YASL_EVENT_DRAG:
        if (w->drag_target && w->drag_target->drag) {
            w->drag_target->drag(w, (enum yasl_drop_action)in->action,
                                 device_pos(w, in),
                                 w->drag_ud);
        }
        break;
    case YASL_EVENT_COMPOSITION:
        free(w->ime_preedit);
        if (in->text && in->length > 0) {
            w->ime_preedit = strndup(in->text, in->length);
            w->ime_begin = (int)in->ime.begin;
            w->ime_end = (int)in->ime.end;
        } else {
            w->ime_preedit = NULL;
            w->ime_begin = 0;
            w->ime_end = 0;
        }
        window_redraw(w);
        break;
    case YASL_EVENT_INPUT_RESET:
        input_keyboard_reset(&w->keyboard);
        w->button_held = false;
        break;
    default:
        break;
    }
}
