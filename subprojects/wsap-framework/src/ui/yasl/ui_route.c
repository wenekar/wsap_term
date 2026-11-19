/*
 * Step 3 of the input path.
 *
 * It drains the queued input events, and it sends each UI event to the surface
 * that owns it. Step 1 is `native_input.c`. Step 2 is `ui_input.c`.
 */

#include <stdio.h>

#include "input/keybind.h"
#include "ui/yasl/inspector.h"
#include "ui/yasl/ui_input.h"
#include "ui/yasl/internal.h"

/* Returns the widget that takes a drop at `pos`, or NULL when no widget
 * takes it. That widget also draws the drop mark. Every type follows one
 * rule: the drop goes to the widget at its position, and it bubbles up from
 * there. While a modal is open, a drop outside the modal reaches nothing,
 * and a drop on the modal bubbles up to the modal at most. */
static struct ui_widget *drop_mark_target(struct ui_surface *surface,
                                          struct vector2i pos)
{
    return surface ? ui_surface_drop_target(surface, pos) : NULL;
}

/* Ends the drag on the window surface before a release. The widget that the
 * drag was over gets one LEAVE, so its mark clears also when the release
 * lands on another widget. Returns the window surface, laid out for a hit
 * test. */
static struct ui_surface *end_drag(struct window *w, struct vector2i pos,
                                   uint64_t now_ms)
{
    struct ui_surface *screen = &ui_of(w)->screen;
    ui_surface_prepare(screen);
    bool enter = false;
    struct ui_widget *left = ui_surface_drag_hover(screen, NULL, &enter);
    if (left) {
        struct ui_drag_event drag = { .pos = pos };
        struct ui_event leave = ui_event_drag(UI_EVENT_DRAG_LEAVE, &drag,
                                              now_ms);
        ui_surface_drop_event(screen, left, &leave);
    }
    return screen;
}

/* Sends the event of a drop release to the widget at the drop location
 * `pos` on the window surface. The event bubbles up from that widget, and
 * while a modal is open it stops at the modal. Hover state from the drag
 * does not choose the widget, and the focused widget is no fallback. When no
 * widget on that path takes the release, nothing takes it. */
static void route_release(struct window *w, struct vector2i pos,
                          const struct ui_event *event, uint64_t now_ms)
{
    struct ui_surface *screen = end_drag(w, pos, now_ms);
    struct ui_widget *target = drop_mark_target(screen, pos);
    if (target)
        ui_surface_drop_event(screen, target, event);
}

static enum ui_edit_command shortcut_edit(input_command_id action)
{
    switch (action) {
    case INPUT_COMMAND_CUT:        return UI_EDIT_CUT;
    case INPUT_COMMAND_COPY:       return UI_EDIT_COPY;
    case INPUT_COMMAND_PASTE:      return UI_EDIT_PASTE;
    case INPUT_COMMAND_UNDO:       return UI_EDIT_UNDO;
    case INPUT_COMMAND_REDO:       return UI_EDIT_REDO;
    case INPUT_COMMAND_SELECT_ALL: return UI_EDIT_SELECT_ALL;
    default:                        return UI_EDIT_NONE;
    }
}

