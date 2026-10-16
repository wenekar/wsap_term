#ifndef WSAP_VTCORE_UNICODE_GRAPHEME_H
#define WSAP_VTCORE_UNICODE_GRAPHEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WSAP_VTCORE_GRAPHEME_MAX_LEN 20
#define WSAP_VTCORE_GRAPHEME_HANDLE_BASE 0x7F000000u

static inline bool wsap_vtcore_grapheme_is_handle(uint32_t c)
{
    return c >= WSAP_VTCORE_GRAPHEME_HANDLE_BASE;
}

static inline bool wsap_vtcore_grapheme_regional_indicator(uint32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

static inline bool wsap_vtcore_grapheme_emoji_modifier(uint32_t cp)
{
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

#endif
