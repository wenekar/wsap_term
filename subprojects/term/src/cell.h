#ifndef WSAP_VTCORE_CELL_H
#define WSAP_VTCORE_CELL_H

#include "wsap_vtcore.h"

enum cell_flags {
    CELL_BOLD      = WSAP_VTCORE_ATTR_BOLD,
    CELL_ITALIC    = WSAP_VTCORE_ATTR_ITALIC,
    CELL_UNDERLINE = WSAP_VTCORE_ATTR_UNDERLINE,
    CELL_STRIKE    = WSAP_VTCORE_ATTR_STRIKE,
    CELL_OVERLINE  = WSAP_VTCORE_ATTR_OVERLINE,
    CELL_EMOJI     = WSAP_VTCORE_ATTR_EMOJI,
    CELL_TEXT      = WSAP_VTCORE_ATTR_TEXT,
    CELL_WIDE      = 1 << 7,
    CELL_TRAIL     = 1 << 8,
    CELL_WRAP_PAD  = 1 << 9,
    CELL_FAINT     = WSAP_VTCORE_ATTR_FAINT,
    CELL_DOUBLE_UNDERLINE = WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE,
    CELL_CURLY_UNDERLINE  = WSAP_VTCORE_ATTR_CURLY_UNDERLINE,
    CELL_DOTTED_UNDERLINE = WSAP_VTCORE_ATTR_DOTTED_UNDERLINE,
    CELL_DASHED_UNDERLINE = WSAP_VTCORE_ATTR_DASHED_UNDERLINE,
    CELL_LINK      = WSAP_VTCORE_ATTR_LINK,
};

#define CELL_ANY_UNDERLINE WSAP_VTCORE_ATTR_UNDERLINE_MASK

/* The pad field of a cell holds its extended style. Bits 0 to 12 hold a
 * style id. The style table (style.h) maps the id to a link and an underline
 * color. Id zero has no link and no underline color. Bits 13 to 15 hold
 * flags. A plain cell has pad zero. */
#define CELL_EXT_ID        0x1FFFu
#define CELL_EXT_BLINK     0x2000u
#define CELL_EXT_CONCEAL   0x4000u
#define CELL_EXT_PROTECTED 0x8000u
#define CELL_EXT_FLAGS     (CELL_EXT_BLINK | CELL_EXT_CONCEAL | CELL_EXT_PROTECTED)

/* A shift moves the blink and conceal flags to their public attribute bits. */
#define CELL_EXT_ATTR_SHIFT 6
_Static_assert((CELL_EXT_BLINK >> CELL_EXT_ATTR_SHIFT) == WSAP_VTCORE_ATTR_BLINK &&
               (CELL_EXT_CONCEAL >> CELL_EXT_ATTR_SHIFT) == WSAP_VTCORE_ATTR_CONCEAL,
               "blink and conceal must shift to their public attribute bits");

struct cell {
    uint32_t c;
    uint32_t fg;
    uint32_t bg;
    uint16_t flags;
    uint16_t pad;
};

_Static_assert(sizeof(struct cell) == 16,
               "struct cell must stay 16 bytes (4 cells per AVX-512 register)");

#endif
