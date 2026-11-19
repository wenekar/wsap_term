#define _POSIX_C_SOURCE 200809L
#include "common/types.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

int main(void)
{
    struct yasl_private_display_spec spec = {.backend = (enum yasl_private_backend)999};
    errno = 0;
    assert(!yasl_private_display_open(NULL, &spec));
    assert(errno == EINVAL);
    assert(yasl_private_display_backend(NULL) == YASL_PRIVATE_BACKEND_AUTO);
#if !YASL_HAS_COCOA
    spec.backend = YASL_PRIVATE_BACKEND_COCOA;
    errno = 0;
    assert(!yasl_private_display_open(NULL, &spec));
    assert(errno == ENOSYS);
#endif
#if !YASL_HAS_WAYLAND
    spec.backend = YASL_PRIVATE_BACKEND_WAYLAND;
    errno = 0;
    assert(!yasl_private_display_open(NULL, &spec));
    assert(errno == ENOSYS);
#endif
#if !YASL_HAS_X11
    spec.backend = YASL_PRIVATE_BACKEND_X11;
    errno = 0;
    assert(!yasl_private_display_open(NULL, &spec));
    assert(errno == ENOSYS);
#endif
    return 0;
}
