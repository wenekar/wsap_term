#include "ui/text_break.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <stdint.h>

/* BEGIN GENERATED UAX29 TABLES */
/* Generated from Unicode 17.0.0 by tools/dev/gen_uax29.py. */
/* Static table footprint: 18104 bytes (17.68 KiB). */

enum uax29_gcb {
    GCB_OTHER = 0,
    GCB_CONTROL = 1,
    GCB_CR = 2,
    GCB_EXTEND = 3,
    GCB_L = 4,
    GCB_LF = 5,
    GCB_LV = 6,
    GCB_LVT = 7,
    GCB_T = 8,
    GCB_V = 9,
    GCB_SPACING_MARK = 10,
    GCB_PREPEND = 11,
    GCB_RI = 12,
    GCB_ZWJ = 13,
};

enum uax29_wb {
    WB_OTHER = 0,
    WB_ALETTER = 1,
    WB_FORMAT = 2,
    WB_KATAKANA = 3,
    WB_MIDLETTER = 4,
    WB_MIDNUM = 5,
    WB_NUMERIC = 6,
    WB_EXTENDNUMLET = 7,
    WB_CR = 8,
    WB_EXTEND = 9,
    WB_LF = 10,
    WB_MIDNUMLET = 11,
    WB_NEWLINE = 12,
    WB_RI = 13,
    WB_HEBREW_LETTER = 14,
    WB_SINGLE_QUOTE = 15,
    WB_DOUBLE_QUOTE = 16,
    WB_ZWJ = 17,
    WB_WSEGSPACE = 18,
    WB_NONE = 255,
};

enum uax29_incb {
    INCB_NONE = 0,
    INCB_CONSONANT = 1,
    INCB_EXTEND = 2,
    INCB_LINKER = 3,
};

enum {
    UAX29_PROP_EXT_PICT = 1u << 0,
    UAX29_PROP_IS_WORD = 1u << 1,
};

struct uax29_prop {
    uint8_t gcb;
    uint8_t wb;
    uint8_t incb;
    uint8_t flags;
};

static const struct uax29_prop uax29_props[38] = {
    { GCB_CONTROL, WB_OTHER, INCB_NONE, 0 },
    { GCB_LF, WB_LF, INCB_NONE, 0 },
    { GCB_CONTROL, WB_NEWLINE, INCB_NONE, 0 },
    { GCB_CR, WB_CR, INCB_NONE, 0 },
    { GCB_OTHER, WB_WSEGSPACE, INCB_NONE, 0 },
    { GCB_OTHER, WB_OTHER, INCB_NONE, 0 },
    { GCB_OTHER, WB_DOUBLE_QUOTE, INCB_NONE, 0 },
    { GCB_OTHER, WB_SINGLE_QUOTE, INCB_NONE, 0 },
    { GCB_OTHER, WB_MIDNUM, INCB_NONE, 0 },
    { GCB_OTHER, WB_MIDNUMLET, INCB_NONE, 0 },
    { GCB_OTHER, WB_NUMERIC, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_OTHER, WB_MIDLETTER, INCB_NONE, 0 },
    { GCB_OTHER, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_OTHER, WB_EXTENDNUMLET, INCB_NONE, 0 },
    { GCB_OTHER, WB_OTHER, INCB_NONE, UAX29_PROP_EXT_PICT },
    { GCB_CONTROL, WB_FORMAT, INCB_NONE, 0 },
    { GCB_EXTEND, WB_EXTEND, INCB_EXTEND, 0 },
    { GCB_OTHER, WB_HEBREW_LETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_PREPEND, WB_NUMERIC, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_PREPEND, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_SPACING_MARK, WB_EXTEND, INCB_NONE, 0 },
    { GCB_OTHER, WB_ALETTER, INCB_CONSONANT, UAX29_PROP_IS_WORD },
    { GCB_EXTEND, WB_EXTEND, INCB_LINKER, 0 },
    { GCB_OTHER, WB_OTHER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_SPACING_MARK, WB_OTHER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_OTHER, WB_OTHER, INCB_CONSONANT, UAX29_PROP_IS_WORD },
    { GCB_OTHER, WB_EXTEND, INCB_NONE, 0 },
    { GCB_L, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_V, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_T, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_EXTEND, WB_EXTEND, INCB_NONE, 0 },
    { GCB_ZWJ, WB_ZWJ, INCB_EXTEND, 0 },
    { GCB_OTHER, WB_ALETTER, INCB_NONE, UAX29_PROP_EXT_PICT | UAX29_PROP_IS_WORD },
    { GCB_OTHER, WB_KATAKANA, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_LV, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_LVT, WB_ALETTER, INCB_NONE, UAX29_PROP_IS_WORD },
    { GCB_EXTEND, WB_EXTEND, INCB_EXTEND, UAX29_PROP_IS_WORD },
    { GCB_RI, WB_RI, INCB_NONE, 0 },
};

static const uint8_t uax29_stage0[2176] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 27, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    28, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 33, 34, 35,
    36, 37, 38, 39, 33, 34, 35, 36, 37, 38, 39, 40, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    28, 62, 63, 41, 41, 41, 41, 64, 28, 28, 65, 28, 28, 28, 28, 28,
    28, 66, 28, 67, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    68, 41, 41, 41, 28, 69, 70, 71, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 72, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 73, 74, 75, 41, 41, 41, 41, 76, 41,
    41, 41, 41, 41, 41, 41, 77, 78, 79, 80, 81, 82, 41, 83, 41, 84,
    85, 86, 87, 88, 89, 41, 41, 90, 91, 92, 93, 94, 95, 96, 97, 98,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 99, 26, 26, 26, 26, 26, 26, 26, 26, 100, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 101, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 102, 26, 103, 41, 41, 41, 41, 26, 104, 41, 41,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 105, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 106, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    107, 108, 108, 108, 108, 108, 108, 108, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
};

