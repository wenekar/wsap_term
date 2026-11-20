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

enum ui_key : uint8_t {
    UI_KEY_NONE = 0,
    UI_KEY_ENTER, UI_KEY_ESCAPE, UI_KEY_TAB, UI_KEY_BACKSPACE, UI_KEY_DELETE,
    UI_KEY_INSERT,
    UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN,
    UI_KEY_F1, UI_KEY_F2, UI_KEY_F3, UI_KEY_F4, UI_KEY_F5,
    UI_KEY_F6, UI_KEY_F7, UI_KEY_F8, UI_KEY_F9, UI_KEY_F10,
    UI_KEY_F11, UI_KEY_F12, UI_KEY_F13, UI_KEY_F14, UI_KEY_F15,
    UI_KEY_F16, UI_KEY_F17, UI_KEY_F18, UI_KEY_F19, UI_KEY_F20,
    UI_KEY_F21, UI_KEY_F22, UI_KEY_F23, UI_KEY_F24, UI_KEY_F25,
    UI_KEY_F26, UI_KEY_F27, UI_KEY_F28, UI_KEY_F29, UI_KEY_F30,
    UI_KEY_F31, UI_KEY_F32, UI_KEY_F33, UI_KEY_F34, UI_KEY_F35,
    UI_KEY_KP_0, UI_KEY_KP_1, UI_KEY_KP_2, UI_KEY_KP_3, UI_KEY_KP_4,
    UI_KEY_KP_5, UI_KEY_KP_6, UI_KEY_KP_7, UI_KEY_KP_8, UI_KEY_KP_9,
    UI_KEY_KP_DECIMAL, UI_KEY_KP_ENTER, UI_KEY_KP_ADD, UI_KEY_KP_SUBTRACT,
    UI_KEY_KP_MULTIPLY, UI_KEY_KP_DIVIDE, UI_KEY_KP_EQUAL,
    UI_KEY_KP_SEPARATOR,
    UI_KEY_KP_LEFT, UI_KEY_KP_RIGHT, UI_KEY_KP_UP, UI_KEY_KP_DOWN,
    UI_KEY_KP_PAGE_UP, UI_KEY_KP_PAGE_DOWN, UI_KEY_KP_HOME, UI_KEY_KP_END,
    UI_KEY_KP_INSERT, UI_KEY_KP_DELETE, UI_KEY_KP_BEGIN,
    UI_KEY_CAPS_LOCK, UI_KEY_SCROLL_LOCK, UI_KEY_NUM_LOCK,
    UI_KEY_PRINT_SCREEN, UI_KEY_PAUSE, UI_KEY_MENU,
    UI_KEY_MEDIA_PLAY, UI_KEY_MEDIA_PAUSE, UI_KEY_MEDIA_STOP,
    UI_KEY_MEDIA_FORWARD, UI_KEY_MEDIA_REWIND, UI_KEY_MEDIA_NEXT,
    UI_KEY_MEDIA_PREV, UI_KEY_MEDIA_RECORD,
    UI_KEY_VOLUME_DOWN, UI_KEY_VOLUME_UP, UI_KEY_MUTE,
    UI_KEY_LSHIFT, UI_KEY_LCTRL, UI_KEY_LALT, UI_KEY_LSUPER,
    UI_KEY_LHYPER, UI_KEY_LMETA,
    UI_KEY_RSHIFT, UI_KEY_RCTRL, UI_KEY_RALT, UI_KEY_RSUPER,
    UI_KEY_RHYPER, UI_KEY_RMETA,
    UI_KEY_ISO_LEVEL3_SHIFT, UI_KEY_ISO_LEVEL5_SHIFT,
    UI_KEY_CHAR,
};

enum ui_edit_unit : uint8_t {
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

enum ui_edit_command : uint8_t {
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
    const void *native;
    unsigned mods;
    uint32_t text;
    enum ui_key key;
    enum ui_edit_unit edit_unit;
    enum ui_edit_command edit_command;
    enum ui_key_action action;
    bool text_deferred;
};

#endif
