#include "ui/yasl/ui_input.h"

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
    default:
        break;
    }
    if (ev->codepoint || ev->shifted) {
        out.key = UI_KEY_CHAR;
        if ((ev->mods & (INPUT_MOD_SHIFT | INPUT_MOD_CAPS)) && ev->shifted)
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
