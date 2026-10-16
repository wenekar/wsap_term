/**
 * @file
 * Typed borrowed Cocoa handles; available on macOS. Use on the main thread.
 */
#ifndef YASL_PRIVATE_NATIVE_COCOA_H
#define YASL_PRIVATE_NATIVE_COCOA_H

#include "common/types.h"

#ifdef __OBJC__
@class NSApplication;
@class NSWindow;
@class NSView;
#else
typedef struct objc_object NSApplication; /**< Opaque Cocoa application in plain C/C++. */
typedef struct objc_object NSWindow; /**< Opaque Cocoa window in plain C/C++. */
typedef struct objc_object NSView; /**< Opaque Cocoa view in plain C/C++. */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Return borrowed Cocoa handles (expire on display/surface destruction).
 * Return NULL / nil if display/surface is not a Cocoa object or NULL. */
/**
 * Get the borrowed NSApplication for an object of the matching backend.
 * @param d Live display, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on display destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
NSApplication *yasl_private_cocoa_display_get_application(struct yasl_private_display *d);
/**
 * Get the borrowed NSWindow for an object of the matching backend.
 * @param s Live surface, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on surface destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
NSWindow *yasl_private_cocoa_surface_get_window(struct yasl_surface *s);
/**
 * Get the borrowed NSView for an object of the matching backend.
 * @param s Live surface, or NULL.
 * @return The handle, or zero/NULL for NULL or a different backend.
 * Expires on surface destruction. Available only when this backend is built.
 * Never pass a pointer to a destroyed object.
 */
NSView *yasl_private_cocoa_surface_get_view(struct yasl_surface *s);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_NATIVE_COCOA_H */
