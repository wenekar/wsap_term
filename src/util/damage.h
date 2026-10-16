#ifndef UTIL_DAMAGE_H
#define UTIL_DAMAGE_H

#include <stdbool.h>

#include "util/geom.h"

#define DAMAGE_RECTS_MAX 8

struct damage_list {
    struct rect2i bounds;
    struct rect2i rects[DAMAGE_RECTS_MAX];
    int n;
    bool any;
    bool all;
};

static inline void damage_add(struct damage_list *d, struct rect2i r)
{
    d->any = true;
    if (rect2i_is_empty(r))
        return;
    d->bounds = rect2i_union(d->bounds, r);
    if (d->n < 0)
        return;
    if (d->n && rect2i_touches(d->rects[d->n - 1], r))
        d->rects[d->n - 1] = rect2i_union(d->rects[d->n - 1], r);
    else if (d->n < DAMAGE_RECTS_MAX)
        d->rects[d->n++] = r;
    else
        d->n = -1;
}

static inline void damage_add_all(struct damage_list *d)
{
    d->any = true;
    d->all = true;
}

#endif
