#define _POSIX_C_SOURCE 200809L
#include "backend.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

void yasl_private_move_trace(const struct yasl_private_display *d,
                             const struct yasl_surface *s, const char *stage,
                             const char *format, ...)
{
    if (!d || !d->trace_moves)
        return;
    const char *backend = "unknown";
    switch (yasl_private_display_backend(d)) {
    case YASL_PRIVATE_BACKEND_COCOA: backend = "cocoa"; break;
    case YASL_PRIVATE_BACKEND_X11: backend = "x11"; break;
    case YASL_PRIVATE_BACKEND_WAYLAND: backend = "wayland"; break;
    default: break;
    }
    int saved_errno = errno;
    va_list args;
    va_start(args, format);
    flockfile(stderr);
    fprintf(stderr, "[wsap-move] t=%" PRIu64 " backend=%s %s",
            yasl_private_loop_now_ns() / 1000000, backend, stage);
    if (s && s->id)
        fprintf(stderr, " surface=%" PRIu64, s->id);
    fputc(' ', stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(args);
    errno = saved_errno;
}
