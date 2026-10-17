#ifndef WSAP_UI_SHADOW_H
#define WSAP_UI_SHADOW_H

#include <stddef.h>
#include <stdint.h>

#include "util/geom.h"

#define UI_SHADOW_MAX_BLUR   64
#define UI_SHADOW_MAX_RADIUS 48
#define UI_SHADOW_MAX_EXTENT ((UI_SHADOW_MAX_BLUR * 3 + 1) / 2)

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
    int corner;
    const uint8_t *tile;
    const uint8_t *edge;
};

const struct ui_shadow_mask *ui_shadow_mask(int radius, int blur);

/*
 * The same mask, built from paint/shape_math.h. The new renderer uses it,
 * so the CPU and the GPU agree. The cache is per thread. A mask stays
 * valid until the same thread builds four other masks.
 */
const struct ui_shadow_mask *ui_shadow_mask_shape(int radius, int blur);

void ui_shadow_cache_destroy(void);

static inline uint8_t ui_shadow_edge_at(const struct ui_shadow_mask *m, int p)
{
    int i = p + m->extent;
    if (i < 0)
        return 0;
    if (i > 2 * m->extent)
        return 255;
    return m->edge[i];
}

static inline uint8_t ui_shadow_slab(const struct ui_shadow_mask *m,
                                     int p, int len)
{
    unsigned near = ui_shadow_edge_at(m, p);
    unsigned far = ui_shadow_edge_at(m, len - 1 - p);
    return near + far > 255 ? (uint8_t)(near + far - 255) : 0;
}

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
