#include "ui/yasl/inspector.h"
#include "ui/yasl/ui_input.h"
#include "ui/yasl/internal.h"

/* The edit command a shortcut stands for. Widgets run it on focus. */
static enum ui_edit_command shortcut_edit(enum input_shortcut action)
{
    switch (action) {
    case INPUT_SHORTCUT_COPY:       return UI_EDIT_COPY;
    case INPUT_SHORTCUT_PASTE:      return UI_EDIT_PASTE;
    case INPUT_SHORTCUT_SELECT_ALL: return UI_EDIT_SELECT_ALL;
    default:                        return UI_EDIT_NONE;
    }
}

/* Keys and text go to the key surface. Pointer input goes to its surface.
 * Keyboard-path dispatches draw only through invalidation. */
static enum window_input_result ui_input(struct window *w, const struct input_event *ev)
{
    uint64_t now_ms = window_frame_time_ms(w);
    struct ui_surface *keys = window_ui_key_surface(w);
    struct ui_event event;
    switch (ev->kind) {
    case INPUT_SHORTCUT:
        event = ui_event_shortcut(ev->shortcut.action,
                                  shortcut_edit(ev->shortcut.action), now_ms);
        ui_surface_dispatch(keys, &event);
        return WINDOW_INPUT_HANDLED;
    case INPUT_KEY: {
        event = ui_event_key(ui_key_from_input(&ev->key), now_ms);
        bool handled = ui_surface_dispatch(keys, &event);
        bool kept = ui_surface_key_text_kept(keys);
        if (!handled)
            return WINDOW_INPUT_UNHANDLED;
        return kept ? WINDOW_INPUT_HANDLED : WINDOW_INPUT_REPLACE_TEXT;
    }
    case INPUT_TEXT:
        event = ui_event_text(ev->text.utf8, ev->text.len, now_ms);
        ui_surface_dispatch(keys, &event);
        return WINDOW_INPUT_HANDLED;
    case INPUT_PASTE:
        event = ui_event_paste(ev->paste.data, ev->paste.len, now_ms);
        ui_surface_dispatch(keys, &event);
        return WINDOW_INPUT_HANDLED;
    case INPUT_DROP_FILES:
        event = ui_event_drop_files(ev->drop.data, ev->drop.len, now_ms);
        ui_surface_dispatch(keys, &event);
        return WINDOW_INPUT_HANDLED;
    case INPUT_MOUSE:
        event = ui_event_pointer(ui_pointer_from_input(&ev->mouse), now_ms);
        ui_surface_dispatch(window_ui_surface_at(w, ev->mouse.popup), &event);
        break;
    default:
        return WINDOW_INPUT_UNHANDLED;
    }
    window_redraw(w);
    return WINDOW_INPUT_HANDLED;
}


static enum yasl_cursor cursor_for(enum ui_cursor cursor)
{
    switch (cursor) {
    case UI_CURSOR_TEXT:    return YASL_CURSOR_TEXT;
    case UI_CURSOR_POINTER: return YASL_CURSOR_LINK;
    case UI_CURSOR_NONE:    return YASL_CURSOR_HIDDEN;
    default:                return YASL_CURSOR_ARROW;
    }
}

void window_ui_update_cursor(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    struct yasl_surface *surface = window_surface(w);
    if (!ui || !surface || !window_pointer(w).inside)
        return;
    yasl_surface_set_cursor(surface, ui->pointer_hidden ? YASL_CURSOR_HIDDEN
        : cursor_for(ui_surface_cursor(&ui->screen)));
}

void window_ui_hide_pointer(struct window *w)
{
    struct window_ui *ui = window_ui_of(w);
    if (!ui)
        return;
    ui->pointer_hidden = true;
    window_ui_update_cursor(w);
}

/* While a menu is open, all input goes to the UI surfaces. A press on
 * the window under menu popups closes the menu. */
static enum window_input_result menu_input(struct window *w, const struct input_event *ev)
{
    if (!ui_menu_is_open(&ui_of(w)->menu))
        return WINDOW_INPUT_UNHANDLED;
    if (ev->kind == INPUT_MOUSE && ev->mouse.kind == PTR_PRESS &&
        ui_of(w)->menu_popups > 0 && ev->mouse.popup == 0) {
        window_menu_close(w);
        return WINDOW_INPUT_HANDLED;
    }
    enum window_input_result result = ui_input(w, ev);
    return result == WINDOW_INPUT_UNHANDLED ? WINDOW_INPUT_HANDLED : result;
}

void window_ui_route(struct window *w, struct input_event *ev)
{
    switch (ev->kind) {
    case INPUT_MOUSE:
        if (ev->mouse.kind == PTR_MOTION)
            ui_of(w)->pointer_hidden = false;
        break;
    case INPUT_KEY:
        if (ev->key.action != INPUT_RELEASE)
            ui_of(w)->replace_key_text = false;
        break;
    case INPUT_FOCUS:
        if (!ev->focus.focused) {
            ui_of(w)->replace_key_text = false;
            ui_surface_cancel_capture(&ui_of(w)->screen);
        }
        /* The content hears the window's focus even while a menu is open. */
        if (ui_of(w)->spec.root) {
            struct ui_event focus = ui_event_window_focus(ev->focus.focused,
                                                          window_frame_time_ms(w));
            ui_widget_event(ui_of(w)->spec.root, &focus);
        }
        window_menu_sync(w);
        return;
    case INPUT_SHORTCUT:
        if (ev->shortcut.action == INPUT_SHORTCUT_DEBUG_OVERLAY) {
            window_debug_toggle_overlay(w);
            return;
        }
        if (window_host_command(w, ev->shortcut.action))
            return;
        break;
    default:
        break;
    }
    if (ev->kind == INPUT_TEXT &&
        (ev->text.source == INPUT_TEXT_SHORTCUT ||
         (ev->text.source == INPUT_TEXT_KEY && ui_of(w)->replace_key_text)))
        return;
    enum window_input_result result = menu_input(w, ev);
    if (result == WINDOW_INPUT_UNHANDLED)
        result = ui_input(w, ev);
    if (ev->kind == INPUT_KEY && ev->key.action != INPUT_RELEASE)
        ui_of(w)->replace_key_text = result == WINDOW_INPUT_REPLACE_TEXT;
    window_menu_sync(w);
    if (ev->kind == INPUT_MOUSE && !window_closed(w))
        window_ui_update_cursor(w);
}