static const uint16_t uax29_stage1[3488] = {
    0x000u, 0x001u, 0x002u, 0x003u, 0x004u, 0x005u, 0x004u, 0x006u, 0x007u, 0x001u, 0x008u, 0x009u,
    0x00Au, 0x00Bu, 0x00Au, 0x00Bu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Cu, 0x00Au, 0x00Au,
    0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Eu, 0x00Fu, 0x00Au, 0x010u, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x011u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x012u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x004u,
    0x00Au, 0x013u, 0x00Au, 0x00Au, 0x014u, 0x015u, 0x00Du, 0x016u, 0x017u, 0x018u, 0x019u, 0x01Au,
    0x01Bu, 0x01Cu, 0x00Au, 0x00Au, 0x01Du, 0x00Du, 0x01Eu, 0x01Fu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x020u, 0x021u, 0x022u, 0x023u, 0x024u, 0x00Au, 0x00Du, 0x025u, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x026u, 0x027u, 0x028u, 0x00Au, 0x01Du, 0x029u, 0x00Au, 0x02Au, 0x02Bu, 0x02Cu,
    0x00Au, 0x02Du, 0x02Eu, 0x00Au, 0x02Fu, 0x030u, 0x00Au, 0x00Au, 0x031u, 0x00Du, 0x032u, 0x00Du,
    0x033u, 0x034u, 0x035u, 0x036u, 0x037u, 0x038u, 0x039u, 0x03Au, 0x03Bu, 0x03Cu, 0x03Du, 0x03Eu,
    0x03Fu, 0x040u, 0x039u, 0x041u, 0x042u, 0x043u, 0x044u, 0x045u, 0x046u, 0x047u, 0x048u, 0x049u,
    0x04Au, 0x04Bu, 0x03Du, 0x04Cu, 0x04Du, 0x04Eu, 0x039u, 0x04Fu, 0x050u, 0x03Cu, 0x03Du, 0x051u,
    0x052u, 0x053u, 0x039u, 0x054u, 0x055u, 0x056u, 0x057u, 0x058u, 0x059u, 0x05Au, 0x048u, 0x02Cu,
    0x05Bu, 0x05Cu, 0x03Du, 0x05Du, 0x05Eu, 0x05Fu, 0x039u, 0x02Cu, 0x060u, 0x061u, 0x044u, 0x062u,
    0x063u, 0x064u, 0x039u, 0x065u, 0x066u, 0x05Cu, 0x035u, 0x067u, 0x068u, 0x069u, 0x039u, 0x06Au,
    0x06Bu, 0x06Cu, 0x00Au, 0x06Du, 0x06Eu, 0x06Fu, 0x048u, 0x070u, 0x071u, 0x072u, 0x072u, 0x073u,
    0x074u, 0x075u, 0x02Cu, 0x02Cu, 0x076u, 0x072u, 0x077u, 0x078u, 0x079u, 0x07Au, 0x02Cu, 0x02Cu,
    0x04Eu, 0x07Bu, 0x075u, 0x07Cu, 0x02Fu, 0x00Au, 0x07Du, 0x07Eu, 0x07Fu, 0x080u, 0x00Du, 0x081u,
    0x082u, 0x02Cu, 0x02Cu, 0x02Cu, 0x083u, 0x083u, 0x084u, 0x085u, 0x075u, 0x086u, 0x087u, 0x088u,
    0x089u, 0x08Au, 0x00Au, 0x00Au, 0x08Bu, 0x00Au, 0x00Au, 0x08Cu, 0x08Du, 0x08Du, 0x08Du, 0x08Du,
    0x08Du, 0x08Du, 0x08Eu, 0x08Eu, 0x08Eu, 0x08Eu, 0x08Fu, 0x090u, 0x090u, 0x090u, 0x090u, 0x090u,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x091u, 0x092u, 0x00Au, 0x00Au, 0x091u, 0x00Au, 0x00Au, 0x093u,
    0x094u, 0x00Bu, 0x00Au, 0x00Au, 0x00Au, 0x094u, 0x00Au, 0x00Au, 0x00Au, 0x095u, 0x02Cu, 0x02Cu,
    0x00Au, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x096u, 0x004u, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x097u, 0x00Au,
    0x098u, 0x02Eu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x099u, 0x09Au, 0x00Au, 0x09Bu, 0x00Au, 0x09Cu,
    0x00Au, 0x09Du, 0x09Eu, 0x09Fu, 0x083u, 0x083u, 0x083u, 0x0A0u, 0x0A1u, 0x0A2u, 0x075u, 0x02Cu,
    0x0A3u, 0x075u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x09Au, 0x0A4u, 0x00Au, 0x0A5u, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x0A6u, 0x00Au, 0x0A7u, 0x0A8u, 0x0A9u, 0x048u, 0x072u, 0x0AAu, 0x0ABu,
    0x072u, 0x072u, 0x0ACu, 0x072u, 0x0ADu, 0x0AEu, 0x02Cu, 0x02Cu, 0x00Au, 0x0AFu, 0x083u, 0x083u,
    0x083u, 0x0B0u, 0x0B1u, 0x0B2u, 0x075u, 0x075u, 0x0B3u, 0x00Du, 0x00Du, 0x0B4u, 0x0B5u, 0x02Cu,
    0x0B6u, 0x0B7u, 0x035u, 0x0B8u, 0x0B9u, 0x075u, 0x0BAu, 0x0BBu, 0x0BCu, 0x035u, 0x0BDu, 0x0BEu,
    0x00Au, 0x00Au, 0x0BFu, 0x0BBu, 0x00Au, 0x00Au, 0x0C0u, 0x0C1u, 0x0C2u, 0x028u, 0x00Au, 0x0C3u,
    0x02Eu, 0x00Au, 0x00Au, 0x0C4u, 0x02Cu, 0x0C5u, 0x0C6u, 0x0C7u, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Du, 0x00Du, 0x00Du, 0x00Du,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x096u, 0x00Au, 0x00Au, 0x096u, 0x0C8u, 0x00Au, 0x0C3u,
    0x00Au, 0x00Au, 0x00Au, 0x0C9u, 0x0CAu, 0x0CBu, 0x07Du, 0x0CAu, 0x0CCu, 0x0CDu, 0x0CEu, 0x0CFu,
    0x0D0u, 0x0D1u, 0x0D2u, 0x0D3u, 0x02Cu, 0x07Du, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Du, 0x00Du, 0x0D4u,
    0x0D5u, 0x0D6u, 0x0D7u, 0x0D8u, 0x0D9u, 0x02Cu, 0x00Au, 0x00Au, 0x09Au, 0x0DAu, 0x0DBu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x0DCu, 0x0DDu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x0DEu, 0x02Cu, 0x0DFu, 0x0E0u,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x0E1u,
    0x0E2u, 0x00Au, 0x0E3u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x0DCu, 0x0E4u, 0x0E5u, 0x02Cu, 0x02Cu, 0x0E6u, 0x0E7u, 0x0E8u, 0x0E9u, 0x0EAu,
    0x0EBu, 0x0ECu, 0x0EDu, 0x0EEu, 0x02Cu, 0x0EFu, 0x0F0u, 0x0F1u, 0x0F2u, 0x0F3u, 0x0DBu, 0x0F4u,
    0x0F5u, 0x0F6u, 0x0F7u, 0x0F8u, 0x0F9u, 0x0FAu, 0x0F8u, 0x02Cu, 0x02Cu, 0x0FBu, 0x0FCu, 0x0FDu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x0FEu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x0FBu, 0x0FFu, 0x02Cu, 0x02Cu, 0x02Cu, 0x100u, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x101u, 0x102u,
    0x00Au, 0x00Au, 0x08Bu, 0x00Au, 0x00Au, 0x00Au, 0x103u, 0x104u, 0x00Au, 0x105u, 0x106u, 0x106u,
    0x106u, 0x106u, 0x00Du, 0x00Du, 0x02Cu, 0x02Cu, 0x107u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x108u, 0x02Cu, 0x109u, 0x10Au, 0x071u, 0x072u, 0x072u, 0x072u, 0x072u, 0x10Bu, 0x10Cu, 0x10Cu,
    0x10Cu, 0x10Cu, 0x10Cu, 0x10Du, 0x10Eu, 0x00Au, 0x00Au, 0x004u, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x0A7u, 0x02Cu, 0x00Au, 0x00Au, 0x02Cu, 0x02Cu, 0x02Cu, 0x10Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x10Fu, 0x02Cu, 0x02Cu, 0x02Cu, 0x10Cu, 0x10Cu, 0x110u,
    0x10Cu, 0x10Cu, 0x10Cu, 0x10Cu, 0x10Cu, 0x111u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x07Du, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x0C3u, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x07Du, 0x00Au, 0x112u, 0x02Cu, 0x00Au, 0x00Au, 0x113u, 0x114u, 0x00Au, 0x115u, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x116u, 0x117u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x07Du, 0x02Cu, 0x004u, 0x118u, 0x00Au, 0x119u, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x11Au, 0x11Bu, 0x00Au, 0x00Au, 0x11Cu, 0x11Du, 0x075u, 0x00Du, 0x11Eu,
    0x028u, 0x00Au, 0x11Fu, 0x00Au, 0x120u, 0x121u, 0x08Du, 0x122u, 0x123u, 0x035u, 0x035u, 0x124u,
    0x125u, 0x075u, 0x126u, 0x127u, 0x00Au, 0x00Au, 0x128u, 0x129u, 0x12Au, 0x075u, 0x083u, 0x12Bu,
    0x072u, 0x072u, 0x072u, 0x12Cu, 0x12Du, 0x12Eu, 0x12Fu, 0x130u, 0x131u, 0x132u, 0x106u, 0x00Au,
    0x00Au, 0x00Au, 0x0E3u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x035u, 0x133u, 0x134u, 0x075u,
    0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u,
    0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u,
    0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u,
    0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u,
    0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u,
    0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u,
    0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u,
    0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u,
    0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u,
    0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u,
    0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u,
    0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u,
    0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u,
    0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u,
    0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u,
    0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u,
    0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u,
    0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u,
    0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u,
    0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u,
    0x137u, 0x138u, 0x137u, 0x139u, 0x137u, 0x135u, 0x136u, 0x137u, 0x138u, 0x137u, 0x13Au, 0x08Eu,
    0x13Bu, 0x090u, 0x090u, 0x13Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x0AAu, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x0ADu, 0x02Cu, 0x02Cu,
    0x105u, 0x13Du, 0x13Eu, 0x13Fu, 0x140u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x141u,
    0x02Cu, 0x142u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x0C3u,
    0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x143u, 0x00Au, 0x00Au, 0x144u, 0x02Cu, 0x02Cu, 0x145u,
    0x00Du, 0x146u, 0x00Du, 0x147u, 0x148u, 0x149u, 0x02Cu, 0x14Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x14Bu, 0x14Cu, 0x003u, 0x004u, 0x005u, 0x004u, 0x02Eu, 0x14Du, 0x10Cu,
    0x10Cu, 0x14Eu, 0x00Au, 0x0A7u, 0x14Fu, 0x150u, 0x02Cu, 0x151u, 0x152u, 0x00Au, 0x00Bu, 0x153u,
    0x0C3u, 0x0C3u, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x02Eu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x154u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x155u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x00Au, 0x07Du, 0x00Au, 0x00Au, 0x00Au, 0x04Eu, 0x0D4u, 0x02Cu, 0x00Au, 0x00Au, 0x156u, 0x00Au,
    0x02Eu, 0x00Au, 0x00Au, 0x157u, 0x00Au, 0x0C3u, 0x00Au, 0x00Au, 0x158u, 0x159u, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x0C3u, 0x075u, 0x00Au,
    0x00Au, 0x158u, 0x00Au, 0x145u, 0x00Au, 0x00Au, 0x144u, 0x00Au, 0x00Au, 0x00Au, 0x11Au, 0x08Cu,
    0x08Cu, 0x15Au, 0x010u, 0x15Bu, 0x00Au, 0x00Au, 0x00Au, 0x11Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x105u, 0x00Au, 0x0A6u, 0x144u, 0x02Cu, 0x011u, 0x00Au, 0x00Au, 0x15Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x15Du, 0x00Au, 0x00Au, 0x15Eu, 0x00Au, 0x0A6u, 0x00Au, 0x105u,
    0x00Au, 0x0A7u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x15Fu, 0x00Au, 0x0A6u, 0x00Au, 0x0E3u,
    0x00Au, 0x0E3u, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x160u, 0x161u, 0x035u, 0x162u, 0x02Cu, 0x02Cu, 0x00Au, 0x07Du, 0x00Au, 0x07Du, 0x02Cu, 0x02Cu,
    0x02Fu, 0x00Au, 0x163u, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x0A6u, 0x00Au, 0x0A6u, 0x00Au, 0x164u,
    0x00Au, 0x141u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x09Au, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x164u, 0x00Au, 0x00Au, 0x00Au, 0x164u,
    0x00Au, 0x00Au, 0x165u, 0x075u, 0x028u, 0x00Au, 0x166u, 0x00Au, 0x0A6u, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x167u, 0x141u, 0x168u, 0x02Cu, 0x02Cu, 0x169u, 0x00Au, 0x07Du, 0x16Au, 0x00Au,
    0x026u, 0x0D4u, 0x02Cu, 0x00Au, 0x16Bu, 0x02Cu, 0x02Cu, 0x00Au, 0x154u, 0x02Cu, 0x00Au, 0x105u,
    0x16Cu, 0x00Au, 0x00Au, 0x16Du, 0x16Eu, 0x02Cu, 0x048u, 0x16Fu, 0x170u, 0x00Au, 0x00Au, 0x171u,
    0x172u, 0x00Au, 0x09Au, 0x075u, 0x173u, 0x035u, 0x174u, 0x175u, 0x176u, 0x00Au, 0x00Au, 0x177u,
    0x170u, 0x00Au, 0x00Au, 0x178u, 0x179u, 0x17Au, 0x02Cu, 0x02Cu, 0x00Au, 0x010u, 0x17Bu, 0x17Cu,
    0x17Du, 0x02Cu, 0x02Cu, 0x02Cu, 0x17Eu, 0x17Fu, 0x09Au, 0x00Au, 0x00Au, 0x113u, 0x180u, 0x075u,
    0x181u, 0x043u, 0x044u, 0x182u, 0x183u, 0x184u, 0x185u, 0x186u, 0x187u, 0x035u, 0x035u, 0x188u,
    0x189u, 0x18Au, 0x18Bu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x18Cu, 0x18Du, 0x18Eu, 0x141u, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x18Fu, 0x190u, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x113u, 0x191u, 0x0D4u, 0x192u, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x193u, 0x194u, 0x075u, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x195u, 0x196u,
    0x075u, 0x197u, 0x198u, 0x02Cu, 0x072u, 0x199u, 0x19Au, 0x075u, 0x19Bu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x17Bu, 0x19Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x075u, 0x107u,
    0x19Du, 0x19Eu, 0x035u, 0x19Fu, 0x1A0u, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1A1u, 0x00Au,
    0x00Au, 0x1A2u, 0x1A3u, 0x02Cu, 0x1A4u, 0x035u, 0x035u, 0x1A5u, 0x1A6u, 0x1A7u, 0x035u, 0x035u,
    0x1A8u, 0x1A9u, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x09Au, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x1AAu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x04Eu, 0x075u,
    0x044u, 0x00Au, 0x1ABu, 0x1ACu, 0x04Eu, 0x075u, 0x02Cu, 0x143u, 0x00Au, 0x1ADu, 0x1AEu, 0x1AFu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1B0u, 0x00Au, 0x00Au, 0x1B1u, 0x1B2u, 0x075u, 0x1B3u, 0x00Au,
    0x1B4u, 0x1B5u, 0x075u, 0x00Au, 0x00Au, 0x145u, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x1B6u,
    0x1B7u, 0x1B8u, 0x035u, 0x1B9u, 0x1BAu, 0x1BBu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x04Eu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x0E3u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x0A7u, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x11Au, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x04Eu, 0x00Au, 0x00Au, 0x00Au, 0x1BCu, 0x1BDu, 0x1BEu, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x02Eu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x105u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x115u, 0x1BFu, 0x075u,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x09Au, 0x00Au, 0x0A7u, 0x075u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x0A7u,
    0x075u, 0x00Au, 0x0C3u, 0x186u, 0x00Au, 0x00Au, 0x00Au, 0x16Eu, 0x11Au, 0x075u, 0x142u, 0x1C0u,
    0x00Au, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x1C1u, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x02Cu, 0x02Cu, 0x00Au, 0x1C2u, 0x00Au, 0x11Au, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x1C3u, 0x1C4u, 0x1C5u, 0x1C5u, 0x1C6u, 0x1C7u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1C8u, 0x1C9u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x1CAu, 0x02Cu, 0x1CBu, 0x072u, 0x1CCu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x1CDu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x1CEu, 0x1CFu, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x1D0u, 0x1D1u,
    0x02Cu, 0x1D2u, 0x1D3u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x0ACu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x02Eu, 0x07Du, 0x09Au, 0x1D4u, 0x1D5u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Du, 0x00Du, 0x0B4u, 0x00Du, 0x16Eu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1D6u, 0x1D7u, 0x1D8u, 0x02Cu, 0x1D9u, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1DAu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x14Au, 0x00Au, 0x00Au, 0x00Au, 0x09Eu, 0x1DBu, 0x1DCu,
    0x1DDu, 0x00Au, 0x00Au, 0x00Au, 0x1DEu, 0x1DFu, 0x00Au, 0x1E0u, 0x1E1u, 0x061u, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x1E2u, 0x00Au, 0x061u, 0x08Cu, 0x00Au, 0x08Cu,
    0x00Au, 0x14Au, 0x00Au, 0x14Au, 0x0A7u, 0x00Au, 0x0A7u, 0x00Au, 0x044u, 0x00Au, 0x044u, 0x00Au,
    0x1E3u, 0x197u, 0x197u, 0x197u, 0x00Du, 0x00Du, 0x00Du, 0x1E4u, 0x00Du, 0x00Du, 0x081u, 0x1E5u,
    0x1E6u, 0x0BAu, 0x015u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x0A7u, 0x1E7u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1E8u, 0x1E9u, 0x1EAu, 0x00Au,
    0x00Au, 0x00Au, 0x0C3u, 0x02Cu, 0x104u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x00Au, 0x00Au, 0x07Du, 0x1EBu, 0x1ECu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x00Au, 0x1EDu, 0x02Cu, 0x00Au, 0x00Au, 0x1EEu, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x00Au, 0x1EEu, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x115u, 0x1EFu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x0A7u, 0x1F0u, 0x1F1u,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x1F2u, 0x0A7u, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x00Au, 0x00Au, 0x00Au, 0x00Au, 0x154u, 0x16Eu, 0x02Cu, 0x02Cu, 0x00Au, 0x00Au, 0x00Au, 0x00Au,
    0x1F3u, 0x075u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x1DDu, 0x00Au, 0x1F4u, 0x1F5u, 0x1F6u, 0x1F7u, 0x1F8u, 0x1F9u, 0x1FAu, 0x145u, 0x1FBu, 0x145u,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1FCu, 0x02Cu, 0x1FDu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x1FEu, 0x0DEu, 0x0E5u, 0x0FDu, 0x0E5u, 0x02Cu, 0x1FFu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x00Au, 0x0E3u, 0x00Au, 0x0E3u, 0x200u, 0x201u, 0x202u, 0x203u, 0x204u,
    0x204u, 0x204u, 0x205u, 0x206u, 0x207u, 0x208u, 0x0DEu, 0x209u, 0x0DFu, 0x204u, 0x1FFu, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x20Au, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x20Bu, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x20Cu,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x20Du, 0x204u, 0x204u, 0x204u, 0x20Eu, 0x20Fu, 0x204u, 0x210u, 0x211u,
    0x212u, 0x213u, 0x214u, 0x215u, 0x216u, 0x217u, 0x218u, 0x219u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x02Cu, 0x02Cu, 0x02Cu, 0x204u, 0x204u, 0x204u, 0x204u, 0x21Au, 0x21Bu, 0x21Cu, 0x21Du,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x21Eu, 0x204u, 0x204u, 0x1FDu, 0x02Cu, 0x02Cu, 0x02Cu, 0x21Fu, 0x21Eu, 0x02Cu, 0x02Cu,
    0x21Fu, 0x02Cu, 0x203u, 0x1FDu, 0x220u, 0x0DFu, 0x204u, 0x204u, 0x1FDu, 0x204u, 0x204u, 0x221u,
    0x222u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x21Fu, 0x203u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x075u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u,
    0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x20Eu,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x02Cu, 0x02Cu, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x0AAu, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x0AAu, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x223u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x0AAu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x072u, 0x0AAu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x224u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u, 0x072u,
    0x072u, 0x072u, 0x072u, 0x0ADu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu,
    0x02Cu, 0x02Cu, 0x02Cu, 0x02Cu, 0x225u, 0x001u, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du,
    0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x00Du, 0x00Du, 0x00Du, 0x00Du,
    0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x00Du, 0x001u,
    0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u,
    0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u,
    0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u, 0x001u,
};

