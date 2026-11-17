/*
 * Step 1 of the input path.
 *
 * It turns yasl events into input events, and it queues them. Step 2 is
 * `ui_input.c`. Step 3 is `ui_route.c`.
 */

#define _GNU_SOURCE
#include "ui/yasl/native_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "input/keybind.h"
#include "loop/latency_probe.h"
#include "ui/yasl/drop.h"
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

static uint32_t fallback_key_text(const struct yasl_event *in)
{
    bool letter = input_key_is_letter(in->key.codepoint, in->key.shifted);
    bool shift = (in->mods & YASL_MOD_SHIFT) != 0;
    bool caps = (in->mods & YASL_MOD_CAPS) != 0;
    bool shifted = input_key_selects_shifted(letter, shift, caps);
    return shifted && in->key.shifted ? in->key.shifted : in->key.codepoint;
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

/* Returns the yasl mask bit of a pointer button, or 0 for a button that
 * yasl does not report. */
static uint32_t held_bit(enum pointer_button button)
{
    for (unsigned b = YASL_BUTTON_PRIMARY; b <= YASL_BUTTON_FORWARD; b++)
        if (button_of(b) == button)
            return YASL_BUTTON_BIT(b);
    return 0;
}

/* Keeps the drag button on a button that yasl still reports held. `down`
 * is the yasl mask after the event. The drag keeps its button while that
 * button stays down. Otherwise the primary button takes over, then the
 * lowest held button. Returns POINTER_BUTTON_NONE when no button is down. */
static enum pointer_button update_drag_button(struct window *w, uint32_t down)
{
    if (held_bit(w->held_button) & down)
        return w->held_button;
    w->held_button = POINTER_BUTTON_NONE;
    for (unsigned b = YASL_BUTTON_PRIMARY; b <= YASL_BUTTON_FORWARD; b++) {
        if (down & YASL_BUTTON_BIT(b)) {
            w->held_button = button_of(b);
            break;
        }
    }
    return w->held_button;
}

/* Returns the button that a pointer event reports. DOWN and UP report the
 * button that changed. Other events report the drag button. */
static enum pointer_button event_button(struct window *w, const struct yasl_event *in)
{
    enum pointer_button held = update_drag_button(w, in->pointer.buttons);
    if (in->action == YASL_POINTER_DOWN || in->action == YASL_POINTER_UP)
        return button_of(in->pointer.button);
    return held;
}

static enum input_key_route queue_key(struct window *w, const struct yasl_event *in,
                                      const char *text, size_t len)
{
    bool native_text = in->key.text_input != YASL_KEY_TEXT_UNSPECIFIED;
    uint32_t label = native_text ? 0 : fallback_key_text(in);
    struct input_key_event ev = {
        .key = native_key(in->key.key),
        .keypad = is_keypad(in->key.key) ? native_key(in->key.key) : INPUT_KEY_NONE,
        .action = action_of(in->action),
        .mods = (unsigned)input_modifiers_from_yasl(in->mods),
        .codepoint = in->key.codepoint,
        .shifted = in->key.shifted,
        .base = us_base(in->key.key),
        .text = label,
        .consumed_mods = (unsigned)input_modifiers_from_yasl(in->key.consumed_mods),
        .text_deferred = native_text || (label != 0 && !(in->mods & YASL_MOD_ALT)),
        .ime_handled = in->key.text_input == YASL_KEY_TEXT_COMPOSITION,
    };

    unsigned key_id = ev.key != INPUT_KEY_NONE ? (unsigned)ev.key
                                               : 512u + (ev.base < 256 ? ev.base : 0);
    enum input_shortcut sc;
    enum input_key_route route = ev.ime_handled ? INPUT_KEY_PASSTHROUGH :
        input_shortcut_route_key(input_keymap_active(), &w->input, &w->sc_state,
                                 &ev, key_id, &sc);
    switch (route) {
    case INPUT_KEY_PASSTHROUGH:
        break;
    case INPUT_KEY_DROPPED:
        fprintf(stderr, "shortcut queue is full\n");
        return route;
    default:
        return route;
    }
    if (len) {
        if (len > INPUT_TEXT_MAX || !(ev.text_utf8 = malloc(len)))
            return INPUT_KEY_DROPPED;
        memcpy(ev.text_utf8, text, len);
        ev.text_len = len;
    }
    if (input_queue_add(&w->input, &(struct input_event){ .kind = INPUT_KEY, .key = ev })
        == INPUT_PUSH_DROPPED) {
        free(ev.text_utf8);
        return INPUT_KEY_DROPPED;
    }
    return INPUT_KEY_PASSTHROUGH;
}

static struct vector2i device_pos(const struct window *w, const struct yasl_event *in)
{
    double scale = window_scale(w);
    return (struct vector2i){ .x = (int)lround(in->x * scale), .y = (int)lround(in->y * scale) };
}

static int popup_of(const struct window *w, const struct yasl_surface *s)
{
    for (int i = 1; i < w->native_count; i++)
        if (w->natives[i].surface == s)
            return i;
    return 0;
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
            .popup = popup_of(w, in->surface),
        },
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
        w->pen_contact = true;
    } else if (in->action == YASL_PEN_UP) {
        kind = PTR_RELEASE;
        btn = POINTER_BUTTON_PRIMARY;
        w->pen_contact = false;
    } else if (in->action == YASL_PEN_LEAVE) {
        kind = PTR_LEAVE;
    } else {
        kind = PTR_MOTION;
        btn = w->pen_contact ? POINTER_BUTTON_PRIMARY : POINTER_BUTTON_NONE;
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
            .popup = popup_of(w, in->surface),
        },
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
            .popup = popup_of(w, in->surface),
        },
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

