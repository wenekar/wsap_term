#ifndef UTIL_UNICODE_GRAPHEME_H
#define UTIL_UNICODE_GRAPHEME_H

/* This module is a copy of the Unicode helpers of wsap_vtcore.
 * See util/unicode/width.h for the sync rule with the term copy. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNICODE_GRAPHEME_MAX_LEN 20
#define UNICODE_GRAPHEME_HANDLE_BASE 0x7F000000u

static inline bool unicode_grapheme_is_handle(uint32_t c)
{
    return c >= UNICODE_GRAPHEME_HANDLE_BASE;
}

static inline bool unicode_grapheme_regional_indicator(uint32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

static inline bool unicode_grapheme_emoji_modifier(uint32_t cp)
{
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

#endif