static const uint8_t uax29_stage2[8800] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  2,  3,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     4,  5,  6,  5,  5,  5,  5,  7,  5,  5,  5,  5,  8,  5,  9,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11,  8,  5,  5,  5,  5,
     5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5, 13,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  0,
     0,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 12,  5,  5, 15, 14,  5,
     5,  5,  5,  5,  5, 12,  5, 11, 12,  5, 12,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5, 12, 12,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    12, 12, 12, 12, 12,  5, 12, 12,  5,  5, 12, 12, 12, 12,  8, 12,
     5,  5,  5,  5,  5,  5, 12, 11, 12, 12, 12,  5, 12,  5, 12, 12,
    12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12,  5, 16, 16, 16, 16, 16, 16, 16, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12,  5, 12, 11,
    12, 12, 12, 12, 12, 12, 12, 12, 12,  8, 12,  5,  5,  5,  5,  5,
     5, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5, 16,
     5, 16, 16,  5, 16, 16,  5, 16,  5,  5,  5,  5,  5,  5,  5,  5,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,  5,  5,  5,  5, 17,
    17, 17, 17, 12, 11,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    18, 18, 18, 18, 18, 18,  5,  5,  5,  5,  5,  5,  8,  8,  5,  5,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5, 15,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5, 10,  8,  5, 12, 12,
    16, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12,  5, 12, 16, 16, 16, 16, 16, 16, 16, 18,  5, 16,
    16, 16, 16, 16, 16, 12, 12, 16, 16,  5, 16, 16, 16, 16, 12, 12,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 12, 12, 12,  5,  5, 12,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 19,
    12, 16, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 12, 12, 12, 12, 12, 12,
    16, 16, 16, 16, 12, 12,  5,  5,  8,  5, 12,  5,  5, 16,  5,  5,
    12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 12, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 12, 16, 16, 16, 12, 16, 16, 16, 16, 16,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12,
    18, 18,  5,  5,  5,  5,  5, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16,
    16, 16, 18, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 20, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 20, 16, 12, 20, 20,
    20, 16, 16, 16, 16, 16, 16, 16, 16, 20, 20, 20, 20, 22, 20, 20,
    12, 16, 16, 16, 16, 16, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21,
    12, 12, 16, 16,  5,  5, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
     5, 12, 12, 12, 12, 12, 12, 12, 21, 21, 21, 21, 21, 21, 21, 21,
    12, 16, 20, 20,  5, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12,
    12,  5,  5, 12, 12, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21,  5, 21, 21, 21, 21, 21, 21,
    21,  5, 21,  5,  5,  5, 21, 21, 21, 21,  5,  5, 16, 12, 16, 20,
    20, 16, 16, 16, 16,  5,  5, 20, 20,  5,  5, 20, 20, 22, 12,  5,
     5,  5,  5,  5,  5,  5,  5, 16,  5,  5,  5,  5, 21, 21,  5, 21,
    21, 21,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12,  5, 16,  5,
     5, 16, 16, 20,  5, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5, 12,
    12,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12,
    12,  5, 12, 12,  5, 12, 12,  5, 12, 12,  5,  5, 16,  5, 20, 20,
    20, 16, 16,  5,  5,  5,  5, 16, 16,  5,  5, 16, 16, 16,  5,  5,
     5, 16,  5,  5,  5,  5,  5,  5,  5, 12, 12, 12, 12,  5, 12,  5,
     5,  5,  5,  5,  5,  5, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    16, 16, 12, 12, 12, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 16, 16, 20,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12,
    12, 12,  5, 12, 12, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21,  5, 21, 21,  5, 21, 21, 21, 21, 21,  5,  5, 16, 12, 20, 20,
    20, 16, 16, 16, 16, 16,  5, 16, 16, 20,  5, 20, 20, 22,  5,  5,
    12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5, 21, 16, 16, 16, 16, 16, 16,
     5, 16, 20, 20,  5, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12,
    21,  5, 21, 21,  5, 21, 21, 21, 21, 21,  5,  5, 16, 12, 16, 16,
    20, 16, 16, 16, 16,  5,  5, 20, 20,  5,  5, 20, 20, 22,  5,  5,
     5,  5,  5,  5,  5, 16, 16, 16,  5,  5,  5,  5, 21, 21,  5, 21,
     5, 21,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5, 16, 12,  5, 12, 12, 12, 12, 12, 12,  5,  5,  5, 12, 12,
    12,  5, 12, 12, 12, 12,  5,  5,  5, 12, 12,  5, 12,  5, 12, 12,
     5,  5,  5, 12, 12,  5,  5,  5, 12, 12, 12,  5,  5,  5, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5, 16, 20,
    16, 20, 20,  5,  5,  5, 20, 20, 20,  5, 20, 20, 20, 16,  5,  5,
    12,  5,  5,  5,  5,  5,  5, 16,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 20, 20, 20, 16, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,
    12,  5, 12, 12, 12, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21,  5,  5, 16, 12, 16, 16,
    16, 20, 20, 20, 20,  5, 16, 16, 16,  5, 16, 16, 16, 22,  5,  5,
     5,  5,  5,  5,  5, 16, 16,  5, 21, 21, 21,  5, 12, 12,  5,  5,
    12, 16, 20, 20,  5, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,
    12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12,  5, 12, 12, 12, 12, 12,  5,  5, 16, 12, 20, 16,
    16, 20, 16, 20, 20,  5, 16, 16, 16,  5, 16, 16, 16, 16,  5,  5,
     5,  5,  5,  5,  5, 16, 16,  5,  5,  5,  5,  5, 12, 12, 12,  5,
     5, 12, 12, 20,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 20, 20, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 12, 16, 20,
    20, 16, 16, 16, 16,  5, 20, 20, 20,  5, 20, 20, 20, 22, 19,  5,
     5,  5,  5,  5, 12, 12, 12, 16,  5,  5,  5,  5,  5,  5,  5, 12,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12, 12, 12, 12, 12, 12,
     5, 16, 20, 20,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12,  5,  5,  5, 12, 12, 12, 12, 12, 12,
    12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12,  5,  5,
    12, 12, 12, 12, 12, 12, 12,  5,  5,  5, 16,  5,  5,  5,  5, 16,
    20, 20, 16, 16, 16,  5, 16,  5, 20, 20, 20, 20, 20, 20, 20, 16,
     5,  5, 20, 20,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 16, 23, 24, 16, 16, 16, 16, 16, 16, 16,  5,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23, 16, 16, 16, 16, 16, 16, 16, 16,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5,  5,  5,  5,  5,
     5, 23, 23,  5, 23,  5, 23, 23, 23, 23, 23,  5, 23, 23, 23, 23,
    23, 23, 23, 23,  5, 23,  5, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 16, 23, 24, 16, 16, 16, 16, 16, 16, 16, 16, 16, 23,  5,  5,
    23, 23, 23, 23, 23,  5, 23,  5, 16, 16, 16, 16, 16, 16, 16,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5, 23, 23, 23, 23,
     5,  5,  5,  5,  5,  5,  5,  5, 16, 16,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5, 16,  5, 16,  5, 16,  5,  5,  5,  5, 20, 20,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,
     5, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 20,
    16, 16, 16, 16, 16,  5, 16, 16, 12, 12, 12, 12, 12, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,  5, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5,  5,
     5,  5,  5,  5,  5,  5, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 16, 16, 16,
    16, 20, 16, 16, 16, 16, 16, 16, 26, 22, 16, 20, 20, 16, 16, 25,
    25, 25, 25, 25, 25, 25, 20, 20, 16, 16, 25, 25, 25, 25, 16, 16,
    16, 25, 26, 26, 26, 25, 25, 26, 26, 26, 26, 26, 26, 26, 25, 25,
    25, 16, 16, 16, 16, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 16, 26, 20, 16, 16, 26, 26, 26, 26, 26, 26, 16, 25, 26,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 26, 26, 26, 16,  5,  5,
    12, 12, 12, 12, 12, 12,  5, 12,  5,  5,  5,  5,  5, 12,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12,  5,  5,
    12, 12, 12, 12, 12, 12, 12,  5, 12,  5, 12, 12, 12, 12,  5,  5,
    12,  5, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12, 12,  5,
    12,  5, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 16, 16, 16,
    12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12,
     4, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12,
    12, 12, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,
    12,  5, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    25, 25, 25, 25, 16, 16, 20, 16, 16, 16, 16, 16, 16, 16, 20, 20,
    20, 20, 20, 20, 20, 20, 16, 20, 20, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 22, 16,  5,  5,  5, 23,  5,  5,  5,  5, 23, 16,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16, 16, 16, 15, 16,
    12, 12, 12, 12, 12, 16, 16, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 12,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,
    16, 16, 16, 20, 20, 20, 20, 16, 16, 20, 20, 20,  5,  5,  5,  5,
    20, 20, 16, 20, 20, 20, 20, 20, 20, 16, 16, 16,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,  5,  5,
    23, 23, 23, 23, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23,  5,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 16, 16, 20, 20, 16,  5,  5,  5,  5,
    25, 25, 25, 25, 25, 20, 16, 20, 16, 16, 16, 16, 16, 16, 16,  5,
    22, 26, 16, 26, 26, 16, 16, 16, 16, 16, 16, 16, 16, 20, 20, 20,
    20, 20, 20, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5, 16,
     5,  5,  5,  5,  5,  5,  5, 23,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5,  5,  5,
    16, 16, 16, 16, 20, 12, 12, 12, 12, 12, 12, 21, 21, 12, 12, 12,
    12, 12, 12, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 20, 20,
    20, 20, 16, 16, 22, 21, 21, 21, 21, 21, 21, 21, 21,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16, 16, 16, 16, 16,
    16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 20, 16, 16, 16, 16, 20, 20, 16, 16, 16, 22, 16, 16, 21, 21,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 12, 21, 21, 21, 12, 12,
    12, 12, 12, 12, 12, 12, 16, 20, 16, 16, 20, 20, 20, 16, 20, 16,
    12, 12, 12, 12, 20, 20, 20, 20, 20, 20, 20, 20, 16, 16, 16, 16,
    16, 16, 16, 16, 20, 20, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5,  5, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12,
    16, 16, 16,  5, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 20, 16, 16, 16, 16, 16, 16, 16, 12, 12, 12, 12, 16, 12, 12,
    12, 12, 12, 12, 16, 12, 12, 20, 16, 16, 12,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12,  5, 12,  5, 12,  5, 12,  5, 12,
    12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12,  5, 12,  5,
     5,  5, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,
    12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,
     4,  4,  4,  4,  4,  4,  4,  5,  4,  4,  4,  0, 30, 31, 15, 15,
     5,  5,  5,  5,  5,  5,  5,  5,  9,  9,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  9,  5,  5, 11,  2,  2, 15, 15, 15, 15, 15, 13,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,  5,  5, 13,
    13,  5,  5,  5,  8,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5, 13,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,
    15, 15, 15, 15, 15,  0, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
     5, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12,
    16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5, 12,  5,  5,  5,  5, 12,  5,  5, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12,  5, 12,  5,  5,  5, 12, 12, 12, 12, 12,  5,  5,
     5,  5, 14,  5, 12,  5, 12,  5, 12,  5, 12, 12, 12, 12,  5, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 32,  5,  5, 12, 12, 12, 12,
     5,  5,  5,  5,  5, 12, 12, 12, 12, 12,  5,  5,  5,  5, 12,  5,
     5,  5,  5,  5, 14, 14, 14, 14, 14, 14,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,
     5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14,  5,  5,  5,  5, 14, 14, 14,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 32, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14,  5,
    14, 14, 14, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,  5,
     5, 14,  5,  5, 14, 14,  5,  5, 14,  5,  5,  5,  5, 14,  5,  5,
    14,  5, 14, 14,  5,  5, 14,  5,  5,  5, 14,  5,  5,  5, 14, 14,
     5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14,  5,  5,  5,  5,  5,
    14,  5, 14,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,
    14,  5,  5, 14,  5, 14, 14,  5, 14,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,  5,  5, 14, 14,
     5,  5, 14, 14, 14, 14, 14, 14,  5, 14,  5, 14, 14,  5,  5,  5,
    14, 14,  5,  5,  5,  5,  5, 14,  5,  5, 14, 14,  5,  5,  5,  5,
    14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14,  5,
     5,  5,  5,  5, 14, 14,  5,  5, 14,  5,  5,  5,  5,  5, 14, 14,
     5, 14,  5, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    14, 14, 14, 14, 14, 14,  5, 14, 14, 14, 14,  5,  5, 14,  5,  5,
     5,  5, 14,  5,  5, 14,  5,  5, 14, 14, 14, 14, 14, 14,  5, 14,
     5,  5, 14,  5, 14,  5, 14,  5,  5,  5,  5,  5,  5, 14,  5,  5,
     5, 14,  5,  5,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5, 14,  5,  5, 14,  5,  5,  5,  5, 14,  5, 14,  5,
     5,  5,  5, 14, 14, 14,  5, 14,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5, 14, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,
     5,  5,  5,  5, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14,  5,  5,  5,
    14,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5, 12, 12, 12, 12, 16,
    16, 16, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5, 12,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16,
    12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12,
     4,  5,  5,  5,  5, 12, 23, 23,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 23, 23, 23, 23, 23, 23, 23, 23, 23, 16, 16, 16, 16, 16, 16,
    14, 33, 33, 33, 33, 33,  5,  5, 23, 23, 23, 12, 12, 14,  5,  5,
    23, 23, 23, 23, 23, 23, 23,  5,  5, 16, 16, 33, 33, 23, 23, 23,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,  5, 33, 33, 33, 33,
     5,  5,  5,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
     5,  5,  5,  5,  5,  5,  5, 14,  5, 14,  5,  5,  5,  5,  5,  5,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,  5,
    33, 33, 33, 33, 33, 33, 33, 33,  5,  5,  5,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 12, 12,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16,
    16, 16, 16,  5, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  5, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16,
    16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 16, 12, 12, 12, 16, 12, 12, 12, 12, 16, 12, 12, 12, 12,
    12, 12, 12, 20, 20, 16, 16, 20,  5,  5,  5,  5, 16,  5,  5,  5,
    12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    20, 20, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 12, 12, 12, 12, 12, 12,  5,  5,  5, 12,  5, 12, 12, 16,
    12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 20, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,  5,  5,  5,
    16, 16, 16, 20, 12, 12, 12, 12, 12, 21, 21, 21, 12, 12, 12, 21,
    21, 21, 21, 16, 20, 20, 16, 16, 16, 16, 20, 20, 16, 16, 20, 20,
    22,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12,
    25, 25, 25, 25, 25, 16, 23, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 25, 25, 25, 25, 25,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 20,
    20, 16, 16, 20, 20, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 16, 12, 12, 12, 12, 12, 12, 12, 12, 16, 20,  5,  5,
    23, 25, 25, 25, 23, 23, 23,  5,  5,  5, 25, 26, 16, 26, 25, 25,
    16, 23, 16, 16, 16, 23, 23, 16, 16, 23, 23, 23, 23, 23, 16, 16,
    23, 16, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 23, 23, 23,  5,  5,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 20, 16, 16, 20, 20,
     5,  5, 12, 12, 12, 20, 22,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12,  5,
     5, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 12, 12, 12, 12, 12,
    12, 12, 12, 20, 20, 16, 20, 20, 16, 20, 20,  5, 20, 16,  5,  5,
    34, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
    35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 34, 35, 35, 35,
    35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
    35, 35, 35, 35, 35, 35, 35, 35, 34, 35, 35, 35, 35, 35, 35, 35,
    35, 35, 35, 35, 34, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
    35, 35, 35, 35,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    28, 28, 28, 28, 28, 28, 28,  5,  5,  5,  5, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,  5,  5,  5,  5,
     5,  5,  5, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5, 17, 16, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17,  5, 17, 17, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17,  5, 17, 17, 17, 17, 17,  5, 17,  5,
    17, 17,  5, 17, 17,  5, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
    12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
     5,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,
     5,  5,  5, 11,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5, 13, 13,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 13, 13, 13,
     8,  5,  9,  5,  8, 11,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 15,
     5,  5,  5,  5,  5,  5,  5,  9,  5,  5,  5,  5,  8,  5,  9,  5,
     5,  5,  5,  5,  5,  5, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 16, 16,
     5,  5, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12,
     5,  5, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12,  5,  5,  5,
     0,  0,  0,  0,  0,  0,  0,  0,  0, 15, 15, 15,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,  5, 12,
    12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16,  5,  5,  5,  5,  5,
    12, 12, 12, 12,  5,  5,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12,
     5, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12,  5, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12,  5, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,  5,  5,  5,
    12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12,  5,  5, 12,  5, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12,  5, 12, 12,  5,  5,  5, 12,  5,  5, 12,
    12, 12, 12,  5, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    21, 16, 16, 16,  5, 16, 16,  5,  5,  5,  5,  5, 16, 16, 16, 16,
    21, 21, 21, 21,  5, 21, 21, 21,  5, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21,  5,  5, 16, 16, 16,  5,  5,  5,  5, 22,
    12, 12, 12, 12, 12, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12,  5,  5,  5, 16, 16, 16, 16, 16,  5, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 16, 16,  5,  5,  5,
     5,  5, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16, 16, 16, 16, 16, 16,
     5,  5,  5,  5,  5,  5,  5, 12,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    20, 16, 20, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 12, 12, 16, 16, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16,
    16, 16, 20, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    20, 20, 20, 16, 16, 16, 16, 20, 20, 16, 16,  5,  5, 18,  5,  5,
     5,  5, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 18,  5,  5,
    16, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 16, 16, 16, 16, 16, 20, 16, 16, 16,
    16, 16, 16, 22, 16,  5, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
     5,  5,  5,  5, 21, 20, 20, 21,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 16,  5,  5, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 20, 20, 20, 16, 16, 16, 16, 16, 16, 16, 16, 16, 20,
    16, 12, 19, 19, 12,  5,  5,  5,  5, 16, 16, 16, 16,  5, 20, 16,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 12,  5, 12,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 20, 20, 20, 16,
    16, 16, 20, 20, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5, 16, 12,
    12, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12,  5, 12,  5, 12, 12, 12, 12,  5, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12,
    20, 20, 20, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5,  5,  5,  5,
    16, 16, 20, 20,  5, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12,
    12,  5, 12, 12,  5, 12, 12, 12, 12, 12,  5, 16, 16, 12, 16, 20,
    16, 20, 20, 20, 20,  5,  5, 20, 20,  5,  5, 20, 20, 16,  5,  5,
    12,  5,  5,  5,  5,  5,  5, 16,  5,  5,  5,  5,  5, 12, 12, 12,
    12, 12, 20, 20,  5,  5, 16, 16, 16, 16, 16, 16, 16,  5,  5,  5,
    16, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21,  5, 21,  5,  5, 21,  5,
    21, 21, 21, 21, 21, 21,  5, 12, 16, 20, 20, 16, 16, 16, 16, 16,
    16,  5, 16,  5,  5, 16,  5, 16, 16, 16, 20,  5, 20, 20, 16, 16,
    22, 19, 16, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 20, 20, 20, 16, 16, 16, 16, 16, 16, 16, 16,
    20, 20, 16, 16, 16, 20, 16, 12, 12, 12, 12,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5,  5,  5, 16, 12,
    16, 20, 20, 16, 16, 16, 16, 16, 16, 20, 16, 20, 20, 16, 20, 16,
    16, 20, 16, 16, 12, 12,  5, 12,  5,  5,  5,  5,  5,  5,  5,  5,
    20, 20, 16, 16, 16, 16,  5,  5, 20, 20, 20, 20, 16, 16, 20, 16,
     5,  5,  5,  5,  5,  5,  5,  5, 12, 12, 12, 12, 16, 16,  5,  5,
    20, 20, 20, 16, 16, 16, 16, 16, 16, 16, 16, 20, 20, 16, 20, 16,
    16,  5,  5,  5, 12,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 20, 16, 20, 20,
    16, 16, 16, 16, 16, 16, 16, 16, 12,  5,  5,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,  5,  5, 16, 20, 16,
    26, 26, 16, 16, 16, 16, 20, 16, 16, 16, 16, 16,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 16, 16, 16, 16, 16, 16, 20, 16, 16,  5,  5,  5,  5,  5,
    21, 21, 21, 21, 21, 21, 21,  5,  5, 21,  5,  5, 21, 21, 21, 21,
    21, 21, 21, 21,  5, 21, 21,  5, 21, 21, 21, 21, 21, 21, 21, 21,
    16, 20, 20, 20, 20, 20,  5, 20, 20,  5,  5, 16, 16, 16, 22, 19,
    20, 19, 20, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12,
    12, 20, 20, 20, 16, 16, 16, 16,  5,  5, 16, 16, 20, 20, 20, 20,
    16, 12,  5, 12, 20,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    21, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 21, 21, 21, 21, 21,
    21, 21, 21, 16, 16, 16, 16, 16, 16, 20, 12, 16, 16, 16, 16,  5,
     5,  5,  5,  5,  5,  5,  5, 22,  5,  5,  5,  5,  5,  5,  5,  5,
    21, 16, 16, 16, 16, 16, 16, 20, 20, 16, 16, 16, 21, 21, 21, 21,
    21, 21, 21, 21, 19, 19, 19, 19, 19, 19, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 20, 16, 22,  5,  5,  5, 12,  5,  5,
    16, 20, 16, 16, 16, 20, 16, 20,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 20,
    16, 16, 16, 16, 16, 16, 16,  5, 16, 16, 16, 16, 16, 16, 20, 16,
     5,  5, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,  5, 20, 16, 16, 16, 16, 16, 16,
    16, 20, 16, 16, 20, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12,  5, 12, 12,  5, 12, 12, 12, 12, 12,
    12, 16, 16, 16, 16, 16, 16,  5,  5,  5, 16,  5, 16, 16,  5, 16,
    16, 16, 16, 16, 16, 16, 19, 16,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12,  5, 12, 12,  5, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 20, 20, 20, 20, 20,  5,
    16, 16,  5, 20, 20, 16, 20, 16, 12,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 16, 16, 20, 20,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 19, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21,  5, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 20, 20, 16, 16, 16, 16, 16,  5,  5,  5, 20, 20,
    16, 16, 22,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 16,  5,  5,  5,  5,  5,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    16, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 20, 20, 20, 16, 16, 16,
    12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5, 12, 12, 12,
    12, 12, 12, 28, 12, 12, 12, 28, 28, 28, 28, 12, 12,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5, 16,
    12, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20,  5,  5,  5,  5,  5,  5,  5, 16,
    16, 16, 16, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12,  5, 12, 36,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    16, 16, 23, 23, 23, 23, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,  5,
    23, 23, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    33, 33, 33, 33,  5, 33, 33, 33, 33, 33, 33, 33,  5, 33, 33,  5,
    33, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    33, 33, 33,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5, 23,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    23, 23, 23,  5,  5, 33,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5, 33, 33, 33, 33,  5,  5,  5,  5,  5,  5,  5,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5, 16, 16,  5,
    15, 15, 15, 15,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5, 16, 16, 16, 16, 16,  5,  5,  5, 16, 16, 16,
    16, 16, 16, 15, 15, 15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 16,
    16, 16, 16,  5,  5, 16, 16, 16, 16, 16, 16, 16,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 16, 16, 16, 16,  5,  5,
     5,  5, 16, 16, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5, 12,  5,  5, 12, 12,  5,  5, 12, 12, 12, 12,  5, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12,  5, 12, 12, 12,
    12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12,  5,  5, 12, 12, 12,
    12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12,  5,
    12, 12, 12, 12, 12,  5, 12,  5,  5,  5, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12,  5,  5, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12,  5, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5, 10, 10,
    16, 16, 16, 16, 16, 16, 16,  5,  5,  5,  5, 16, 16, 16, 16, 16,
     5,  5,  5,  5,  5, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5, 16,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5,  5,
    16, 16, 16, 16, 16, 16, 16,  5, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16,  5,  5, 16, 16, 16, 16, 16,
    16, 16,  5, 16, 16,  5, 16, 16, 16, 16, 16,  5,  5,  5,  5,  5,
    16, 16, 16, 16, 16, 16, 16, 12, 12, 12, 12, 12, 12, 12,  5,  5,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5,  5,  5, 12,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16,
    12, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,  5,  5,  5,  5,  5,
    12, 12, 12, 16, 12, 12, 16, 12, 12, 12, 12, 12, 12, 12, 16, 16,
    12, 12, 12, 12, 12, 16,  5,  5,  5,  5,  5,  5,  5,  5, 12, 12,
    12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12,  5, 12, 12,  5,
    12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 12,  5,  5,  5,  5,
     5, 12, 12,  5, 12,  5,  5, 12,  5, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12,  5, 12, 12, 12, 12,  5, 12,  5, 12,  5,  5,  5,  5,
     5,  5, 12,  5,  5,  5,  5, 12,  5, 12,  5, 12,  5, 12, 12, 12,
     5, 12, 12,  5, 12,  5,  5, 12,  5, 12,  5, 12,  5, 12,  5, 12,
     5, 12, 12,  5, 12,  5,  5, 12, 12, 12, 12,  5, 12, 12, 12, 12,
    12, 12, 12,  5, 12, 12, 12, 12,  5, 12, 12, 12, 12,  5, 12,  5,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12,
     5, 12, 12, 12,  5, 12, 12, 12, 12, 12,  5, 12, 12, 12, 12, 12,
     5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14,
     5,  5,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
     5,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    32, 32, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 32, 32,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12,  5,  5,  5,  5, 14,  5,
     5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
     5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,
     5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14,  5, 14, 14, 14, 14,
    14, 14,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14,  5,  5, 14, 14,  5, 14, 14, 14,  5,  5, 14, 14,
    14,  5,  5, 14, 14, 14,  5, 14, 14, 14, 14, 16, 16, 16, 16, 16,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,  5, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14,  5,
    14, 14, 14, 14, 14, 14, 14, 14,  5,  5,  5,  5,  5,  5,  5, 14,
    14,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14,  5,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5, 14,  5,  5, 14, 14, 14, 14,  5,  5,
    14,  5,  5,  5,  5, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5,  5,  5,  5, 14, 14,  5,  5, 14,  5,  5,  5,  5,  5,  5,  5,
     5, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14,  5,  5,  5,
     5,  5, 14, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
     5, 14, 14, 14,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14,  5,
     5, 14,  5, 14,  5,  5,  5,  5, 14,  5,  5,  5,  5,  5,  5, 14,
     5,  5,  5, 14,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14,
    14, 14, 14,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14,  5,  5,  5, 14,  5, 14, 14, 14, 14, 14,
    14,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14,
     5,  5,  5,  5,  5,  5,  5,  5, 14, 14, 14, 14, 14, 14, 14, 14,
     5,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,  5, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14,  5, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    23,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,  5,  5,  5,  5,  5,
     0, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};
