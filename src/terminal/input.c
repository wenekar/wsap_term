#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal/internal.h"
#include "ui/surface.h"

struct wsap_vtcore_modes terminal_input_modes(const struct terminal *t)
{
    if (!t || !t->session.term)
        return (struct wsap_vtcore_modes){0};
    return wsap_vtcore_modes(t->session.term);
}

void terminal_send_input(struct terminal *t, const char *data, size_t len)
{
    if (!t || !len || !t->session.term)
        return;
    struct wsap_vtcore *term = t->session.term;
    session_on_keystroke(&t->session, yasl_time_ns() / 1000000);
    if (wsap_vtcore_cursor(term).blink && t->window)
        window_redraw(t->window);
    bool changed = false;
    if (wsap_vtcore_has_selection(term)) {
        wsap_vtcore_select_clear(term);
        changed = true;
    }
    if (wsap_vtcore_scrollback(term).offset > 0) {
        wsap_vtcore_scroll_to(term, 0);
        changed = true;
    }
    if (changed && t->window)
        window_redraw(t->window);
    session_write_bytes(&t->session, data, len);
}

void terminal_copy(struct terminal *t)
{
    if (!t || !t->session.term || !t->window)
        return;
    char *text = wsap_vtcore_selection_text(t->session.term);
    if (text) {
        window_copy(t->window, YASL_CLIPBOARD, text);
        wsap_vtcore_text_free(text);
    }
}

static void scroll_view_lines(struct terminal *t, int delta_lines)
{
    if (!t || !t->session.term)
        return;
    wsap_vtcore_scroll(t->session.term, delta_lines);
    selection_extend_to_pointer(&t->selection, t->session.term);
    terminal_show_scrollbar(t);
    if (t->window)
        window_redraw(t->window);
}

/* The terminal commands. The owner runs the tab commands and leaves the
 * rest to this function. */
void terminal_handle_shortcut(struct terminal *t, enum input_shortcut sc)
{
    if (!t)
        return;
    struct window *w = t->window;
    switch (sc) {
    case INPUT_SHORTCUT_FONT_LARGER:  terminal_adjust_font_size(t, 1); break;
    case INPUT_SHORTCUT_FONT_SMALLER: terminal_adjust_font_size(t, -1); break;
    case INPUT_SHORTCUT_COPY:
        if (t->session.term && wsap_vtcore_has_selection(t->session.term)) {
            terminal_copy(t);
            wsap_vtcore_select_clear(t->session.term);
            if (w)
                window_redraw(w);
        } else {
            terminal_send_input(t, "\x03", 1);
        }
        break;
    case INPUT_SHORTCUT_PASTE:
        if (w)
            window_paste(w, YASL_CLIPBOARD);
        break;
    case INPUT_SHORTCUT_SELECT_ALL:   terminal_select_all(t); break;
    case INPUT_SHORTCUT_KILL_LINE:    terminal_send_input(t, "\x15", 1); break;
    case INPUT_SHORTCUT_SCROLL_UP:    scroll_view_lines(t, 15); break;
    case INPUT_SHORTCUT_SCROLL_DOWN:  scroll_view_lines(t, -15); break;
    case INPUT_SHORTCUT_SETTINGS:
        if (w)
            window_host_command(w, INPUT_SHORTCUT_SETTINGS);
        break;
    default:
        break;
    }
}

_Static_assert(INPUT_KEY_COUNT == (int)WSAP_VTCORE_KEY_COUNT &&
               INPUT_KEY_F1 == (int)WSAP_VTCORE_KEY_F1 && INPUT_KEY_KP_0 == (int)WSAP_VTCORE_KEY_KP_0 &&
               INPUT_KEY_CAPS_LOCK == (int)WSAP_VTCORE_KEY_CAPS_LOCK &&
               INPUT_KEY_MEDIA_PLAY == (int)WSAP_VTCORE_KEY_MEDIA_PLAY &&
               INPUT_KEY_LSHIFT == (int)WSAP_VTCORE_KEY_LSHIFT &&
               INPUT_RELEASE == (int)WSAP_VTCORE_KEY_RELEASE,
               "input and term key codes differ");

bool terminal_key(struct terminal *t, const struct input_key_event *key)
{
    struct window *w = t ? t->window : NULL;
    if (!w)
        return false;
    if (key->action != INPUT_RELEASE)
        window_ui_hide_pointer(w);
    struct wsap_vtcore_modes m = terminal_input_modes(t);
    char local[128];
    size_t capacity = sizeof(local);
    char *buf = local;
    if (key->text_len > (SIZE_MAX - 64) / 8)
        return false;
    size_t needed = 64 + 8 * key->text_len;
    if (needed > capacity) {
        capacity = needed;
        buf = malloc(capacity);
        if (!buf) return false;
    }
    struct wsap_vtcore_key_event tkey = {
        .action = (enum wsap_vtcore_key_action)key->action,
        .key = (uint16_t)key->key,
        .keypad = (uint16_t)key->keypad,
        .mods = key->mods & ~key->consumed_mods,
        .codepoint = key->codepoint,
        .shifted = key->shifted,
        .base = key->base,
        .text = key->text,
        .text_deferred = key->text_deferred,
        .text_utf8 = key->text_utf8,
        .text_len = key->text_len,
    };
    struct wsap_vtcore_input_result r =
        wsap_vtcore_input_encode_key((const struct wsap_vtcore_keyboard_state *)window_keyboard(w), &tkey, &m, buf, capacity);
    bool sent = r.status == WSAP_VTCORE_INPUT_BYTES;
    if (sent)
        terminal_send_input(t, buf, r.len);
    if (buf != local) free(buf);
    return sent;
}

void terminal_text(struct terminal *t, const char *text, size_t len)
{
    struct wsap_vtcore_modes modes = terminal_input_modes(t);
    unsigned text_flags = WSAP_VTCORE_KEYBOARD_ALL_KEYS | WSAP_VTCORE_KEYBOARD_ASSOCIATED_TEXT;
    if ((modes.keyboard & text_flags) == text_flags) {
        struct input_key_event key = {
            .action = INPUT_PRESS, .text_utf8 = (char *)text, .text_len = len,
        };
        if (terminal_key(t, &key)) return;
    }
    terminal_send_input(t, text, len);
}
