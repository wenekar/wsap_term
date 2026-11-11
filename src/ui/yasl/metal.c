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

void window_metal_detach(void *layer, struct yasl_surface *s)
{
    (void)layer;
    (void)s;
}

bool window_metal_draw(void *layer, struct yasl_surface *s,
                       struct window_metal_watch watch,
                       window_metal_draw_fn draw, void *ud)
{
    (void)layer;
    (void)s;
    (void)watch;
    (void)draw;
    (void)ud;
    return false;
}
