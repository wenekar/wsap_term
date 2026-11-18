#define _GNU_SOURCE
#include "font/font.h"
#include "font/internal.h"

#include <stdlib.h>
#include <string.h>

bool glyph_reset(struct glyph *g, unsigned width, unsigned rows)
{
    void *r = g->raster, *c = g->color;
    uint8_t **s = g->subpx;
    uint32_t rc = g->raster_capacity, cc = g->color_capacity, sc = g->subpx_capacity;
    *g = (struct glyph){
        .raster = r, .raster_capacity = rc,
        .color = c, .color_capacity = cc,
        .subpx = s, .subpx_capacity = sc
    };
    return glyph_prepare(g, width, rows);
}

bool glyph_color_plane(struct glyph *g)
{
    size_t needed = (size_t)glyph_rows(g) * g->width * sizeof(*g->color);
    if (needed > g->color_capacity) {
        if (needed > UINT32_MAX)
            return false;
        void *p = realloc(g->color, needed);
        if (!p)
            return false;
        g->color = p;
        g->color_capacity = (uint32_t)needed;
    }
    memset(g->color, 0, needed);
    return true;
}

bool glyph_subpx_plane(struct glyph *g)
{
    if (!g || !g->width)
        return false;
    size_t rows = (size_t)glyph_rows(g);
    size_t ptr_bytes = rows * sizeof(*g->subpx);
    size_t data_bytes = rows * (size_t)g->width * 3;
    size_t needed = ptr_bytes + data_bytes;
    if (needed > g->subpx_capacity) {
        if (needed > UINT32_MAX)
            return false;
        void *p = realloc(g->subpx, needed);
        if (!p)
            return false;
        g->subpx = p;
        g->subpx_capacity = (uint32_t)needed;
    }
    memset(g->subpx, 0, needed);
    uint8_t *pixels = (uint8_t *)(g->subpx + rows);
    for (size_t y = 0; y < rows; y++)
        g->subpx[y] = pixels + y * (size_t)g->width * 3;
    g->has_subpx_ink = true;
    return true;
}

bool glyph_prepare(struct glyph *g, unsigned width, unsigned rows)
{
    if (!g || !width || width > UINT16_MAX || !rows || rows > UINT16_MAX)
        return false;
    g->width = (uint16_t)width;
    g->height = (uint16_t)rows;
    g->mask_words = (uint16_t)((width + 63) / 64);
    size_t nrows = g->height;
    size_t mask_bytes = nrows * g->mask_words * sizeof(*g->row_masks);
    size_t needed = mask_bytes + nrows * sizeof(*g->alpha) + nrows * g->width;
    if (needed > g->raster_capacity) {
        if (needed > UINT32_MAX) {
            g->width = 0;
            return false;
        }
        void *p = realloc(g->raster, needed);
        if (!p) {
            g->width = 0;
            return false;
        }
        g->raster = p;
        g->raster_capacity = (uint32_t)needed;
    }
    memset(g->raster, 0, needed);
    g->row_masks = g->raster;
    g->alpha = (uint8_t **)((char *)g->raster + mask_bytes);
    uint8_t *pixels = (uint8_t *)(g->alpha + nrows);
    for (size_t y = 0; y < nrows; y++)
        g->alpha[y] = pixels + y * g->width;
    g->has_subpx_ink = false;
    if (g->subpx && !glyph_subpx_plane(g)) {
        g->width = 0;
        return false;
    }
    g->has_subpx_ink = false;
    if (g->color && !glyph_color_plane(g)) {
        g->width = 0;
        return false;
    }
    return true;
}

void glyph_release(struct glyph *g)
{
    if (!g) return;
    free(g->raster);
    free(g->color);
    free(g->subpx);
    *g = (struct glyph){0};
}
