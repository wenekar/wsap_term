/** @file Borrowed cocoa handles for an initialized, connected surface. */
#ifndef YASL_NATIVE_COCOA_H
#define YASL_NATIVE_COCOA_H
#include "yasl/yasl.h"
#ifdef __OBJC__
@class NSApplication;
@class NSWindow;
@class NSView;
#else
typedef struct objc_object NSApplication;
typedef struct objc_object NSWindow;
typedef struct objc_object NSView;
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct yasl_cocoa_handles {
    NSApplication *application;
    NSWindow *window;
    NSView *view;
};
/** Owner thread only. Returns -ENOTSUP for another backend and -ENOTCONN
 * while disconnected. Clears out on failure. Handles expire on display loss
 * or surface destruction; query again after DISPLAY_RESTORED. Never destroy
 * borrowed objects or dispatch/close the connection independently. */
YASL_API int yasl_cocoa_get_native(const yasl_surface *surface, struct yasl_cocoa_handles *out);
#ifdef __cplusplus
}
#endif
#endif
