#ifndef WSAP_UI_KEYS_H
#define WSAP_UI_KEYS_H

#include <stdbool.h>
#include <stdint.h>

enum ui_modifiers {
    UI_MOD_SHIFT = 1,
    UI_MOD_ALT = 2,
    UI_MOD_CTRL = 4,
    UI_MOD_SUPER = 8,
};

enum ui_key {
    UI_KEY_NONE = 0,
    UI_KEY_ENTER, UI_KEY_ESCAPE, UI_KEY_TAB, UI_KEY_BACKSPACE, UI_KEY_DELETE,
    UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN,
    UI_KEY_CHAR,
};

enum ui_edit_unit {
    UI_EDIT_CHARACTER,
    UI_EDIT_WORD,
    UI_EDIT_WORD_END,
    UI_EDIT_LINE,
};

enum ui_edit_bindings {
    UI_EDIT_BINDINGS_DEFAULT,
    UI_EDIT_BINDINGS_MACOS,
};

static inline enum ui_edit_unit ui_edit_unit_for_mods(unsigned mods,
                                                     enum ui_edit_bindings bindings)
{
    if (bindings == UI_EDIT_BINDINGS_MACOS) {
        if (mods & UI_MOD_SUPER)
            return UI_EDIT_LINE;
        if (mods & UI_MOD_ALT)
            return UI_EDIT_WORD_END;
    } else if (mods & UI_MOD_CTRL) {
        return UI_EDIT_WORD;
    }
    return UI_EDIT_CHARACTER;
}

enum ui_edit_command {
    UI_EDIT_NONE,
    UI_EDIT_START, UI_EDIT_END,
    UI_EDIT_LEFT, UI_EDIT_RIGHT,
    UI_EDIT_DELETE_BACK, UI_EDIT_DELETE_FORWARD,
    UI_EDIT_SELECT_ALL, UI_EDIT_COPY, UI_EDIT_CUT, UI_EDIT_PASTE,
    UI_EDIT_UNDO, UI_EDIT_REDO,
    UI_EDIT_KILL_START, UI_EDIT_KILL_END, UI_EDIT_KILL_WORD,
    UI_EDIT_YANK, UI_EDIT_TRANSPOSE,
};

static inline enum ui_edit_command ui_edit_command_for_key(enum ui_key key,
    uint32_t text, unsigned mods, enum ui_edit_bindings bindings)
{
    unsigned primary = bindings == UI_EDIT_BINDINGS_MACOS ? UI_MOD_SUPER : UI_MOD_CTRL;
    if ((mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SUPER)) == primary && key == UI_KEY_CHAR) {
        switch (text >= 'A' && text <= 'Z' ? text + 'a' - 'A' : text) {
        case 'a': return UI_EDIT_SELECT_ALL;
        case 'c': return UI_EDIT_COPY;
        case 'x': return UI_EDIT_CUT;
        case 'v': return UI_EDIT_PASTE;
        case 'z': return mods & UI_MOD_SHIFT ? UI_EDIT_REDO : UI_EDIT_UNDO;
        case 'y': if (bindings == UI_EDIT_BINDINGS_DEFAULT) return UI_EDIT_REDO; break;
        }
    }
    if (bindings == UI_EDIT_BINDINGS_MACOS) {
        if (key == UI_KEY_UP && (mods & UI_MOD_SUPER)) return UI_EDIT_START;
        if (key == UI_KEY_DOWN && (mods & UI_MOD_SUPER)) return UI_EDIT_END;
        if ((mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SUPER)) == UI_MOD_CTRL && key == UI_KEY_CHAR) {
            switch (text >= 'A' && text <= 'Z' ? text + 'a' - 'A' : text) {
            case 'a': case 'p': return UI_EDIT_START;
            case 'e': case 'n': return UI_EDIT_END;
            case 'b': return UI_EDIT_LEFT;
            case 'f': return UI_EDIT_RIGHT;
            case 'h': return UI_EDIT_DELETE_BACK;
            case 'd': return UI_EDIT_DELETE_FORWARD;
            case 'k': return UI_EDIT_KILL_END;
            case 'u': return UI_EDIT_KILL_START;
            case 'w': return UI_EDIT_KILL_WORD;
            case 'y': return UI_EDIT_YANK;
            case 't': return UI_EDIT_TRANSPOSE;
            }
        }
    }
    return UI_EDIT_NONE;
}

enum ui_key_action : uint8_t {
    UI_KEY_PRESS = 0,
    UI_KEY_REPEAT,
    UI_KEY_RELEASE,
};

struct ui_key_event {
    enum ui_key key;
    enum ui_key_action action;
    unsigned mods;
    uint32_t text;
    const void *native;
    enum ui_edit_unit edit_unit;
    enum ui_edit_command edit_command;
    bool text_deferred;
};

#endif
