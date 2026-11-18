#ifndef UTIL_UNICODE_WIDTH_H
#define UTIL_UNICODE_WIDTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This module is a copy of the Unicode helpers of wsap_vtcore.
 * The source is subprojects/term/include/wsap_vtcore/unicode/.
 * That subproject builds alone and cannot use the main tree.
 * So each side holds a copy of the code and of the data tables.
 * The term copy is the output of subprojects/term/tools/generate_unicode.py.
 * When that copy changes, apply the same change here.
 * Rename the wsap_vtcore_ prefix to unicode_, and give each table the
 * unicode_table_ prefix. tests/util/unicode_parity_test.c checks that the two
 * copies agree.
 */

struct unicode_range {
    uint32_t lo, hi;
};

#include "unicode_data.inc"

static inline bool unicode_in_ranges(const struct unicode_range *ranges, size_t count, uint32_t cp)
{
    if (count == 0 || cp < ranges[0].lo || cp > ranges[count - 1].hi)
        return false;
    const struct unicode_range *base = ranges;
    size_t n = count;
    while (n > 1) {
        size_t half = n / 2;
        base = (base[half].lo <= cp) ? base + half : base;
        n -= half;
    }
    return cp <= base->hi;
}

#define UNICODE_IN_TABLE(table, cp) unicode_in_ranges(table, sizeof(table) / sizeof(table[0]), cp)

/* -1 for a nonprintable code point, 0 for a zero-width code point, else the
 * number of cells the code point takes. */
static inline int unicode_width(uint32_t cp)
{
    if (cp > 0x10FFFF || UNICODE_IN_TABLE(unicode_table_nonprintable, cp))
        return -1;
    if (UNICODE_IN_TABLE(unicode_table_zero_width, cp))
        return 0;
    return UNICODE_IN_TABLE(unicode_table_wide, cp) ? 2 : 1;
}

static inline bool unicode_extended_pictographic(uint32_t cp)
{
    return UNICODE_IN_TABLE(unicode_table_extended_pictographic, cp);
}

static inline bool unicode_emoji_presentation(uint32_t cp)
{
    return UNICODE_IN_TABLE(unicode_table_emoji_presentation, cp);
}

static inline bool unicode_spacing_mark(uint32_t cp)
{
    if (cp < 0x903 || ((cp - 0xABEDu) < (0x11000u - 0xABEDu)) || cp > 0x1D281)
        return false;
    return UNICODE_IN_TABLE(unicode_table_spacing_mark, cp);
}

#undef UNICODE_IN_TABLE

#endif
