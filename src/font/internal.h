#ifndef FONT_INTERNAL_H
#define FONT_INTERNAL_H

#include "font/font.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern __thread struct font_metrics font_thread_metrics;
#define GLYPH_CELL_H font_thread_metrics.cell_h
#define GLYPH_CELL_W font_thread_metrics.cell_w

static inline int glyph_rows(const struct glyph *g)
{
    return g->height ? g->height : GLYPH_CELL_H;
}

struct box { int x, y, w, h; };

bool glyph_reset(struct glyph *g, unsigned width);
void font_thread_metrics_set(const struct font_metrics *m);
void face_activate(const struct font_metrics *m);
bool font_measure_size(struct font_size size, struct font_metrics *out);

#endif
