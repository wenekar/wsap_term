#include <assert.h>

#include "ui/yasl/ui_input.h"

static void key_test(void)
{
    struct input_key_event ev = {0};
    assert(ui_key_from_input(NULL).key == UI_KEY_NONE);
    assert(ui_key_from_input(&ev).key == UI_KEY_NONE);

    ev.key = INPUT_KEY_RETURN;
    assert(ui_key_from_input(&ev).key == UI_KEY_ENTER);
    ev.key = INPUT_KEY_ESCAPE;
    assert(ui_key_from_input(&ev).key == UI_KEY_ESCAPE);
    ev.key = INPUT_KEY_BACKSPACE;
    assert(ui_key_from_input(&ev).key == UI_KEY_BACKSPACE);
    ev.key = INPUT_KEY_DELETE;
    assert(ui_key_from_input(&ev).key == UI_KEY_DELETE);
    ev.key = INPUT_KEY_TAB;
    assert(ui_key_from_input(&ev).key == UI_KEY_TAB);
    ev.key = INPUT_KEY_LEFT;
    assert(ui_key_from_input(&ev).key == UI_KEY_LEFT);
    ev.key = INPUT_KEY_RIGHT;
    assert(ui_key_from_input(&ev).key == UI_KEY_RIGHT);
    ev.key = INPUT_KEY_UP;
    assert(ui_key_from_input(&ev).key == UI_KEY_UP);
    ev.key = INPUT_KEY_DOWN;
    assert(ui_key_from_input(&ev).key == UI_KEY_DOWN);
    ev.key = INPUT_KEY_HOME;
    assert(ui_key_from_input(&ev).key == UI_KEY_HOME);
    ev.key = INPUT_KEY_END;
    assert(ui_key_from_input(&ev).key == UI_KEY_END);
    ev.key = INPUT_KEY_PAGE_UP;
    assert(ui_key_from_input(&ev).key == UI_KEY_PAGE_UP);
    ev.key = INPUT_KEY_PAGE_DOWN;
    assert(ui_key_from_input(&ev).key == UI_KEY_PAGE_DOWN);

    ev.key = INPUT_KEY_NONE;
    ev.codepoint = 'q';
    ev.mods = INPUT_MOD_SHIFT;
    struct ui_key_event uk = ui_key_from_input(&ev);
    assert(uk.key == UI_KEY_CHAR && uk.text == 'q' &&
           uk.mods == INPUT_MOD_SHIFT);

    ev.key = INPUT_KEY_F5;
    ev.codepoint = 0;
    assert(ui_key_from_input(&ev).key == UI_KEY_NONE);

    ev.action = INPUT_RELEASE;
    uk = ui_key_from_input(&ev);
    assert(uk.action == UI_KEY_RELEASE && uk.native == &ev);
    ev.action = INPUT_REPEAT;
    assert(ui_key_from_input(&ev).action == UI_KEY_REPEAT);
    ev.action = INPUT_PRESS;
    assert(ui_key_from_input(&ev).action == UI_KEY_PRESS);
}

static void editing_bindings_test(void)
{
    assert(ui_edit_unit_for_mods(UI_MOD_CTRL, UI_EDIT_BINDINGS_DEFAULT) == UI_EDIT_WORD);
    assert(ui_edit_unit_for_mods(UI_MOD_SUPER, UI_EDIT_BINDINGS_DEFAULT) == UI_EDIT_CHARACTER);
    assert(ui_edit_unit_for_mods(UI_MOD_ALT, UI_EDIT_BINDINGS_MACOS) == UI_EDIT_WORD_END);
    assert(ui_edit_unit_for_mods(UI_MOD_SUPER | UI_MOD_ALT | UI_MOD_SHIFT,
        UI_EDIT_BINDINGS_MACOS) == UI_EDIT_LINE);
    struct input_key_event ev = { .key = INPUT_KEY_RIGHT };
#if defined(__APPLE__)
    ev.mods = INPUT_MOD_ALT;
    assert(ui_key_from_input(&ev).edit_unit == UI_EDIT_WORD_END);
    ev.mods = INPUT_MOD_SUPER;
    assert(ui_key_from_input(&ev).edit_unit == UI_EDIT_LINE);
#else
    ev.mods = INPUT_MOD_CTRL;
    assert(ui_key_from_input(&ev).edit_unit == UI_EDIT_WORD);
#endif
    ev.mods |= INPUT_MOD_SHIFT;
    assert(ui_key_from_input(&ev).mods == ev.mods);
    assert(ui_key_from_input(&ev).native == &ev);
}

int main(void)
{
    key_test();
    editing_bindings_test();
    return 0;
}
