#ifndef RENDER_UTIL_H
#define RENDER_UTIL_H

#include <stdint.h>

#include "paint/blend.h"
#ifndef RENDER_UTIL_NO_TERMINAL
#include "render/cell.h"
#endif

void render_pool_destroy(void);

#endif
