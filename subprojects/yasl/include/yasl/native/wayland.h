/** @file Borrowed wayland handles for an initialized, connected surface. */
#ifndef YASL_NATIVE_WAYLAND_H
#define YASL_NATIVE_WAYLAND_H
#include "yasl/yasl.h"
struct wl_display;
struct wl_surface;
#ifdef __cplusplus
extern "C" {
#endif
struct yasl_wayland_handles {
    struct wl_display *display;
    struct wl_surface *surface;
};
/** Owner thread only. Returns -ENOTSUP for another backend and -ENOTCONN
 * while disconnected. Clears out on failure. Handles expire on display loss
 * or surface destruction; query again after DISPLAY_RESTORED. Never destroy
 * borrowed objects or dispatch/close the connection independently. */
YASL_API int yasl_wayland_get_native(const yasl_surface *surface, struct yasl_wayland_handles *out);
#ifdef __cplusplus
}
#endif
#endif
