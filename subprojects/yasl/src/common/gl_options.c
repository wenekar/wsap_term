#include "gl_options.h"
#include <errno.h>
#include <stddef.h>
bool yasl_private_gl_parse_options(const int *attrs, struct yasl_private_gl_options *out)
{
    if (!out) { errno = EINVAL; return false; }
    struct yasl_private_gl_options o = {.major=3, .minor=2, .profile=YASL_PRIVATE_GL_CORE,
        .red_bits=8, .green_bits=8, .blue_bits=8, .alpha_bits=8, .depth_bits=24, .stencil_bits=8};
    if (attrs) for (size_t i=0;;i+=2) {
        if (i >= 128) { errno=EINVAL; return false; }
        int key=attrs[i];
        if (key==YASL_PRIVATE_GL_END) break;
        int v=attrs[i+1];
        switch(key) {
        case YASL_PRIVATE_GL_MAJOR: o.major=v; break;
        case YASL_PRIVATE_GL_MINOR: o.minor=v; break;
        case YASL_PRIVATE_GL_PROFILE: o.profile=v; break;
        case YASL_PRIVATE_GL_DEBUG: o.debug=v; break;
        case YASL_PRIVATE_GL_FORWARD_COMPAT: o.forward_compat=v; break;
        case YASL_PRIVATE_GL_RED_BITS: o.red_bits=v; break;
        case YASL_PRIVATE_GL_GREEN_BITS: o.green_bits=v; break;
        case YASL_PRIVATE_GL_BLUE_BITS: o.blue_bits=v; break;
        case YASL_PRIVATE_GL_ALPHA_BITS: o.alpha_bits=v; break;
        case YASL_PRIVATE_GL_DEPTH_BITS: o.depth_bits=v; break;
        case YASL_PRIVATE_GL_STENCIL_BITS: o.stencil_bits=v; break;
        case YASL_PRIVATE_GL_SAMPLES: o.samples=v; break;
        case YASL_PRIVATE_GL_SRGB: o.srgb=v; break;
        case YASL_PRIVATE_GL_API: o.api=v; break;
        default: errno=EINVAL; return false;
        }
    }
    if (o.major<1 || o.major>4 || o.minor<0 || o.minor>9 || o.profile<0 || o.profile>2 ||
        o.api<0 || o.api>1 || (unsigned)o.debug>1 || (unsigned)o.forward_compat>1 ||
        (unsigned)o.srgb>1 || (unsigned)o.red_bits>64 || (unsigned)o.green_bits>64 ||
        (unsigned)o.blue_bits>64 || (unsigned)o.alpha_bits>64 || (unsigned)o.depth_bits>64 ||
        (unsigned)o.stencil_bits>64 || (unsigned)o.samples>64) { errno=EINVAL; return false; }
    *out=o; return true;
}