static enum window_input_result ui_input_dispatch(struct window *w, const struct input_event *ev)
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
        /* A paste goes to the focused widget. The release of a text drop
         * goes only to its drop location. */
        if (ev->paste.dropped)
            route_release(w, ev->paste.pos, &event, now_ms);
        else
            ui_surface_dispatch(keys, &event);
        return WINDOW_INPUT_HANDLED;
    case INPUT_DROP_FILES:
        event = ui_event_drop_files(ev->drop.data, ev->drop.len, now_ms);
        /* The file drop goes only to the widget at its drop location. */
        route_release(w, ev->drop.pos, &event, now_ms);
        return WINDOW_INPUT_HANDLED;
    case INPUT_DROP: {
        /* The drop goes to the widget at its drop location. */
        const char *mimes[1] = { ev->drop_type.mime };
        struct ui_drag_event drag = {
            .mimes = mimes,
            .mime_count = 1,
            .mime = ev->drop_type.mime,
            .data = ev->drop_type.data,
            .len = ev->drop_type.len,
            .pos = ev->drop_type.pos,
        };
        event = ui_event_drag(UI_EVENT_DROP, &drag, now_ms);
        /* A drop position is in the coordinates of the window surface. A menu
         * popup sits above that surface and takes no drop. */
        route_release(w, drag.pos, &event, now_ms);
        return WINDOW_INPUT_HANDLED;
    }
    case INPUT_DRAG: {
        /* A drag marks only the widget that will accept the drop. */
        const char *mimes[1] = { ev->drop_type.mime };
        struct ui_drag_event drag = {
            .mimes = mimes,
            .mime_count = ev->drop_type.mime ? 1 : 0,
            .mime = ev->drop_type.mime,
            .pos = ev->drop_type.pos,
        };
        struct ui_surface *screen = &ui_of(w)->screen;
        ui_surface_prepare(screen);
        struct ui_widget *target = ev->drop_type.leave
            ? NULL : drop_mark_target(screen, drag.pos);
        /* A drag that stays on one widget gets ENTER once, then OVER. A drag
         * that leaves the widget gives it LEAVE. */
        bool enter = false;
        /* The drop is hit-tested on the window surface, so its hover state
         * lives there too. A popup must not keep a target it cannot reach. */
        struct ui_widget *left = ui_surface_drag_hover(screen, target, &enter);
        if (left) {
            struct ui_event leave_event =
                ui_event_drag(UI_EVENT_DRAG_LEAVE, &drag, now_ms);
            ui_surface_drop_event(screen, left, &leave_event);
        }
        if (target) {
            event = ui_event_drag(enter ? UI_EVENT_DRAG_ENTER
                                        : UI_EVENT_DRAG_OVER,
                                  &drag, now_ms);
            ui_surface_drop_event(screen, target, &event);
        }
        return WINDOW_INPUT_HANDLED;
    }
    case INPUT_IME: {
        struct ui_ime_text_event ime = {
            .data = ev->ime.data, .len = ev->ime.len,
            .begin = ev->ime.begin, .end = ev->ime.end,
            .revision = ev->ime.revision,
        };
        event = ev->ime.edit ? ui_event_text_edit(&ime, now_ms)
                             : ui_event_composition(&ime, now_ms);
        ui_surface_dispatch(keys, &event);
        return WINDOW_INPUT_HANDLED;
    }
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

static enum window_input_result ui_input(struct window *w, const struct input_event *ev)
{
    enum window_input_result result = ui_input_dispatch(w, ev);
    window_menu_reap(w);
    return result;
}

