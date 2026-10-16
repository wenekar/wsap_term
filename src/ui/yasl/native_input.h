#ifndef WINDOW_NATIVE_INPUT_H
#define WINDOW_NATIVE_INPUT_H

#include <stdint.h>

#include "yasl/yasl.h"

struct window;

/* Queues one native event of w, after shortcut routing for keys. Text,
 * paste, and drop data are copied. */
void window_native_input(struct window *w, const struct yasl_event *in);
/* The INPUT_MOD_* bits for yasl's YASL_MOD_* bits. */
int input_modifiers_from_yasl(uint32_t mods);

#endif
