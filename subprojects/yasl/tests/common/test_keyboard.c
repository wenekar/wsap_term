#include "common/keyboard.h"
#include "platform/wayland/keymap.h"
#include "platform/x11/keymap.h"
#include "platform/cocoa/keymap.h"
#include "platform/cocoa/keycodes.h"
#include <assert.h>

int main(void)
{
    /* Same physical positions across native encodings, including keypad and
     * left/right modifiers. macOS A is code zero, which is not UNKNOWN. */
    assert(yasl_private_key_from_evdev(30) == YASL_KEY_A);
    assert(yasl_private_key_from_cocoa(kVK_ANSI_A) == YASL_KEY_A);
    assert(yasl_private_key_from_xkb_name("AC01") == YASL_KEY_A);
    assert(yasl_private_key_from_evdev(1) == YASL_KEY_ESCAPE);
    assert(yasl_private_key_from_cocoa(kVK_Escape) == YASL_KEY_ESCAPE);
    assert(yasl_private_key_from_xkb_name("ESC\0") == YASL_KEY_ESCAPE);
    assert(yasl_private_key_from_evdev(96) == YASL_KEY_KP_ENTER);
    assert(yasl_private_key_from_cocoa(kVK_ANSI_KeypadEnter) == YASL_KEY_KP_ENTER);
    assert(yasl_private_key_from_cocoa(kVK_Return) == YASL_KEY_ENTER);
    assert(yasl_private_key_from_evdev(100) == YASL_KEY_RIGHT_ALT);
    assert(yasl_private_key_from_cocoa(kVK_RightOption) == YASL_KEY_RIGHT_ALT);
    assert(yasl_private_key_from_evdev(42) == YASL_KEY_LEFT_SHIFT);
    assert(yasl_private_key_from_cocoa(kVK_Shift) == YASL_KEY_LEFT_SHIFT);
    assert(yasl_private_key_from_evdev(86) == YASL_KEY_ISO_BACKSLASH);
    assert(yasl_private_key_from_cocoa(kVK_ISO_Section) == YASL_KEY_ISO_BACKSLASH);
    assert(yasl_private_key_from_evdev(0) == YASL_KEY_UNKNOWN);
    assert(yasl_private_key_from_evdev(UINT32_MAX) == YASL_KEY_UNKNOWN);
    assert(yasl_private_key_from_cocoa(UINT32_MAX) == YASL_KEY_UNKNOWN);
    assert(yasl_private_key_from_xkb_name("ZZZZ") == YASL_KEY_UNKNOWN);
    assert(yasl_private_key_label(0x1f600) == 0x1f600);
    assert(yasl_private_key_label(0xd800) == 0 && yasl_private_key_label(0x110000) == 0);
    assert(yasl_private_key_label(0xf700) == 0 && yasl_private_key_label('\t') == 0);
    return 0;
}
