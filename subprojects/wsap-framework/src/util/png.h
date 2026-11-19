#ifndef UTIL_PNG_H
#define UTIL_PNG_H

#include <stdbool.h>
#include <stdint.h>

#include "util/geom.h"

bool png_write_xrgb(const char *path, const uint32_t *px,
                    struct vector2i size, int stride);

#endif
