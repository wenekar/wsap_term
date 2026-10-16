#ifndef YASL_PRIVATE_COCOA_KEYMAP_H
#define YASL_PRIVATE_COCOA_KEYMAP_H

#include <stdint.h>
#include "yasl/keycodes.h"

enum yasl_key yasl_private_key_from_cocoa(uint32_t code);

#endif
