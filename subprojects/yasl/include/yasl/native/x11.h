/** @file Borrowed x11 handles for an initialized, connected surface. */
#ifndef YASL_NATIVE_X11_H
#define YASL_NATIVE_X11_H
#include "yasl/yasl.h"
typedef struct _XDisplay Display;
#ifdef __cplusplus
extern "C" {
#endif
struct yasl_x11_handles {
    Display *display;
    uintptr_t window;
};
/** Owner thread only. Returns -ENOTSUP for another backend and -ENOTCONN
 * while disconnected. Clears out on failure. Handles expire on display loss
 * or surface destruction; query again after DISPLAY_RESTORED. Never destroy
 * borrowed objects or dispatch/close the connection independently. */
YASL_API int yasl_x11_get_native(const yasl_surface *surface, struct yasl_x11_handles *out);
#ifdef __cplusplus
}
#endif
#endif
