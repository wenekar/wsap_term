#ifndef WINDOW_NATIVE_INPUT_H
#define WINDOW_NATIVE_INPUT_H

#include <stdint.h>

#include "yasl/yasl.h"

struct window;

void window_native_input(struct window *w, const struct yasl_event *in);
int input_modifiers_from_yasl(uint32_t mods);

#endif
