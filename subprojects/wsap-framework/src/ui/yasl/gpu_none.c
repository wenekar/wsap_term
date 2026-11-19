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

bool window_gpu_cursor_supported(struct yasl_surface *s)
{
    (void)s;
    return false;
}

bool window_gpu_set_cursor(struct yasl_surface *s, bool active, bool visible,
                           struct rect2i rect, uint32_t color,
                           const uint32_t *pixels, uint64_t key)
{
    (void)s; (void)active; (void)visible; (void)rect;
    (void)color; (void)pixels; (void)key;
    return false;
}

bool window_gpu_set_cursor_visible(struct yasl_surface *s, bool visible)
{
    (void)s; (void)visible;
    return false;
}

void window_gpu_clear_cursor(struct yasl_surface *s)
{
    (void)s;
}
