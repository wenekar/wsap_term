#ifndef WSAP_UI_CONSTRAINTS_H
#define WSAP_UI_CONSTRAINTS_H

#include <limits.h>

#include "util/geom.h"

struct ui_constraints {
    struct vector2i min;
    struct vector2i max;
};

static inline struct ui_constraints ui_constraints_unbounded(void)
{
    return (struct ui_constraints){
        .max = { .x = INT_MAX, .y = INT_MAX },
    };
}

static inline struct vector2i ui_constraints_clamp(
    struct ui_constraints constraints, struct vector2i size)
{
    return vector2i_clamp(size, constraints.min, constraints.max);
}

#endif
