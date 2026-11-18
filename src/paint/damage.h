#ifndef WSAP_PAINT_DAMAGE_H
#define WSAP_PAINT_DAMAGE_H

#include "util/geom.h"
#include <stdbool.h>

/* The most damage rectangles of one frame. The merge in ui_damage_add()
 * holds the list below this bound, so a large frame keeps its detail. */
#define UI_DAMAGE_MAX_RECTS 32

struct ui_damage {
    struct rect2i rects[UI_DAMAGE_MAX_RECTS];
    int n;
    bool all;
};

void ui_damage_add(struct ui_damage *d, struct rect2i r);
struct rect2i ui_damage_bounds(const struct ui_damage *d);

static inline bool ui_damage_is_empty(const struct ui_damage *d)
{
    return !d->all && d->n == 0;
}

#endif
