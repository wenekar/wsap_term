#ifndef YASL_PRIVATE_COMMON_KEYBOARD_H
#define YASL_PRIVATE_COMMON_KEYBOARD_H

#include <stdint.h>

/* Layout labels omit controls, invalid Unicode, and Cocoa function-key PUA. */
static inline uint32_t yasl_private_key_label(uint32_t cp)
{
    return cp >= 0x20 && cp != 0x7f && cp <= 0x10ffff &&
        !(cp >= 0x80 && cp <= 0x9f) && !(cp >= 0xd800 && cp <= 0xdfff) &&
        !(cp >= 0xf700 && cp <= 0xf7ff) ? cp : 0;
}

#endif
