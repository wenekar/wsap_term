#include "common/backend.h"
#include "common/private_native_x11.h"

Display *yasl_private_x11_display_get_display(struct yasl_private_display *d)
{
    return (d && yasl_private_display_backend(d) == YASL_PRIVATE_BACKEND_X11) ?
        (Display *)yasl_private_display_get_native_handle(d) : NULL;
}

uintptr_t yasl_private_x11_surface_get_window(struct yasl_surface *s)
{
    return (s && s->display && yasl_private_display_backend(s->display) == YASL_PRIVATE_BACKEND_X11) ?
        (uintptr_t)yasl_private_surface_get_native_handle(s) : 0;
}
