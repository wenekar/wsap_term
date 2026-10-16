#include "common/backend.h"
#include "common/private_native_wayland.h"

struct wl_display *yasl_private_wayland_display_get_wl_display(struct yasl_private_display *d)
{
    return (d && yasl_private_display_backend(d) == YASL_PRIVATE_BACKEND_WAYLAND) ?
        (struct wl_display *)yasl_private_display_get_native_handle(d) : NULL;
}

struct wl_surface *yasl_private_wayland_surface_get_wl_surface(struct yasl_surface *s)
{
    return (s && s->display && yasl_private_display_backend(s->display) == YASL_PRIVATE_BACKEND_WAYLAND) ?
        (struct wl_surface *)yasl_private_surface_get_native_handle(s) : NULL;
}
