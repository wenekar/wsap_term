#ifndef WSAP_VTCORE_RECT_H
#define WSAP_VTCORE_RECT_H

#include "internal.h"

/* A screen rectangle. The values are screen rows and columns, counted from
 * 0. The rectangle includes its last row and its last column.
 */
struct area {
    int top, left, bottom, right;
};

/* Read the rectangle in the CSI parameters first to first + 3 (Pt, Pl, Pb,
 * Pr). A zero or missing parameter selects the edge of the screen. In origin
 * mode (DECOM), row 1 and column 1 are at the margins, and the margins clip
 * the rectangle. Return false when the rectangle is empty.
 */
bool wsap_vtcore_area_parse(const struct wsap_vtcore *t, const struct csi *csi, int first,
                            struct area *a);

/* Give the columns of row y that an attribute change covers. With DECSACE 2,
 * this is the rectangle. Otherwise it is the character stream from the first
 * cell of the area to its last cell.
 */
void wsap_vtcore_area_row_span(const struct wsap_vtcore *t, const struct area *a, int y,
                               int *x0, int *x1);

/* DECFRA: fill a rectangle with one character and the current attributes. */
void wsap_vtcore_sc_decfra(struct wsap_vtcore *t, const struct csi *csi);
/* DECERA: erase a rectangle with the current background. */
void wsap_vtcore_sc_decera(struct wsap_vtcore *t, const struct csi *csi);
/* DECCRA: copy a rectangle. The source and the destination can overlap. */
void wsap_vtcore_sc_deccra(struct wsap_vtcore *t, const struct csi *csi);

/* Insert or delete count columns at column x, in each row of the scroll
 * region. The cells from x to the right margin move. Blank columns get the
 * current background. The cursor does not move.
 */
void wsap_vtcore_screen_shift_cols(struct wsap_vtcore *t, int x, int count, bool insert);

#endif
