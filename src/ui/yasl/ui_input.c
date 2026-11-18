/*
 * Step 2 of the input path.
 *
 * It turns an input event into a UI event. Step 1 is `native_input.c`. Step 3
 * is `ui_route.c`.
 */

#include "ui/yasl/ui_input.h"
#include "ui/yasl/native_input.h"

static enum ui_key_action key_action(enum input_action action)
{
    switch (action) {
    case INPUT_REPEAT:  return UI_KEY_REPEAT;
    case INPUT_RELEASE: return UI_KEY_RELEASE;
    default:            return UI_KEY_PRESS;
    }
}

static struct ui_key_event raw_key_from_input(const struct input_key_event *ev)
{
    struct ui_key_event out = { .key = UI_KEY_NONE };
    if (!ev)
        return out;
    out.mods = ev->mods;
#if defined(__APPLE__)
    out.edit_unit = ui_edit_unit_for_mods(out.mods, UI_EDIT_BINDINGS_MACOS);
#else
    out.edit_unit = ui_edit_unit_for_mods(out.mods, UI_EDIT_BINDINGS_DEFAULT);
#endif
    out.action = key_action(ev->action);
    out.native = ev;
    switch (ev->key) {
    case INPUT_KEY_RETURN:
    case INPUT_KEY_KP_ENTER:
        out.key = UI_KEY_ENTER;
        return out;
    case INPUT_KEY_ESCAPE:    out.key = UI_KEY_ESCAPE;    return out;
    case INPUT_KEY_TAB:       out.key = UI_KEY_TAB;       return out;
    case INPUT_KEY_BACKSPACE: out.key = UI_KEY_BACKSPACE; return out;
    case INPUT_KEY_DELETE:
    case INPUT_KEY_KP_DELETE:
        out.key = UI_KEY_DELETE;
        return out;
    case INPUT_KEY_UP:
    case INPUT_KEY_KP_UP:     out.key = UI_KEY_UP;        return out;
    case INPUT_KEY_DOWN:
    case INPUT_KEY_KP_DOWN:   out.key = UI_KEY_DOWN;      return out;
    case INPUT_KEY_LEFT:
    case INPUT_KEY_KP_LEFT:   out.key = UI_KEY_LEFT;      return out;
    case INPUT_KEY_RIGHT:
    case INPUT_KEY_KP_RIGHT:  out.key = UI_KEY_RIGHT;     return out;
    case INPUT_KEY_HOME:
    case INPUT_KEY_KP_HOME:   out.key = UI_KEY_HOME;      return out;
    case INPUT_KEY_END:
    case INPUT_KEY_KP_END:    out.key = UI_KEY_END;       return out;
    case INPUT_KEY_PAGE_UP:
    case INPUT_KEY_KP_PAGE_UP:
        out.key = UI_KEY_PAGE_UP;
        return out;
    case INPUT_KEY_PAGE_DOWN:
    case INPUT_KEY_KP_PAGE_DOWN:
        out.key = UI_KEY_PAGE_DOWN;
        return out;
    case INPUT_KEY_INSERT:
    case INPUT_KEY_KP_INSERT:
        out.key = UI_KEY_INSERT;
        return out;
    default:
        break;
    }
    if (ev->key >= INPUT_KEY_F1 && ev->key <= INPUT_KEY_F35) {
        out.key = (enum ui_key)(UI_KEY_F1 + (ev->key - INPUT_KEY_F1));
        return out;
    }
    if (ev->key >= INPUT_KEY_KP_0 && ev->key <= INPUT_KEY_KP_BEGIN) {
        out.key = (enum ui_key)(UI_KEY_KP_0 + (ev->key - INPUT_KEY_KP_0));
        return out;
    }
    if (ev->key >= INPUT_KEY_CAPS_LOCK && ev->key <= INPUT_KEY_ISO_LEVEL5_SHIFT) {
        out.key = (enum ui_key)(UI_KEY_CAPS_LOCK + (ev->key - INPUT_KEY_CAPS_LOCK));
        return out;
    }
    if (ev->codepoint || ev->shifted) {
        out.key = UI_KEY_CHAR;
        bool letter = input_key_is_letter(ev->codepoint, ev->shifted);
        bool shift = (ev->mods & INPUT_MOD_SHIFT) != 0;
        bool caps = (ev->mods & INPUT_MOD_CAPS) != 0;
        if (input_key_selects_shifted(letter, shift, caps) && ev->shifted)
            out.text = ev->shifted;
        else
            out.text = ev->codepoint ? ev->codepoint : ev->shifted;
    }
    return out;
}

struct ui_key_event ui_key_from_input(const struct input_key_event *ev)
{
    struct ui_key_event out = raw_key_from_input(ev);
#if defined(__APPLE__)
    enum ui_edit_bindings bindings = UI_EDIT_BINDINGS_MACOS;
#else
    enum ui_edit_bindings bindings = UI_EDIT_BINDINGS_DEFAULT;
#endif
    out.edit_command = ui_edit_command_for_key(out.key, out.text, out.mods, bindings);
    out.text_deferred = ev && ev->text_deferred;
    return out;
}

struct ui_pointer_event ui_pointer_from_input(const struct input_mouse_event *ev)
{
    struct ui_pointer_event out = {0};
    if (!ev)
        return out;
    out.pos = ev->pos;
    out.button = ev->button;
    out.scroll_lines = ev->scroll_lines;
    out.scroll_delta = ev->scroll_delta;
    out.mods = ev->mods;
    out.device = ev->device;
    out.pen = ev->pen;
    switch (ev->kind) {
    case PTR_PRESS:   out.kind = UI_POINTER_PRESS; break;
    case PTR_RELEASE: out.kind = UI_POINTER_RELEASE; break;
    case PTR_MOTION:  out.kind = UI_POINTER_MOTION; break;
    case PTR_SCROLL:  out.kind = UI_POINTER_SCROLL; break;
    case PTR_LEAVE:   out.kind = UI_POINTER_LEAVE; break;
    }
    return out;
}
