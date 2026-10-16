#ifndef YASL_PRIVATE_GL_OPTIONS_H
#define YASL_PRIVATE_GL_OPTIONS_H
#include <stdbool.h>
/* Attribute values are shared with the public GL bridge; not installed. */
enum {
    YASL_PRIVATE_GL_END = 0, YASL_PRIVATE_GL_MAJOR = 1, YASL_PRIVATE_GL_MINOR,
    YASL_PRIVATE_GL_PROFILE, YASL_PRIVATE_GL_DEBUG, YASL_PRIVATE_GL_FORWARD_COMPAT,
    YASL_PRIVATE_GL_RED_BITS, YASL_PRIVATE_GL_GREEN_BITS, YASL_PRIVATE_GL_BLUE_BITS,
    YASL_PRIVATE_GL_ALPHA_BITS, YASL_PRIVATE_GL_DEPTH_BITS, YASL_PRIVATE_GL_STENCIL_BITS,
    YASL_PRIVATE_GL_SAMPLES, YASL_PRIVATE_GL_SRGB, YASL_PRIVATE_GL_API
};
enum { YASL_PRIVATE_GL_ANY = 0, YASL_PRIVATE_GL_CORE = 1, YASL_PRIVATE_GL_COMPATIBILITY = 2 };
enum { YASL_PRIVATE_GL_OPENGL = 0, YASL_PRIVATE_GL_OPENGL_ES = 1 };
struct yasl_private_gl_options {
    int major, minor, profile, debug, forward_compat;
    int red_bits, green_bits, blue_bits, alpha_bits, depth_bits, stencil_bits;
    int samples, srgb, api;
};
bool yasl_private_gl_parse_options(const int *, struct yasl_private_gl_options *);
#endif
