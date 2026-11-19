#ifndef YASL_PRIVATE_X11_KEYMAP_H
#define YASL_PRIVATE_X11_KEYMAP_H

#include <stdint.h>
#include "yasl/keycodes.h"

enum yasl_key yasl_private_key_from_xkb_name(const char name[4]);

#endif
