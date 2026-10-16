#include "ui/anchor.h"

static int fit_axis(int v, int size, int lo, int end)
{
    if ((int64_t)v + size > end)
        v = geom_sat_i((int64_t)end - size);
    return v < lo ? lo : v;
}

struct rect2i ui_anchor_place_below(struct vector2i anchor,
                                    struct vector2i size,
                                    int gap, int flip_to,
                                    struct rect2i extent)
{
    struct vector2i pos = {
        .x = anchor.x,
        .y = geom_sat_i((int64_t)anchor.y + gap),
    };
    if (rect2i_is_empty(extent))
        return (struct rect2i){ .pos = pos, .size = size };
    struct vector2i end = rect2i_end(extent);
    pos.x = fit_axis(pos.x, size.x, extent.pos.x, end.x);
    if ((int64_t)pos.y + size.y > end.y)
        pos.y = geom_sat_i((int64_t)flip_to - size.y);
    if (pos.y < extent.pos.y)
        pos.y = extent.pos.y;
    return (struct rect2i){ .pos = pos, .size = size };
}

struct rect2i ui_anchor_place_right(struct vector2i anchor,
                                    struct vector2i size,
                                    int gap, int flip_to,
                                    struct vector2i extent)
{
    struct vector2i pos = {
        .x = geom_sat_i((int64_t)anchor.x + gap),
        .y = anchor.y,
    };
    if (flip_to >= 0 && (int64_t)pos.x + size.x > extent.x)
        pos.x = geom_sat_i((int64_t)flip_to - size.x);
    pos.x = fit_axis(pos.x, size.x, 0, extent.x);
    pos.y = fit_axis(pos.y, size.y, 0, extent.y);
    return (struct rect2i){ .pos = pos, .size = size };
}
