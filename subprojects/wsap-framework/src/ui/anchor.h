#ifndef WSAP_UI_ANCHOR_H
#define WSAP_UI_ANCHOR_H

#include "util/geom.h"

struct rect2i ui_anchor_place_below(struct vector2i anchor,
                                    struct vector2i size,
                                    int gap, int flip_to,
                                    struct rect2i extent);

struct rect2i ui_anchor_place_right(struct vector2i anchor,
                                    struct vector2i size,
                                    int gap, int flip_to,
                                    struct vector2i extent);

#endif
