#ifndef WSAP_UI_SHADOW_H
#define WSAP_UI_SHADOW_H

#include <stddef.h>
#include <stdint.h>

#include "util/geom.h"

/* A drop shadow is a Gaussian blur of the rounded rect that casts it.
 * CSS gives a blur radius; the standard deviation is half of it.
 *
 * A Gaussian blur is separable, so the blurred box is a nine-patch: a
 * solid middle, four straight edges that all share one profile, and
 * four corners that all share one tile. Both parts are tables. The
 * tables are built once per radius and blur pair and then cached, so a
 * shadow costs one table read and one blend per pixel to paint. */

#define UI_SHADOW_MAX_BLUR   64
#define UI_SHADOW_MAX_RADIUS 48
#define UI_SHADOW_MAX_EXTENT ((UI_SHADOW_MAX_BLUR * 3 + 1) / 2)

/* Pixels the shadow reaches past the edge of the box that casts it.
 * This is three standard deviations, where the blur has faded out. */
static inline int ui_shadow_extent(int blur)
{
    if (blur <= 0)
        return 0;
    if (blur > UI_SHADOW_MAX_BLUR)
        blur = UI_SHADOW_MAX_BLUR;
    return (blur * 3 + 1) / 2;
}

struct ui_shadow_mask {
    int radius;
    int blur;
    int extent;
    int corner;          /* corner tile side, radius + 2 * extent */
    const uint8_t *tile; /* corner * corner coverage, top left corner */
    const uint8_t *edge; /* 2 * extent + 1 coverage across one edge */
};

/* Returns the mask for this radius and blur. The first call builds it;
 * later calls read the cache. Returns NULL if blur is not positive.
 * A mask stays valid until a later call evicts it, so paint with it
 * before you ask for another one. */
const struct ui_shadow_mask *ui_shadow_mask(int radius, int blur);

void ui_shadow_cache_destroy(void);

/* Coverage of one straight edge at signed distance p. p is 0 at the
 * first pixel inside the box and negative outside it. */
static inline uint8_t ui_shadow_edge_at(const struct ui_shadow_mask *m, int p)
{
    int i = p + m->extent;
    if (i < 0)
        return 0;
    if (i > 2 * m->extent)
        return 255;
    return m->edge[i];
}

/* Coverage across a slab of len pixels, both of its edges combined.
 * This is exact for a box with square corners, in which case the box is
 * the product of two slabs. */
static inline uint8_t ui_shadow_slab(const struct ui_shadow_mask *m,
                                     int p, int len)
{
    unsigned near = ui_shadow_edge_at(m, p);
    unsigned far = ui_shadow_edge_at(m, len - 1 - p);
    return near + far > 255 ? (uint8_t)(near + far - 255) : 0;
}

/* Coverage of pixel p, 0 to 255. box is the rect that casts the shadow,
 * in the same space as p. Pixels in a corner read the tile. Every other
 * pixel is the product of the two slabs, which the tile boundary meets
 * because the corner stops mattering that far in. */
static inline uint8_t ui_shadow_coverage(const struct ui_shadow_mask *m,
                                         struct rect2i box,
                                         struct vector2i p)
{
    int px = p.x - box.x, py = p.y - box.y;
    int span = 2 * (m->radius + m->extent);
    if (m->tile && box.width >= span && box.height >= span) {
        int cx = px + m->extent, cy = py + m->extent;
        int mx = box.width - 1 - px + m->extent;
        int my = box.height - 1 - py + m->extent;
        if (mx < cx)
            cx = mx;
        if (my < cy)
            cy = my;
        if (cx < m->corner && cy < m->corner)
            return cx < 0 || cy < 0
                ? 0 : m->tile[(size_t)cy * m->corner + cx];
    }
    unsigned sx = ui_shadow_slab(m, px, box.width);
    unsigned sy = ui_shadow_slab(m, py, box.height);
    return (uint8_t)((sx * sy + 127) / 255);
}

#endif
