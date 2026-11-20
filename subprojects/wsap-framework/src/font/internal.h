#ifndef FONT_INTERNAL_H
#define FONT_INTERNAL_H

#include "font/font.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Cell metrics of the font that the calling thread rasterizes with. */
extern __thread struct font_metrics font_thread_metrics;

/* Returns the raster rows of glyph `g`. */
static inline int glyph_rows(const struct glyph *g)
{
    return g->height;
}

struct box { int x, y, w, h; };

bool glyph_reset(struct glyph *g, unsigned width, unsigned rows);
void font_thread_metrics_set(const struct font_metrics *m);
void face_activate(const struct font_metrics *m);
bool font_measure_desc(const struct font_desc *desc, struct font_metrics *out);
void font_apply_desc_family(const struct font_desc *desc);

#endif