/* END GENERATED UAX29 TABLES */

struct grapheme_state {
    uint8_t prev_gcb;
    uint8_t incb_state;
    uint8_t ep_state;
    bool ri_odd;
};

struct word_state {
    uint8_t raw_prev_wb;
    uint8_t eff_wb;
    uint8_t prev2_wb;
    bool has_prev2;
    bool ri_odd;
    bool has_word;
};

static inline struct uax29_prop prop_for(uint32_t cp)
{
    if (cp > 0x10FFFFu)
        cp = 0xFFFDu;
    uint8_t b0 = uax29_stage0[cp >> 9];
    uint16_t b1 = uax29_stage1[((size_t)b0 << 5) | ((cp >> 4) & 0x1Fu)];
    return uax29_props[uax29_stage2[((size_t)b1 << 4) | (cp & 0xFu)]];
}

static inline size_t decode_next(const char *text, size_t len, size_t pos,
                                 uint32_t *cp)
{
    int n = wsap_vtcore_utf8_decode_one(text + pos, len - pos, cp);
    if (n <= 0) {
        *cp = 0xFFFDu;
        return 1;
    }
    return (size_t)n;
}

static size_t utf8_align_down(const char *text, size_t len, size_t pos)
{
    if (pos >= len)
        return len;
    if (((unsigned char)text[pos] & 0xC0u) != 0x80u)
        return pos;
    size_t start = pos;
    while (start > 0 && (pos - start) < 3 &&
           ((unsigned char)text[start] & 0xC0u) == 0x80u)
        start--;
    uint32_t cp = 0;
    int n = wsap_vtcore_utf8_decode_one(text + start, len - start, &cp);
    if (n > 0 && start + (size_t)n > pos)
        return start;
    return pos;
}

