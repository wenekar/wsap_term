#ifndef WSAP_VTCORE_CELL_H
#define WSAP_VTCORE_CELL_H

/*
 * The cell layout that the terminal stores. Only src/term uses it. A
 * snapshot converts these cells to the public struct wsap_vtcore_cell.
 */

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

/* c is a code point or a grapheme handle. pad holds the OSC 8 link id. */
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
