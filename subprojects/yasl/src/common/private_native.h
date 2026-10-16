/**
 * @file
 * Optional native handles and application-managed rendering.
 *
 * These extensions follow the core thread and lifetime rules. Handles are borrowed;
 * never destroy them or dispatch their native connection independently. Typed
 * backend accessors live in the yasl/native headers and are installed only for built
 * backends. Application-managed drawing currently supports Cocoa/Metal only.
 */
#ifndef YASL_PRIVATE_NATIVE_H
#define YASL_PRIVATE_NATIVE_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional platform interoperability. All handles are borrowed and expire on
 * object destruction. Check yasl_private_display_backend() before casting:
 *
 *              display                  surface
 * Wayland      struct wl_display *       struct wl_surface *
 * X11          Display *                 Window (cast through uintptr_t)
 * Cocoa        NSApplication *           NSWindow * (view: window.contentView)
 */
/**
 * Get a borrowed, backend-specific display handle.
 * @param d Display, or NULL.
 * @return Native handle, or NULL. Inspect yasl_private_display_backend() before casting.
 * Prefer a typed accessor from yasl/native/wayland.h, x11.h, or cocoa.h.
 * The handle expires on display close; do not close or dispatch it independently.
 */
void *yasl_private_display_get_native_handle(struct yasl_private_display *d);
/**
 * Get a borrowed, backend-specific surface handle.
 * @param s Surface, or NULL.
 * @return Native handle, or NULL. X11 encodes a Window through uintptr_t.
 * Prefer typed accessors. The handle expires on surface destruction. Do not
 * change native ownership, event masks, buffer attachment, or surface roles.
 */
void *yasl_private_surface_get_native_handle(struct yasl_surface *s);

/* Create a surface for application-managed native rendering. graphics_device
 * must be non-NULL; currently an id<MTLDevice> on Cocoa, retained by the surface.
 * Other backends return NULL with errno=ENOTSUP. CPU rendering uses the ordinary
 * yasl_private_surface_create_toplevel() and requires no native device. */
/**
 * Create a toplevel for application-managed native rendering.
 * @param d Display with YASL_PRIVATE_CAP_NATIVE_GRAPHICS.
 * @param spec Copied window options, with logical-pixel size and optional callbacks.
 * @param graphics_device Required native graphics device; currently id<MTLDevice> on Cocoa.
 * @return Owned surface, or NULL with errno set: EINVAL for invalid arguments,
 * ENOTSUP for an unsupported backend, or a backend error.
 * Cocoa retains the device. Query the drawable during draw, submit/present it,
 * then call yasl_private_surface_present() to record submission.
 * Callback ownership and timing follow yasl_private_surface_create_toplevel().
 */
struct yasl_surface *yasl_private_surface_create_native(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device);

/** Borrowed native drawable for the current draw target, currently Cocoa/Metal. */
struct yasl_private_native_frame {
    /** Borrowed id<CAMetalDrawable> on Cocoa. */
    void *drawable;
    /** Cocoa CAMetalLayer presentsWithTransaction setting for this frame. */
    bool transaction;
};

/* Query the current draw target's native drawable. Returns false for unsupported
 * backends, stale targets, or outside the draw callback. The result expires on
 * successful present, callback return, or surface destruction. Native renderers
 * submit/present the drawable, then call yasl_private_surface_present() to record it. */
/**
 * Get the current draw target's native drawable.
 * @param s Surface currently being drawn.
 * @param target Exact framebuffer identity supplied to this draw callback.
 * @param out Required output native frame.
 * @return true if available, false for NULL arguments, unsupported backends,
 * stale targets, or calls outside draw. The drawable is borrowed and expires on
 * successful present, callback return, or surface destruction. Currently Cocoa only.
 */
bool yasl_private_surface_get_native_frame(struct yasl_surface *s,
        const struct yasl_private_framebuffer *target, struct yasl_private_native_frame *out);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_NATIVE_H */
