#include "paint/damage.h"

#include <stdint.h>

static int64_t area(struct rect2i r)
{
    return (int64_t)r.width * r.height;
}

void ui_damage_add(struct ui_damage *d, struct rect2i r)
{
    if (!d || d->all || rect2i_is_empty(r))
        return;
    bool merged;
    do {
        merged = false;
        for (int i = 0; i < d->n;) {
            if (rect2i_touches(d->rects[i], r)) {
                r = rect2i_union(d->rects[i], r);
                d->rects[i] = d->rects[--d->n];
                merged = true;
                continue;
            }
            i++;
        }
    } while (merged && d->n > 0);
    if (d->n < UI_DAMAGE_MAX_RECTS) {
        d->rects[d->n++] = r;
        return;
    }
    int best = 0;
    int64_t best_growth = INT64_MAX;
    for (int i = 0; i < d->n; i++) {
        struct rect2i u = rect2i_union(d->rects[i], r);
        int64_t growth = area(u) - area(d->rects[i]);
        if (growth < best_growth) {
            best_growth = growth;
            best = i;
        }
    }
    r = rect2i_union(d->rects[best], r);
    d->rects[best] = d->rects[--d->n];
    ui_damage_add(d, r);
}

void ui_damage_merge(struct ui_damage *d, const struct ui_damage *src)
{
    if (!d || !src)
        return;
    if (src->all) {
        d->all = true;
        d->n = 0;
        return;
    }
    for (int i = 0; i < src->n; i++)
        ui_damage_add(d, src->rects[i]);
}

struct rect2i ui_damage_bounds(const struct ui_damage *d)
{
    if (!d || d->n <= 0)
        return (struct rect2i){0};
    struct rect2i u = d->rects[0];
    for (int i = 1; i < d->n; i++)
        u = rect2i_union(u, d->rects[i]);
    return u;
}