static size_t utf8_prev(const char *text, size_t len, size_t pos,
                        uint32_t *cp)
{
    if (((unsigned char)text[pos - 1] & 0x80u) == 0) {
        *cp = (unsigned char)text[pos - 1];
        return pos - 1;
    }
    size_t start = pos - 1;
    while (start > 0 && (pos - start) < 4 &&
           ((unsigned char)text[start] & 0xC0u) == 0x80u)
        start--;
    int n = wsap_vtcore_utf8_decode_one(text + start, len - start, cp);
    if (n > 0 && start + (size_t)n == pos)
        return start;
    *cp = 0xFFFDu;
    return pos - 1;
}

static inline bool is_plain_gcb(struct uax29_prop p)
{
    return p.gcb == GCB_OTHER && p.incb == INCB_NONE &&
           !(p.flags & UAX29_PROP_EXT_PICT);
}

static inline bool grapheme_guaranteed_break(struct uax29_prop left,
                                           struct uax29_prop right)
{
    if (left.gcb == GCB_CONTROL || left.gcb == GCB_LF)
        return true;
    if (left.gcb == GCB_CR && right.gcb != GCB_LF)
        return true;
    if (right.gcb == GCB_CONTROL || right.gcb == GCB_CR)
        return true;
    if (right.gcb == GCB_LF && left.gcb != GCB_CR)
        return true;
    return is_plain_gcb(left) && is_plain_gcb(right);
}

