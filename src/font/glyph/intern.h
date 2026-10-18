#ifndef WSAP_FONT_GLYPH_INTERN_H
#define WSAP_FONT_GLYPH_INTERN_H

#include "wsap_vtcore/unicode/grapheme.h"

uint32_t glyph_intern(const uint32_t *cps, int len);
int glyph_intern_expand(uint32_t c, uint32_t *out, size_t out_cap);
/* Changes each time interned handles are reused. Starts at 0.
 * Read it before you expand or draw a handle. A later change then shows
 * that the handle can name a different cluster. */
uint32_t glyph_intern_epoch(void);

#endif
