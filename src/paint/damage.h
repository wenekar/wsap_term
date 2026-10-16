#ifndef WSAP_PAINT_DAMAGE_H
#define WSAP_PAINT_DAMAGE_H

#include "util/geom.h"
#include <stdbool.h>

#define UI_DAMAGE_MAX_RECTS 16

struct ui_damage {
    struct rect2i rects[UI_DAMAGE_MAX_RECTS];
    int n;
    bool all;
};

void ui_damage_add(struct ui_damage *d, struct rect2i r);
void ui_damage_merge(struct ui_damage *d, const struct ui_damage *src);
struct rect2i ui_damage_bounds(const struct ui_damage *d);

static inline bool ui_damage_is_empty(const struct ui_damage *d)
{
    return !d->all && d->n == 0;
}

#endif
