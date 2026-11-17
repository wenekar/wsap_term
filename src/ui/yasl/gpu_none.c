#include "ui/yasl/gpu_surface.h"

#include <errno.h>
#include <stddef.h>

/*
 * The GPU surface of a build with no GPU. It attaches no layer, so the surface
 * draws on the CPU.
 */

void *window_gpu_attach(struct yasl_surface *s, void *device)
{
    (void)s;
    (void)device;
    errno = ENOTSUP;
    return NULL;
}

void window_gpu_detach(void *layer, struct yasl_surface *s)
{
    (void)layer;
    (void)s;
}

bool window_gpu_draw(void *layer, struct yasl_surface *s,
                     struct window_gpu_watch watch,
                     window_gpu_draw_fn draw, void *ud)
{
    (void)layer;
    (void)s;
    (void)watch;
    (void)draw;
    (void)ud;
    return false;
}
