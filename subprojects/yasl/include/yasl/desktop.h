/** @file Optional desktop services; window operations use the owning thread. */
#ifndef YASL_DESKTOP_H
#define YASL_DESKTOP_H
#include "yasl/yasl.h"
#ifdef __cplusplus
extern "C" {
#endif
enum yasl_appearance {
    YASL_APPEARANCE_DARK = 1u,
    YASL_APPEARANCE_LIGHT = 2u,
    YASL_APPEARANCE_HIGH_CONTRAST = 4u
};
YASL_API int yasl_open_uri(const char *uri);
YASL_API int yasl_notify(const char *title, const char *body);
YASL_API unsigned yasl_get_appearance(void);
YASL_API int yasl_surface_bell(yasl_surface *surface);
/** Full byte length excluding NUL, zero on failure. Buffer unchanged unless cap
 * exceeds returned length. NULL/zero queries size. These three are thread-safe. */
YASL_API size_t yasl_get_executable_path(char *out, size_t capacity);
YASL_API size_t yasl_get_resource_dir(char *out, size_t capacity);
YASL_API size_t yasl_get_default_locale(char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