static size_t grapheme_anchor(const char *text, size_t len, size_t pos,
                              bool want_before)
{
    size_t cur = utf8_align_down(text, len, pos);
    if (want_before && cur == pos) {
        uint32_t dummy = 0;
        cur = utf8_prev(text, len, cur, &dummy);
    }
    uint32_t right_cp = 0;
    decode_next(text, len, cur, &right_cp);
    struct uax29_prop right = prop_for(right_cp);
    while (cur > 0) {
        uint32_t left_cp = 0;
        size_t prev = utf8_prev(text, len, cur, &left_cp);
        struct uax29_prop left = prop_for(left_cp);
        if (grapheme_guaranteed_break(left, right))
            break;
        cur = prev;
        right = left;
    }
    return cur;
}

static inline void grapheme_reset(struct grapheme_state *st,
                                  struct uax29_prop cur)
{
    st->prev_gcb = cur.gcb;
    st->incb_state = cur.incb == INCB_CONSONANT ? 1 : 0;
    st->ep_state = (cur.flags & UAX29_PROP_EXT_PICT) ? 1 : 0;
    st->ri_odd = cur.gcb == GCB_RI;
}

static inline bool grapheme_should_break(const struct grapheme_state *st,
                                         struct uax29_prop cur)
{
    if (st->prev_gcb == GCB_CR && cur.gcb == GCB_LF)
        return false; /* GB3 */
    if (st->prev_gcb == GCB_CONTROL || st->prev_gcb == GCB_CR ||
        st->prev_gcb == GCB_LF)
        return true; /* GB4 */
    if (cur.gcb == GCB_CONTROL || cur.gcb == GCB_CR || cur.gcb == GCB_LF)
        return true; /* GB5 */
    if (st->prev_gcb == GCB_L &&
        (cur.gcb == GCB_L || cur.gcb == GCB_V ||
         cur.gcb == GCB_LV || cur.gcb == GCB_LVT))
        return false; /* GB6 */
    if ((st->prev_gcb == GCB_LV || st->prev_gcb == GCB_V) &&
        (cur.gcb == GCB_V || cur.gcb == GCB_T))
        return false; /* GB7 */
    if ((st->prev_gcb == GCB_LVT || st->prev_gcb == GCB_T) &&
        cur.gcb == GCB_T)
        return false; /* GB8 */
    if (cur.gcb == GCB_EXTEND || cur.gcb == GCB_ZWJ)
        return false; /* GB9 */
    if (cur.gcb == GCB_SPACING_MARK)
        return false; /* GB9a */
    if (st->prev_gcb == GCB_PREPEND)
        return false; /* GB9b */
    if (st->incb_state == 2 && cur.incb == INCB_CONSONANT)
        return false; /* GB9c */
    if (st->ep_state == 2 && (cur.flags & UAX29_PROP_EXT_PICT))
        return false; /* GB11 */
    if (st->ri_odd && cur.gcb == GCB_RI)
        return false; /* GB12, GB13 */
    return true; /* GB999 */
}

