#ifndef UTIL_PNG_H
#define UTIL_PNG_H

#include <stdbool.h>
#include <stdint.h>

#include "util/geom.h"

/* Writes 0xAARRGGBB pixels as an 8-bit RGB PNG, ignoring alpha. The
 * image data is stored uncompressed, so no zlib is needed. stride is in
 * pixels. */
bool png_write_xrgb(const char *path, const uint32_t *px,
                    struct vector2i size, int stride);

#endif
