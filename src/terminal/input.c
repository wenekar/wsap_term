#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal/internal.h"
#include "ui/surface.h"

struct wsap_vtcore_modes terminal_input_modes(const struct window *w)
{
    struct wsap_vtcore *t = terminal_term(w);
    if (!t)
        return (struct wsap_vtcore_modes){0};
    return wsap_vtcore_modes(t);
}

void terminal_focus(struct window *w, bool focused)
{
    struct terminal *tw = terminal_of(w);
    if (tw)
        tw->focused = focused;
    struct wsap_vtcore *t = terminal_term(w);
    if (t)
        wsap_vtcore_focus(t, focused);
    if (!focused)
        ui_palette_close(&tw->palette);
    window_redraw(w);
}

void terminal_send_input(struct window *w, const char *data, size_t len)
{
    struct terminal *tw = terminal_of(w);
    if (!len || ui_palette_is_open(&tw->palette) || window_menu_is_open(w))
        return;
    struct tab *tab = terminal_active_tab(w);
    if (tab) {
        struct wsap_vtcore *t = tab->session.term;
        session_on_keystroke(&tab->session, yasl_time_ns() / 1000000);
        if (wsap_vtcore_cursor(t).blink)
            window_redraw(w);
        bool changed = false;
        if (wsap_vtcore_has_selection(t)) {
            wsap_vtcore_select_clear(t);
            changed = true;
        }
        if (wsap_vtcore_scrollback(t).offset > 0) {
            wsap_vtcore_scroll_to(t, 0);
            changed = true;
        }
        if (changed)
            window_redraw(w);
    }
    session_write_bytes(terminal_active_session(w), data, len);
}

void terminal_copy(struct window *w)
{
    char *text = wsap_vtcore_selection_text(terminal_term(w));
    if (text) {
        window_copy(w, YASL_CLIPBOARD, text);
        wsap_vtcore_text_free(text);
    }
}

void terminal_paste(struct window *w, const char *text, size_t len)
{
    struct session *s = terminal_active_session(w);
    if (!session_paste(s, text, len))
        return;
    wsap_vtcore_select_clear(s->term);
    wsap_vtcore_scroll_to(s->term, 0);
    window_redraw(w);
}

static void scroll_view_lines(struct window *w, int delta_lines)
{
    struct tab *t = terminal_active_tab(w);
    if (!t)
        return;
    wsap_vtcore_scroll(t->session.term, delta_lines);
    selection_extend_to_pointer(&t->selection, t->session.term);
    terminal_show_scrollbar(w);
    window_redraw(w);
}

void terminal_handle_shortcut(struct window *w, enum input_shortcut sc)
{
    struct terminal *tw = terminal_of(w);
    switch (sc) {
    case INPUT_SHORTCUT_FONT_LARGER:  terminal_adjust_font_size(w, 1); break;
    case INPUT_SHORTCUT_FONT_SMALLER: terminal_adjust_font_size(w, -1); break;
    case INPUT_SHORTCUT_COPY:
        if (terminal_term(w) && wsap_vtcore_has_selection(terminal_term(w))) {
            terminal_copy(w);
            wsap_vtcore_select_clear(terminal_term(w));
            window_redraw(w);
        } else {
            terminal_send_input(w, "\x03", 1);
        }
        break;
    case INPUT_SHORTCUT_PASTE:        window_paste(w, YASL_CLIPBOARD); break;
    case INPUT_SHORTCUT_SELECT_ALL:   terminal_select_all(w); break;
    case INPUT_SHORTCUT_KILL_LINE:    terminal_send_input(w, "\x15", 1); break;
    case INPUT_SHORTCUT_SCROLL_UP:    scroll_view_lines(w, 15); break;
    case INPUT_SHORTCUT_SCROLL_DOWN:  scroll_view_lines(w, -15); break;
    case INPUT_SHORTCUT_NEW_TAB:      (void)terminal_new_tab(w); break;
    case INPUT_SHORTCUT_CLOSE_TAB:    terminal_close_tab(w, tw->active_tab); break;
    case INPUT_SHORTCUT_NEXT_TAB:
        if (tw->tab_count > 1)
            terminal_select_tab(w, (tw->active_tab + 1) % tw->tab_count);
        break;
    case INPUT_SHORTCUT_PREV_TAB:
        if (tw->tab_count > 1)
            terminal_select_tab(w, (tw->active_tab + tw->tab_count - 1) % tw->tab_count);
        break;
    case INPUT_SHORTCUT_LAST_TAB:
        terminal_select_tab(w, tw->tab_count - 1);
        break;
    case INPUT_SHORTCUT_TAB_1:
    case INPUT_SHORTCUT_TAB_2:
    case INPUT_SHORTCUT_TAB_3:
    case INPUT_SHORTCUT_TAB_4:
    case INPUT_SHORTCUT_TAB_5:
    case INPUT_SHORTCUT_TAB_6:
    case INPUT_SHORTCUT_TAB_7:
    case INPUT_SHORTCUT_TAB_8:
        terminal_select_tab(w, (int)sc - INPUT_SHORTCUT_TAB_1);
        break;
    case INPUT_SHORTCUT_COMMAND_PALETTE:
        terminal_palette_toggle(w);
        break;
    case INPUT_SHORTCUT_SETTINGS:
        window_host_command(w, INPUT_SHORTCUT_SETTINGS);
        break;
    case INPUT_SHORTCUT_FOCUS_TABS: {
        struct ui_widget *bar = ui_tabbar_widget(&tw->tabbar);
        if (tw->tab_count > 1)
            ui_surface_focus(window_ui_surface(w),
                ui_surface_focused(window_ui_surface(w)) == bar ? NULL : bar);
        break;
    }
    case INPUT_SHORTCUT_DEBUG_OVERLAY:
    case INPUT_SHORTCUT_NONE:
    case INPUT_SHORTCUT_COUNT:
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

bool terminal_key(struct window *w, const struct input_key_event *key)
{
    if (key->action != INPUT_RELEASE)
        window_ui_hide_pointer(w);
    struct wsap_vtcore_modes m = terminal_input_modes(w);
    char buf[64];
    struct wsap_vtcore_key_event tkey = {
        .action = (enum wsap_vtcore_key_action)key->action,
        .key = (uint16_t)key->key,
        .keypad = (uint16_t)key->keypad,
        .mods = key->mods,
        .codepoint = key->codepoint,
        .shifted = key->shifted,
        .base = key->base,
        .text = key->text,
        .text_deferred = key->text_deferred,
    };
    struct wsap_vtcore_input_result r =
        wsap_vtcore_input_encode_key((const struct wsap_vtcore_keyboard_state *)window_keyboard(w), &tkey, &m, buf, sizeof(buf));
    if (r.status != WSAP_VTCORE_INPUT_BYTES)
        return false;
    terminal_send_input(w, buf, r.len);
    return true;
}
