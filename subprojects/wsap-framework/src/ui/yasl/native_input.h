#ifndef WINDOW_NATIVE_INPUT_H
#define WINDOW_NATIVE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "yasl/yasl.h"

struct window;

void window_native_input(struct window *w, const struct yasl_event *in);
int input_modifiers_from_yasl(uint32_t mods);

/* Returns true for a letter key, judged from its unshifted and shifted
 * labels, not its physical key. Call it before input_key_selects_shifted(). */
static inline bool input_key_is_letter(uint32_t codepoint, uint32_t shifted)
{
    return (codepoint >= 'a' && codepoint <= 'z') ||
           (codepoint >= 0x80 && shifted >= 0x80 && codepoint != shifted);
}

/* Returns true when a key types its shifted symbol. Shift and Caps Lock
 * cancel each other on letter keys. Caps Lock does nothing on other keys.
 * Pass letter as true for a letter key. */
static inline bool input_key_selects_shifted(bool letter, bool shift, bool caps)
{
    return shift != (letter && caps);
}

#endif
