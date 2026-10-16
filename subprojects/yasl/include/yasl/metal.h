/** @file Direct Metal layer; application configures its device/format and presents. */
#ifndef YASL_METAL_H
#define YASL_METAL_H
#include "yasl/yasl.h"
#ifdef __OBJC__
@class CAMetalLayer;
#else
typedef struct objc_object CAMetalLayer;
#endif
#ifdef __cplusplus
extern "C" {
#endif
/** Borrowed until surface destruction. YASL owns view/layer and drawableSize.
 * No device, drawable acquisition, command submission, or implicit present.
 * Returns -ENOTSUP on unsupported backends, -EBUSY for a different presenter.
 * Clears out on failure. Successful access reserves the surface for Metal. */
YASL_API int yasl_metal_get_layer(yasl_surface *surface, CAMetalLayer **out);
#ifdef __cplusplus
}
#endif
#endif
