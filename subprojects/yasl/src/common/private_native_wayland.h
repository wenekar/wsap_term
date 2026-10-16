/**
 * @file
 * Typed borrowed Wayland handles; available when built with Wayland support.
 */
#ifndef YASL_PRIVATE_NATIVE_WAYLAND_H
#define YASL_PRIVATE_NATIVE_WAYLAND_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;

/* Return borrowed Wayland handles (expire on display/surface destruction).
 * Return NULL if display/surface is not a Wayland object or NULL. */
/**
 * Get the borrowed Wayland connection for an object of the matching backend.
 * @param d Live display, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on display destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
struct wl_display *yasl_private_wayland_display_get_wl_display(struct yasl_private_display *d);
/**
 * Get the borrowed Wayland surface for an object of the matching backend.
 * @param s Live surface, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on surface destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
struct wl_surface *yasl_private_wayland_surface_get_wl_surface(struct yasl_surface *s);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_NATIVE_WAYLAND_H */
