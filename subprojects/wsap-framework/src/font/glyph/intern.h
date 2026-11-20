#ifndef WSAP_FONT_GLYPH_INTERN_H
#define WSAP_FONT_GLYPH_INTERN_H

#include "util/unicode/grapheme.h"

uint32_t glyph_intern(const uint32_t *cps, int len);
int glyph_intern_expand(uint32_t c, uint32_t *out, size_t out_cap);
/* Returns the epoch of the interned cluster table. The epoch starts at 0 and
 * changes each time a handle is reused for another cluster. Read it before
 * expanding a handle; a later change means the handle can name a different
 * cluster. */
uint32_t glyph_intern_epoch(void);

#endif