static inline void grapheme_advance(struct grapheme_state *st,
                                    struct uax29_prop cur)
{
    st->prev_gcb = cur.gcb;
    if (cur.incb == INCB_CONSONANT)
        st->incb_state = 1;
    else if (cur.incb == INCB_LINKER)
        st->incb_state = st->incb_state > 0 ? 2 : 0;
    else if (cur.incb != INCB_EXTEND)
        st->incb_state = 0;

    if (cur.flags & UAX29_PROP_EXT_PICT)
        st->ep_state = 1;
    else if (st->ep_state == 1 && cur.gcb == GCB_EXTEND)
        st->ep_state = 1;
    else if (st->ep_state == 1 && cur.gcb == GCB_ZWJ)
        st->ep_state = 2;
    else
        st->ep_state = 0;

    st->ri_odd = cur.gcb == GCB_RI ? !st->ri_odd : false;
}

static void grapheme_scan(const char *text, size_t len, size_t start_pos,
                          ui_text_grapheme_fn fn, void *userdata)
{
    uint32_t cp = 0;
    size_t step = decode_next(text, len, start_pos, &cp);
    struct grapheme_state st = {0};
    grapheme_reset(&st, prop_for(cp));

    size_t cluster_start = start_pos;
    size_t pos = start_pos + step;
    while (pos < len) {
        step = decode_next(text, len, pos, &cp);
        struct uax29_prop cur = prop_for(cp);
        if (grapheme_should_break(&st, cur)) {
            if (!fn(cluster_start, pos, userdata))
                return;
            cluster_start = pos;
            grapheme_reset(&st, cur);
        } else {
            grapheme_advance(&st, cur);
        }
        pos += step;
    }
    fn(cluster_start, len, userdata);
}

