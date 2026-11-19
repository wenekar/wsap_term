/**
 * @file
 * Typed borrowed X11 handles; available when built with X11 support.
 */
#ifndef YASL_PRIVATE_NATIVE_X11_H
#define YASL_PRIVATE_NATIVE_X11_H

#include "common/types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _XDisplay Display; /**< Opaque Xlib connection, compatible with Xlib.h. */

/* Return borrowed X11 handles (expire on display/surface destruction).
 * Return NULL / 0 if display/surface is not an X11 object or NULL. */
/**
 * Get the borrowed X11 Display for an object of the matching backend.
 * @param d Live display, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on display destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
Display *yasl_private_x11_display_get_display(struct yasl_private_display *d);
/**
 * Get the borrowed X11 Window ID for an object of the matching backend.
 * @param s Live surface, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on surface destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
uintptr_t yasl_private_x11_surface_get_window(struct yasl_surface *s);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_NATIVE_X11_H */