static enum yasl_cursor cursor_for(enum ui_cursor cursor)
{
    switch (cursor) {
    case UI_CURSOR_TEXT:       return YASL_CURSOR_TEXT;
    case UI_CURSOR_POINTER:    return YASL_CURSOR_LINK;
    case UI_CURSOR_NONE:       return YASL_CURSOR_HIDDEN;
    case UI_CURSOR_CROSSHAIR:  return YASL_CURSOR_CROSSHAIR;
    case UI_CURSOR_MOVE:       return YASL_CURSOR_MOVE;
    /* yasl has no grab cursor, so a grab shows the move cursor. */
    case UI_CURSOR_GRAB:       return YASL_CURSOR_MOVE;
    case UI_CURSOR_RESIZE_EW:  return YASL_CURSOR_RESIZE_EW;
    case UI_CURSOR_RESIZE_NS:  return YASL_CURSOR_RESIZE_NS;
    case UI_CURSOR_RESIZE_NWSE: return YASL_CURSOR_RESIZE_NWSE;
    case UI_CURSOR_RESIZE_NESW: return YASL_CURSOR_RESIZE_NESW;
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
    if (ev->kind == INPUT_KEY && ev->key.action != INPUT_RELEASE)
        ui_of(w)->key_to_keymap = false;
    if (ev->kind == INPUT_KEY && ev->key.ime_handled)
        return;
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
            ui_of(w)->key_to_keymap = false;
            ui_surface_cancel_capture(&ui_of(w)->screen);
        }
        if (ui_of(w)->spec.root) {
            struct ui_event focus = ui_event_window_focus(ev->focus.focused,
                                                          window_frame_time_ms(w));
            ui_widget_event(ui_of(w)->spec.root, &focus);
        }
        window_menu_sync(w);
        return;
    case INPUT_SHORTCUT:
        if (ev->shortcut.action == INPUT_COMMAND_DEBUG_OVERLAY) {
            window_debug_toggle_overlay(w);
            return;
        }
        if (window_host_command(w, ev->shortcut.action))
            return;
        break;
    default:
        break;
    }
    /* The route of a key decides its text. The text is dropped when the
     * key ran a command, or when the widget that took the key replaces its
     * text. A widget that took the key gets its text. */
    if (ev->kind == INPUT_TEXT && ev->text.source == INPUT_TEXT_KEY &&
        (ui_of(w)->key_to_keymap || ui_of(w)->replace_key_text))
        return;
    enum window_input_result result = menu_input(w, ev);
    if (result == WINDOW_INPUT_UNHANDLED)
        result = ui_input(w, ev);
    /* The focused widget sees the key first. The keymap of the window runs
     * only for a key that no widget handles. */
    if (result == WINDOW_INPUT_UNHANDLED && ev->kind == INPUT_KEY &&
        !ev->key.ime_handled) {
        /* A widget scope in the chain of the key target wins over the
         * app scope. The lookup reads that scope first and the app scope
         * after it. */
        enum input_key_route route = input_shortcut_route_key_scoped(
            window_keymap(w), &w->input, &w->sc_state, &ev->key,
            input_key_id(&ev->key),
            ui_surface_command_scope(window_ui_key_surface(w)), NULL);
        if (route == INPUT_KEY_DROPPED)
            fprintf(stderr, "shortcut queue is full\n");
        /* A command that the full queue dropped did not run, so the text
         * of the key is typed. */
        if (ev->key.action != INPUT_RELEASE)
            ui_of(w)->key_to_keymap = route == INPUT_KEY_SHORTCUT ||
                                      route == INPUT_KEY_SWALLOWED;
    }
    if (ev->kind == INPUT_KEY && ev->key.action != INPUT_RELEASE)
        ui_of(w)->replace_key_text = result == WINDOW_INPUT_REPLACE_TEXT;
    window_menu_sync(w);
    if (ev->kind == INPUT_MOUSE && !window_closed(w))
        window_ui_update_cursor(w);
}

/* Takes the pointer state of one input event into the window. */
static void track_pointer(struct window *w, struct input_mouse_event *m)
{
    if (m->kind == PTR_LEAVE) {
        w->pointer.inside = false;
        return;
    }
    if ((m->scroll_lines || m->scroll_delta != 0.0f) && m->kind != PTR_SCROLL)
        m->kind = PTR_SCROLL;
    w->pointer.pos = m->pos;
    w->pointer.inside = true;
}

/* Takes the held keys and the pointer of one input event into the window, so
 * the UI reads the state before the event. */
static void track_native(struct window *w, struct input_event *ev)
{
    switch (ev->kind) {
    case INPUT_MOUSE:
        track_pointer(w, &ev->mouse);
        break;
    case INPUT_KEY:
        input_keyboard_update(&w->keyboard, &ev->key);
        break;
    case INPUT_FOCUS:
        if (!ev->focus.focused)
            input_keyboard_reset(&w->keyboard);
        break;
    default:
        break;
    }
}

void window_drain_input(struct window *w)
{
    if (!w->surface || w->closed)
        return;
    struct input_event ev;
    while (!w->closed && (window_inject_pop(w, &ev) || input_queue_take(&w->input, &ev))) {
        track_native(w, &ev);
        if (window_mounted(w))
            window_ui_route(w, &ev);
        input_event_release(&ev);
    }
}
