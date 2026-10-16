#ifndef WSAP_VTCORE_UNICODE_WIDTH_H
#define WSAP_VTCORE_UNICODE_WIDTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wsap_vtcore_unicode_range { uint32_t lo, hi; };

#include "unicode_data.inc"

static inline bool wsap_vtcore_in_ranges(const struct wsap_vtcore_unicode_range *ranges, size_t count, uint32_t cp)
{
    if (count == 0 || cp < ranges[0].lo || cp > ranges[count - 1].hi)
        return false;
    const struct wsap_vtcore_unicode_range *base = ranges;
    size_t n = count;
    while (n > 1) {
        size_t half = n / 2;
        base = (base[half].lo <= cp) ? base + half : base;
        n -= half;
    }
    return cp <= base->hi;
}

#define WSAP_VTCORE_IN_TABLE(table, cp) wsap_vtcore_in_ranges(table, sizeof(table) / sizeof(table[0]), cp)

static inline int wsap_vtcore_unicode_width(uint32_t cp)
{
    if (cp > 0x10FFFF || WSAP_VTCORE_IN_TABLE(wsap_vtcore_nonprintable, cp))
        return -1;
    if (WSAP_VTCORE_IN_TABLE(wsap_vtcore_zero_width, cp))
        return 0;
    return WSAP_VTCORE_IN_TABLE(wsap_vtcore_unicode_wide, cp) ? 2 : 1;
}

static inline bool wsap_vtcore_unicode_extended_pictographic(uint32_t cp)
{
    return WSAP_VTCORE_IN_TABLE(wsap_vtcore_extended_pictographic, cp);
}

static inline bool wsap_vtcore_unicode_emoji_presentation(uint32_t cp)
{
    return WSAP_VTCORE_IN_TABLE(wsap_vtcore_emoji_presentation, cp);
}

static inline bool wsap_vtcore_unicode_spacing_mark(uint32_t cp)
{
    if (cp < 0x903 || ((cp - 0xABEDu) < (0x11000u - 0xABEDu)) || cp > 0x1D281)
        return false;
    return WSAP_VTCORE_IN_TABLE(wsap_vtcore_spacing_mark, cp);
}

#undef WSAP_VTCORE_IN_TABLE

#endif
