#ifndef CELLDRAW_H
#define CELLDRAW_H

#include <stdbool.h>
#include <stdint.h>
#include "font/font.h"

bool draw_cell_aligned_symbol(uint32_t cp, struct glyph *g);
bool is_cell_aligned_symbol(uint32_t cp);

/* The fallbacks for a glyph that no face has. They draw into a reset g. */
void glyph_render_5x7(uint32_t cp, struct glyph *g);
void glyph_render_unknown_box(struct glyph *g);

#endif
