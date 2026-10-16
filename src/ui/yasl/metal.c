#include "ui/yasl/metal.h"

#include <errno.h>
#include <stddef.h>

void *window_metal_attach(struct yasl_surface *s, void *device)
{
    (void)s;
    (void)device;
    errno = ENOTSUP;
    return NULL;
}

bool window_metal_draw(void *layer, struct yasl_surface *s,
                       window_metal_draw_fn draw, void *ud)
{
    (void)layer;
    (void)s;
    (void)draw;
    (void)ud;
    return false;
}
