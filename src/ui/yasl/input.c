#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"

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
