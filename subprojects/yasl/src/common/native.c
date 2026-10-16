#include "backend.h"

#include <errno.h>

void *yasl_private_display_get_native_handle(struct yasl_private_display *d)
{
    if (!d)
        return NULL;
    if (d->native_handle)
        return d->native_handle;
    return d->ops && d->ops->display_get_native_handle ? d->ops->display_get_native_handle(d)
                                                       : NULL;
}

void *yasl_private_surface_get_native_handle(struct yasl_surface *s)
{
    if (!s)
        return NULL;
    if (s->native_handle)
        return s->native_handle;
    return s->display && s->display->ops && s->display->ops->surface_get_native_handle
               ? s->display->ops->surface_get_native_handle(s)
               : NULL;
}

struct yasl_surface *yasl_private_surface_create_native(struct yasl_private_display *d,
                                                        const struct yasl_private_window_spec *spec,
                                                        void *graphics_device)
{
    if (!graphics_device) {
        errno = EINVAL;
        return NULL;
    }
    return yasl_private_create_toplevel(d, spec, graphics_device);
}

bool yasl_private_surface_get_native_frame(struct yasl_surface *s,
                                           const struct yasl_private_framebuffer *target,
                                           struct yasl_private_native_frame *out)
{
    if (!s || !target || !out || !target->generation || target->generation != s->frame_generation ||
        !s->display->ops->surface_get_native_frame)
        return false;
    return s->display->ops->surface_get_native_frame(s, target, out);
}

#include "public_internal.h"
#if YASL_HAS_COCOA
#include "common/private_native_cocoa.h"
#endif
int yasl_surface_get_native(const yasl_surface *cs, struct yasl_native *out)
{
    yasl_surface *s = (yasl_surface *)cs;
    if (!out)
        return yasl_public_error(EINVAL);
    *out = (struct yasl_native){0};
    if (!yasl_public_connected(s))
        return -errno;
    void *window = yasl_private_surface_get_native_handle(s);
    switch (s->display->ops->type) {
    case YASL_PRIVATE_BACKEND_WAYLAND:
        out->wl_display = yasl_private_display_get_native_handle(s->display);
        out->wl_surface = window;
        break;
    case YASL_PRIVATE_BACKEND_X11:
        out->x11_display = yasl_private_display_get_native_handle(s->display);
        out->x11_window = (unsigned long)(uintptr_t)window;
        break;
#if YASL_HAS_COCOA
    case YASL_PRIVATE_BACKEND_COCOA:
        out->ns_window = yasl_private_cocoa_surface_get_window(s);
        out->ns_view = yasl_private_cocoa_surface_get_view(s);
        break;
#endif
    default:
        return yasl_public_error(ENOTSUP);
    }
    if (!window) {
        *out = (struct yasl_native){0};
        return yasl_public_error(ENOTCONN);
    }
    return 0;
}
void *yasl_metal_get_layer(yasl_surface *s)
{
    if (!yasl_public_connected(s))
        return NULL;
    if (s->vulkan || (s->presenter != YASL_PRIVATE_PRESENTER_NONE &&
                      s->presenter != YASL_PRIVATE_PRESENTER_METAL))
        return yasl_public_fail(EBUSY), NULL;
    void *layer = s->display->ops->surface_get_metal_layer
                      ? s->display->ops->surface_get_metal_layer(s)
                      : NULL;
    if (!layer)
        return yasl_public_fail(ENOTSUP), NULL;
    s->presenter = YASL_PRIVATE_PRESENTER_METAL;
    return layer;
}
