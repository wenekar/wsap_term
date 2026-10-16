#ifndef WSAP_FONT_GLYPH_INTERN_H
#define WSAP_FONT_GLYPH_INTERN_H

#include "wsap_vtcore/unicode/grapheme.h"

/* The shared glyph table maps font handles to code points. */
uint32_t glyph_intern(const uint32_t *cps, int len);
int glyph_intern_expand(uint32_t c, uint32_t *out, size_t out_cap);

#endif