/* Returns true when `mime` names an image type with a subtype. */
static bool mime_is_image(const char *mime)
{
    return !strncasecmp(mime, "image/", 6) && mime[6];
}

/* Returns true when `mime` names a plain text type. */
static bool mime_is_text(const char *mime)
{
    return !strcasecmp(mime, "text/plain;charset=utf-8") ||
           !strcasecmp(mime, "UTF8_STRING") || !strcasecmp(mime, "text/plain");
}

/* An empty type counts as a uri list. The yasl API reports it that way. */
static bool mime_is_uri_list(const char *mime)
{
    return !mime[0] || !strcasecmp(mime, "text/uri-list");
}

/* Returns true for a byte that a file URI keeps without escaping. */
static bool uri_keeps(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
}

/* Returns a new file URI for the absolute `path`. Other bytes are percent
 * escaped, so the URI decodes to the same path. Returns NULL when memory runs
 * out. The caller frees the string. */
static char *file_uri(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    static const char scheme[] = "file://";
    size_t len = strlen(path);
    char *out = malloc(sizeof scheme - 1 + len * 3 + 1);
    if (!out)
        return NULL;
    size_t n = sizeof scheme - 1;
    memcpy(out, scheme, n);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (uri_keeps(c)) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
    return out;
}

/* Saves a dropped image and queues its file path as a file drop. The path
 * then takes the same route as a dropped file. */
static void queue_image_drop(struct window *w, const struct yasl_event *in)
{
    const char *path = drop_save_image(in->drop.mime, in->text, in->length);
    if (!path)
        return;
    char *uri = file_uri(path);
    if (uri) {
        input_queue_push_drop_files(&w->input, uri, strlen(uri));
        free(uri);
    }
}

/* Returns true when the uri list of `in` names at least one local file. */
static bool uri_list_has_file(const struct yasl_event *in)
{
    const char *cursor = in->text;
    return yasl_drop_next_path(&cursor, NULL, 0) != 0;
}

/* Pastes the lines of a uri list as text. It skips comment lines, and it
 * joins the other lines with a space. */
static void paste_uri_lines(struct window *w, const struct yasl_event *in)
{
    char *out = malloc(in->length);
    if (!out)
        return;
    size_t n = 0;
    const char *p = in->text;
    const char *end = in->text + in->length;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\r' && *p != '\n')
            p++;
        const char *line_end = p;
        while (p < end && (*p == '\r' || *p == '\n'))
            p++;
        while (line_end > line && (line_end[-1] == ' ' || line_end[-1] == '\t'))
            line_end--;
        if (line_end == line || *line == '#')
            continue;
        if (n)
            out[n++] = ' ';
        size_t size = (size_t)(line_end - line);
        memcpy(out + n, line, size);
        n += size;
    }
    input_queue_push_paste(&w->input, out, n);
    free(out);
}

/* Queues the payload of a DROP RELEASE by its data type. Other types are
 * ignored. */
static void queue_drop_release(struct window *w, const struct yasl_event *in)
{
    const char *mime = in->drop.mime;
    if (mime_is_image(mime)) {
        queue_image_drop(w, in);
    } else if (mime_is_text(mime)) {
        input_queue_push_paste(&w->input, in->text, in->length);
    } else if (mime_is_uri_list(mime)) {
        if (uri_list_has_file(in))
            input_queue_push_drop_files(&w->input, in->text, in->length);
        else
            paste_uri_lines(w, in);
    }
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
        w->key_text_pending = in->key.text_input == YASL_KEY_TEXT_UNSPECIFIED &&
            in->action != YASL_KEY_RELEASE && fallback_key_text(in) != 0;
        if (in->key.text_id) {
            w->native_key = *in;
        } else {
            queue_key(w, in, NULL, 0);
        }
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
    case YASL_EVENT_TEXT_EDIT: {
        enum input_text_source source = in->type == YASL_EVENT_TEXT && key_text
            ? INPUT_TEXT_KEY : INPUT_TEXT_COMMIT;
        if (in->ime.key_id && in->ime.key_id == w->native_key.key.text_id &&
            in->surface == w->native_key.surface) {
            enum input_key_route route = queue_key(w, &w->native_key, in->text, in->length);
            source = route == INPUT_KEY_PASSTHROUGH ? INPUT_TEXT_KEY : INPUT_TEXT_SHORTCUT;
            w->native_key = (struct yasl_event){0};
        }
        input_queue_push_text(&w->input, in->text, in->length, source);
        break;
    }
    case YASL_EVENT_CLIPBOARD:
        if (in->action == YASL_TRANSFER_OK && in->text) {
            input_queue_push_paste(&w->input, in->text, in->length);
        }
        break;
    case YASL_EVENT_DROP:
        if (in->action == YASL_DROP_RELEASE && in->text) {
            queue_drop_release(w, in);
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
        w->held_button = POINTER_BUTTON_NONE;
        w->pen_contact = false;
        w->key_text_pending = false;
        w->native_key = (struct yasl_event){0};
        break;
    default:
        break;
    }
}