void ui_text_each_grapheme(const char *text, size_t len,
                           ui_text_grapheme_fn fn, void *userdata)
{
    if (!text || !len || !fn)
        return;
    grapheme_scan(text, len, 0, fn, userdata);
}

struct grapheme_seek {
    size_t target;
    size_t result;
    bool backward;
};

static bool grapheme_seek_cb(size_t start, size_t end, void *userdata)
{
    struct grapheme_seek *seek = userdata;
    if (seek->backward) {
        if (start < seek->target)
            seek->result = start;
        return end < seek->target;
    }
    if (end > seek->target) {
        seek->result = end;
        return false;
    }
    return true;
}

size_t ui_text_break_grapheme_next(const char *text, size_t len, size_t pos)
{
    if (!text || !len)
        return 0;
    if (pos >= len)
        return len;
    size_t anchor = grapheme_anchor(text, len, pos, false);
    struct grapheme_seek seek = { .target = pos, .result = len };
    grapheme_scan(text, len, anchor, grapheme_seek_cb, &seek);
    return seek.result;
}

size_t ui_text_break_grapheme_prev(const char *text, size_t len, size_t pos)
{
    if (!text || !len || pos == 0)
        return 0;
    if (pos > len)
        pos = len;
    size_t anchor = grapheme_anchor(text, len, pos, true);
    struct grapheme_seek seek = { .target = pos, .backward = true };
    grapheme_scan(text, len, anchor, grapheme_seek_cb, &seek);
    return seek.result;
}

static inline bool wb_is_ignorable(uint8_t wb)
{
    return wb == WB_EXTEND || wb == WB_FORMAT || wb == WB_ZWJ;
}

static inline bool wb_is_nl_cr_lf(uint8_t wb)
{
    return wb == WB_NEWLINE || wb == WB_CR || wb == WB_LF;
}

static inline bool wb_is_ah(uint8_t wb)
{
    return wb == WB_ALETTER || wb == WB_HEBREW_LETTER;
}

static inline bool wb_is_mid_letter_q(uint8_t wb)
{
    return wb == WB_MIDLETTER || wb == WB_MIDNUMLET ||
           wb == WB_SINGLE_QUOTE;
}

static inline bool wb_is_mid_num_q(uint8_t wb)
{
    return wb == WB_MIDNUM || wb == WB_MIDNUMLET || wb == WB_SINGLE_QUOTE;
}

static inline bool wb_is_joiner(uint8_t wb)
{
    return wb == WB_ALETTER || wb == WB_HEBREW_LETTER ||
           wb == WB_NUMERIC || wb == WB_KATAKANA;
}

static uint8_t next_non_ignorable(const char *text, size_t len, size_t pos)
{
    while (pos < len) {
        uint32_t cp = 0;
        size_t step = decode_next(text, len, pos, &cp);
        uint8_t wb = prop_for(cp).wb;
        if (!wb_is_ignorable(wb))
            return wb;
        pos += step;
    }
    return WB_NONE;
}

static inline void word_reset(struct word_state *st, struct uax29_prop cur)
{
    st->raw_prev_wb = cur.wb;
    st->eff_wb = cur.wb;
    st->prev2_wb = 0;
    st->has_prev2 = false;
    st->ri_odd = cur.wb == WB_RI;
    st->has_word = (cur.flags & UAX29_PROP_IS_WORD) != 0;
}

static bool word_should_break(const struct word_state *st,
                              struct uax29_prop cur,
                              const char *text, size_t len, size_t next_pos,
                              bool *merged_wb4)
{
    uint8_t wb = cur.wb;
    *merged_wb4 = false;

    if (st->raw_prev_wb == WB_CR && wb == WB_LF)
        return false; /* WB3 */
    if (wb_is_nl_cr_lf(st->raw_prev_wb) || wb_is_nl_cr_lf(wb))
        return true; /* WB3a, WB3b */
    if (st->raw_prev_wb == WB_ZWJ && (cur.flags & UAX29_PROP_EXT_PICT))
        return false; /* WB3c */
    if (st->raw_prev_wb == WB_WSEGSPACE && wb == WB_WSEGSPACE)
        return false; /* WB3d */
    if (wb_is_ignorable(wb)) {
        *merged_wb4 = true; /* WB4 */
        return false;
    }

    if (wb_is_ah(st->eff_wb) && wb_is_ah(wb))
        return false; /* WB5 */
    if (st->has_prev2 && wb_is_ah(st->prev2_wb) &&
        wb_is_mid_letter_q(st->eff_wb) && wb_is_ah(wb))
        return false; /* WB7 */
    if (st->eff_wb == WB_HEBREW_LETTER && wb == WB_SINGLE_QUOTE)
        return false; /* WB7a */
    if (st->has_prev2 && st->prev2_wb == WB_HEBREW_LETTER &&
        st->eff_wb == WB_DOUBLE_QUOTE && wb == WB_HEBREW_LETTER)
        return false; /* WB7c */
    if (st->eff_wb == WB_NUMERIC && wb == WB_NUMERIC)
        return false; /* WB8 */
    if (wb_is_ah(st->eff_wb) && wb == WB_NUMERIC)
        return false; /* WB9 */
    if (st->eff_wb == WB_NUMERIC && wb_is_ah(wb))
        return false; /* WB10 */
    if (st->has_prev2 && st->prev2_wb == WB_NUMERIC &&
        wb_is_mid_num_q(st->eff_wb) && wb == WB_NUMERIC)
        return false; /* WB11 */
    if (st->eff_wb == WB_KATAKANA && wb == WB_KATAKANA)
        return false; /* WB13 */
    if ((wb_is_joiner(st->eff_wb) || st->eff_wb == WB_EXTENDNUMLET) &&
        wb == WB_EXTENDNUMLET)
        return false; /* WB13a */
    if (st->eff_wb == WB_EXTENDNUMLET && wb_is_joiner(wb))
        return false; /* WB13b */
    if (st->ri_odd && wb == WB_RI)
        return false; /* WB15, WB16 */

    bool need_ah = wb_is_ah(st->eff_wb) && wb_is_mid_letter_q(wb);
    bool need_hl_dq = st->eff_wb == WB_HEBREW_LETTER && wb == WB_DOUBLE_QUOTE;
    bool need_num = st->eff_wb == WB_NUMERIC && wb_is_mid_num_q(wb);
    if (need_ah || need_hl_dq || need_num) {
        uint8_t next_wb = next_non_ignorable(text, len, next_pos);
        if (need_ah && wb_is_ah(next_wb))
            return false; /* WB6 */
        if (need_hl_dq && next_wb == WB_HEBREW_LETTER)
            return false; /* WB7b */
        if (need_num && next_wb == WB_NUMERIC)
            return false; /* WB12 */
    }
    return true; /* WB999 */
}

void ui_text_each_word(const char *text, size_t len,
                       ui_text_word_fn fn, void *userdata)
{
    if (!text || !len || !fn)
        return;
    uint32_t cp = 0;
    size_t step = decode_next(text, len, 0, &cp);
    struct word_state st = {0};
    word_reset(&st, prop_for(cp));

    size_t seg_start = 0;
    size_t pos = step;
    while (pos < len) {
        step = decode_next(text, len, pos, &cp);
        struct uax29_prop cur = prop_for(cp);
        bool merged_wb4 = false;
        if (word_should_break(&st, cur, text, len, pos + step, &merged_wb4)) {
            if (!fn(seg_start, pos, st.has_word, userdata))
                return;
            seg_start = pos;
            word_reset(&st, cur);
        } else {
            if (cur.flags & UAX29_PROP_IS_WORD)
                st.has_word = true;
            if (!merged_wb4) {
                st.has_prev2 = true;
                st.prev2_wb = st.eff_wb;
                st.eff_wb = cur.wb;
                st.ri_odd = cur.wb == WB_RI ? !st.ri_odd : false;
            }
            st.raw_prev_wb = cur.wb;
        }
        pos += step;
    }
    fn(seg_start, len, st.has_word, userdata);
}
