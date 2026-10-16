/** @file Optional EGL/OpenGL/OpenGL ES contexts on Linux. */
#ifndef YASL_GL_H
#define YASL_GL_H
#include "yasl/yasl.h"
#ifdef __cplusplus
extern "C" {
#endif
enum yasl_gl_profile { YASL_GL_ANY, YASL_GL_CORE, YASL_GL_COMPATIBILITY };
enum yasl_gl_api { YASL_GL_OPENGL, YASL_GL_OPENGL_ES };
typedef void (*yasl_gl_proc)(void);
struct yasl_gl_options {
    int major, minor;
    enum yasl_gl_profile profile;
    enum yasl_gl_api api;
    int red_bits, green_bits, blue_bits, alpha_bits;
    int depth_bits, stencil_bits, samples;
    bool debug, forward_compat, srgb;
    yasl_surface *share;
};
/** C and C++ initializer: GL 3.2 core, RGBA8, D24, S8. */
#define YASL_GL_OPTIONS_INIT                                                                       \
    {3, 2, YASL_GL_CORE, YASL_GL_OPENGL, 8, 8, 8, 8, 24, 8, 0, false, false, false, NULL}
/** NULL gl selects defaults. options->type must be NATIVE. share, when set,
 * must be a live GL surface. Detach the owned context from every rendering
 * thread before destruction. Cocoa fails with errno=ENOTSUP. */
YASL_API yasl_surface *yasl_gl_surface_create(const struct yasl_surface_options *options,
                                              const struct yasl_gl_options *gl);
/** These four operations may run on a rendering thread. The application must
 * synchronize surface/display destruction. NULL make_current detaches. */
YASL_API int yasl_gl_make_current(yasl_surface *surface);
YASL_API yasl_gl_proc yasl_gl_get_proc_address(const char *name);
YASL_API int yasl_gl_set_swap_interval(yasl_surface *surface, int interval);
YASL_API int yasl_gl_present(yasl_surface *surface);
#ifdef __cplusplus
}
#endif
#endif
