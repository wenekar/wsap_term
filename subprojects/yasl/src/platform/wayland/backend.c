#include "platform/wayland/internal.h"
#include "platform/freedesktop/dbus.h"
#include <errno.h>

static struct yasl_private_display *impl_display_open(struct yasl_loop *loop, const struct yasl_private_display_spec *spec)
{
    struct wayland_display *result = wayland_display_open(loop, spec);
    return result ? &result->base : NULL;
}

static void impl_display_close(struct yasl_private_display *d)
{
    wayland_display_close((struct wayland_display *)d);
}

static uint32_t impl_display_caps(const struct yasl_private_display *d)
{
    return wayland_display_caps((const struct wayland_display *)d);
}

static bool impl_display_prepare(struct yasl_private_display *d)
{
    return wayland_display_prepare((struct wayland_display *)d);
}

static void impl_display_check(struct yasl_private_display *d)
{
    wayland_display_check((struct wayland_display *)d);
}

static bool impl_surface_frame_in_flight(const struct yasl_surface *s)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    /* A swap on a rendering thread is in flight before the owner arms its callback. */
    return w->frame_cb != NULL || atomic_load(&w->frame_deferred);
}

static void impl_display_flush(struct yasl_private_display *d)
{
    wayland_display_flush((struct wayland_display *)d);
}

static int impl_display_modifiers(const struct yasl_private_display *d)
{
    return wayland_display_modifiers((const struct wayland_display *)d);
}

static int impl_display_double_click_ms(const struct yasl_private_display *d)
{
    /* Wayland has no protocol for it; desktops publish it as a setting. */
    (void)d;
    return portal_double_click_ms();
}

static bool impl_display_copy_text(struct yasl_private_display *d, enum yasl_private_selection sel,
                                   const char *text)
{
    bool ok = wayland_display_copy_text((struct wayland_display *)d, sel, text);
    if (!ok && errno == 0) errno = ENOTSUP;
    return ok;
}

static bool impl_display_request_paste(struct yasl_private_display *d, enum yasl_private_selection sel,
                                       struct yasl_surface *target, uint64_t request_id)
{
    bool ok = wayland_display_request_paste((struct wayland_display *)d, sel,
            (struct wayland_surface *)target, request_id);
    if (!ok && errno == 0) errno = ENOTSUP;
    return ok;
}

static bool impl_display_drag_start(struct yasl_private_display *d, struct yasl_surface *origin, const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *l, void *ud)
{
    return wayland_display_drag_start((struct wayland_display *)d, (struct wayland_surface *)origin, image, l, ud);
}

static void impl_display_drag_cancel(struct yasl_private_display *d)
{
    wayland_display_drag_cancel((struct wayland_display *)d);
}

static struct yasl_surface *impl_surface_create_toplevel(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device)
{
    struct wayland_surface *result = wayland_surface_create_toplevel((struct wayland_display *)d, spec, graphics_device);
    return result ? &result->base : NULL;
}

static bool impl_surface_update_text_input(struct yasl_surface *s)
{
    return wayland_surface_update_text_input((struct wayland_surface *)s);
}

#if YASL_HAS_EGL
static bool egl_configure(struct wayland_display *d, const struct yasl_private_gl_options *o, EGLConfig *config)
{
    if (!d->egl_initialized) return false;
    EGLint renderable = o->api == YASL_PRIVATE_GL_OPENGL_ES
        ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT;
#ifdef EGL_OPENGL_ES_BIT
    if (o->api == YASL_PRIVATE_GL_OPENGL_ES && o->major < 2)
        renderable = EGL_OPENGL_ES_BIT;
#endif
#ifdef EGL_OPENGL_ES3_BIT_KHR
    if (o->api == YASL_PRIVATE_GL_OPENGL_ES && o->major >= 3)
        renderable = EGL_OPENGL_ES3_BIT_KHR;
#endif
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, renderable,
        EGL_RED_SIZE, o->red_bits,
        EGL_GREEN_SIZE, o->green_bits,
        EGL_BLUE_SIZE, o->blue_bits,
        EGL_ALPHA_SIZE, o->alpha_bits,
        EGL_DEPTH_SIZE, o->depth_bits,
        EGL_STENCIL_SIZE, o->stencil_bits,
        EGL_SAMPLE_BUFFERS, o->samples > 0 ? 1 : 0,
        EGL_SAMPLES, o->samples,
        EGL_NONE,
    };
    EGLint api = o->api == YASL_PRIVATE_GL_OPENGL_ES ? EGL_OPENGL_ES_API : EGL_OPENGL_API;
    if (d->egl_api && d->egl_api != api) return false;
    EGLint count = 0;
    if (!eglChooseConfig(d->egl_display, attrs, config, 1, &count) || count < 1)
        return false;
    d->egl_api = api;
    return eglBindAPI(api) == EGL_TRUE;
}

bool surface_build_gl(struct wayland_surface *w)
{
    struct wayland_display *d = w->conn;
    const struct yasl_private_gl_options *o = &w->gl_options;
    if (!egl_configure(d, o, &w->egl_config)) return false;
    EGLContext share_context = EGL_NO_CONTEXT;
    if (w->gl_share) {
        if (!w->gl_share->gl_built && !surface_build_gl(w->gl_share)) return false;
        share_context = w->gl_share->egl_context;
    }
    w->egl_window = wl_egl_window_create(w->surface, w->width, w->height);
    if (!w->egl_window) return false;
    EGLint context_attrs[12];
    int n = 0;
    if (o->api == YASL_PRIVATE_GL_OPENGL_ES) {
        context_attrs[n++] = EGL_CONTEXT_CLIENT_VERSION;
        context_attrs[n++] = o->major;
    } else {
        context_attrs[n++] = EGL_CONTEXT_MAJOR_VERSION_KHR;
        context_attrs[n++] = o->major;
        context_attrs[n++] = EGL_CONTEXT_MINOR_VERSION_KHR;
        context_attrs[n++] = o->minor;
        if (o->profile != YASL_PRIVATE_GL_ANY) {
            context_attrs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
            context_attrs[n++] = o->profile == YASL_PRIVATE_GL_CORE
                ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
                : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR;
        }
        EGLint flags = (o->debug ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0) |
            (o->forward_compat ? EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR : 0);
        if (flags) {
            context_attrs[n++] = EGL_CONTEXT_FLAGS_KHR;
            context_attrs[n++] = flags;
        }
    }
    context_attrs[n] = EGL_NONE;
    w->egl_context = eglCreateContext(d->egl_display, w->egl_config, share_context, context_attrs);
    if (w->egl_context == EGL_NO_CONTEXT) { w->egl_context = 0; return false; }
    EGLint surface_attrs[] = {
#ifdef EGL_GL_COLORSPACE_KHR
        EGL_GL_COLORSPACE_KHR, o->srgb ? EGL_GL_COLORSPACE_SRGB_KHR : EGL_GL_COLORSPACE_LINEAR_KHR,
#endif
        EGL_NONE,
    };
    w->egl_surface = eglCreateWindowSurface(d->egl_display, w->egl_config,
            (EGLNativeWindowType)w->egl_window, surface_attrs);
    if (w->egl_surface == EGL_NO_SURFACE) { w->egl_surface = 0; return false; }
    w->gl_built = true;
    return true;
}

static struct yasl_surface *impl_surface_create_gl(struct yasl_private_display *display,
        const struct yasl_private_window_spec *spec, const int *attributes, struct yasl_surface *share)
{
    struct wayland_display *d = (struct wayland_display *)display;
    struct yasl_private_gl_options o;
    if (!spec || !d || !yasl_private_gl_parse_options(attributes, &o)) {
        errno = EINVAL;
        return NULL;
    }
    if (!d->egl_initialized) {
        errno = ENOTSUP;
        return NULL;
    }
    if (share && (share->display != display || !((struct wayland_surface *)share)->egl_context)) {
        errno = EINVAL;
        return NULL;
    }
    struct yasl_private_window_spec gl_spec = *spec;
    gl_spec.external_gpu = true;
    gl_spec.flags &= ~YASL_PRIVATE_SURFACE_CPU;
    struct wayland_surface *w = wayland_surface_create_toplevel(d, &gl_spec, NULL);
    if (!w) return NULL;
    w->gl_options = o;
    w->gl_share = (struct wayland_surface *)share;
    if (!surface_build_gl(w)) {
        wayland_surface_destroy(w);
        return NULL;
    }
    w->base.presenter = YASL_PRIVATE_PRESENTER_GL;
    return &w->base;
}

/* An EGL call failed: a lost context means the display is gone, anything else is a bad argument. */
static int egl_failure_errno(void)
{
    return eglGetError() == EGL_CONTEXT_LOST ? ENOTCONN : EINVAL;
}

static bool impl_gl_make_current(struct yasl_private_display *display, struct yasl_surface *surface)
{
    struct wayland_display *d = (struct wayland_display *)display;
    struct wayland_surface *w = (struct wayland_surface *)surface;
    if (!d) {
        errno = EINVAL;
        return false;
    }
    if (!w) {
        /* Nothing to detach when no context is current or the display is gone. */
        if (eglGetCurrentContext() == EGL_NO_CONTEXT || d->egl_display == EGL_NO_DISPLAY)
            return true;
        if (eglMakeCurrent(d->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
            errno = egl_failure_errno();
            return false;
        }
        return true;
    }
    if (w->conn != d || w->base.presenter != YASL_PRIVATE_PRESENTER_GL) {
        errno = EINVAL;
        return false;
    }
    /* A window whose rebuild failed has no context and binds nothing. */
    if (!w->egl_context) {
        errno = ENOTCONN;
        return false;
    }
    if (eglMakeCurrent(d->egl_display, w->egl_surface, w->egl_surface, w->egl_context) != EGL_TRUE) {
        errno = egl_failure_errno();
        return false;
    }
    return true;
}

static yasl_private_gl_proc impl_gl_get_proc_address(struct yasl_private_display *display, const char *name)
{
    (void)display;
    return name ? (yasl_private_gl_proc)eglGetProcAddress(name) : NULL;
}

static bool impl_gl_swap_interval(struct yasl_private_display *display, struct yasl_surface *surface, int interval)
{
    struct wayland_display *d = (struct wayland_display *)display;
    struct wayland_surface *w = (struct wayland_surface *)surface;
    return d && w && w->conn == d && w->base.presenter == YASL_PRIVATE_PRESENTER_GL &&
        impl_gl_make_current(display, surface) && eglSwapInterval(d->egl_display, interval) == EGL_TRUE;
}

static bool impl_gl_swap_buffers(struct yasl_private_display *display, struct yasl_surface *surface)
{
    struct wayland_display *d = (struct wayland_display *)display;
    struct wayland_surface *w = (struct wayland_surface *)surface;
    if (!d || !w || w->conn != d || w->closed || !w->configured ||
        w->base.presenter != YASL_PRIVATE_PRESENTER_GL)
        return false;
    if (!impl_gl_make_current(display, surface)) return false;
    /* Frame callbacks and feedback belong to the owner thread. Another thread only
     * flags the surface and wakes the loop; the owner arms them at its next flush. */
    bool owner = pthread_equal(pthread_self(), d->owner);
    if (owner) {
        wayland_surface_arm_frame(w);
        surface_request_presentation_feedback(w);
    }
    bool swapped = eglSwapBuffers(d->egl_display, w->egl_surface) == EGL_TRUE;
    if (!swapped) errno = egl_failure_errno();
    if (!owner) {
        atomic_store(&w->frame_deferred, true);
        if (d->loop) yasl_private_loop_kick(d->loop);
    }
    return swapped;
}
#endif

static struct yasl_surface *impl_surface_create_popup(struct yasl_surface *parent, const struct yasl_private_popup_spec *spec)
{
    struct wayland_surface *result = wayland_surface_create_popup((struct wayland_surface *)parent, spec);
    return result ? &result->base : NULL;
}

static bool impl_surface_popup_reposition(struct yasl_surface *popup, const struct yasl_private_popup_spec *spec)
{
    return wayland_surface_popup_reposition((struct wayland_surface *)popup, spec);
}

static void impl_surface_destroy(struct yasl_surface *s)
{
    wayland_surface_destroy((struct wayland_surface *)s);
}

static struct input_queue *impl_surface_input_queue(struct yasl_surface *s)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    return w ? &w->input : NULL;
}

static bool impl_surface_set_size(struct yasl_surface *s, struct yasl_private_vector2i size)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    if (!w || w->closed || w->destroyed || (!w->toplevel && !w->conn->lost) ||
        size.x <= 0 || size.y <= 0) {
        errno = EINVAL;
        return false;
    }
    /* Floating windows choose their own size; constrained states keep it for later. */
    w->floating_width = size.x;
    w->floating_height = size.y;
    /* A fixed-size window is pinned at its size, so the compositor must not stretch it. */
    if ((w->base.creation_flags & YASL_PRIVATE_SURFACE_FIXED_SIZE) && w->toplevel && !w->conn->lost) {
        xdg_toplevel_set_min_size(w->toplevel, size.x, size.y);
        xdg_toplevel_set_max_size(w->toplevel, size.x, size.y);
    }
    if (w->conn->lost ||
        (w->base.state & (YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_TILED)))
        return true;
    w->req_width = w->req_height = 0;
    w->size_pending = true;
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU && w->configured && apply_pending_geometry(w) < 0) {
        errno = EIO;
        return false;
    }
    wayland_surface_schedule_redraw(w);
    return true;
}

static bool impl_surface_set_fullscreen(struct yasl_surface *s, bool enabled)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    if (!w || w->closed || w->destroyed || !w->toplevel) {
        errno = EINVAL;
        return false;
    }
    if (enabled) xdg_toplevel_set_fullscreen(w->toplevel, NULL);
    else xdg_toplevel_unset_fullscreen(w->toplevel);
    return true;
}

static bool impl_surface_set_maximized(struct yasl_surface *s, bool enabled)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    if (!w || w->closed || w->destroyed || !w->toplevel) {
        errno = EINVAL;
        return false;
    }
    if (enabled) xdg_toplevel_set_maximized(w->toplevel);
    else xdg_toplevel_unset_maximized(w->toplevel);
    return true;
}

static bool impl_surface_minimize(struct yasl_surface *s)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    if (!w || w->closed || w->destroyed || !w->toplevel) {
        errno = EINVAL;
        return false;
    }
    xdg_toplevel_set_minimized(w->toplevel);
    return true;
}

static struct yasl_private_display *impl_surface_display(const struct yasl_surface *s)
{
    const struct wayland_surface *w = (const struct wayland_surface *)s;
    struct wayland_display *result = w ? w->conn : NULL;
    return result ? &result->base : NULL;
}

static void impl_surface_set_callbacks(struct yasl_surface *s, const struct yasl_private_window_cbs *cbs, void *ud)
{
    wayland_surface_set_callbacks((struct wayland_surface *)s, cbs, ud);
}

static void impl_surface_set_title(struct yasl_surface *s, const char *title)
{
    wayland_surface_set_title((struct wayland_surface *)s, title);
}

static void impl_surface_schedule_redraw(struct yasl_surface *s)
{
    wayland_surface_schedule_redraw((struct wayland_surface *)s);
}

static bool impl_surface_present(struct yasl_surface *s, const struct yasl_private_framebuffer *fb, const struct yasl_private_damage_list *damage)
{
    return wayland_surface_present((struct wayland_surface *)s, fb, damage);
}

static struct yasl_private_geometry impl_surface_geometry(const struct yasl_surface *s)
{
    return wayland_surface_geometry((const struct wayland_surface *)s);
}

static struct yasl_private_timing impl_surface_timing(const struct yasl_surface *s)
{
    return wayland_surface_timing((const struct wayland_surface *)s);
}

static uint64_t impl_surface_frame_time_ns(struct yasl_surface *s, uint64_t now)
{
    return yasl_private_clock_frame_timestamp_ns(&((struct wayland_surface *)s)->clock, now);
}

static void impl_surface_set_cursor(struct yasl_surface *s, enum yasl_private_cursor cursor)
{
    wayland_surface_set_cursor((struct wayland_surface *)s, cursor);
}

static bool impl_surface_set_pointer_mode(struct yasl_surface *s, enum yasl_private_pointer_mode mode)
{
    return wayland_surface_set_pointer_mode((struct wayland_surface *)s, mode);
}

static void impl_surface_set_background_color(struct yasl_surface *s, uint32_t bg)
{
    wayland_surface_set_background_color((struct wayland_surface *)s, bg);
}

static const char *impl_surface_ime_preedit(const struct yasl_surface *s)
{
    const struct wayland_surface *w = (const struct wayland_surface *)s;
    return w ? w->ime_shown : NULL;
}

static bool impl_surface_ime_preedit_cursor(const struct yasl_surface *s, int *begin, int *end)
{
    return wayland_surface_ime_preedit_cursor((const struct wayland_surface *)s, begin, end);
}

static void impl_surface_set_ime_caret(struct yasl_surface *s, struct yasl_private_rect2i caret)
{
    wayland_surface_set_ime_caret((struct wayland_surface *)s, caret);
}

static bool impl_surface_start_interactive_move(struct yasl_surface *s)
{
    return wayland_surface_start_interactive_move((struct wayland_surface *)s);
}

static bool impl_surface_start_interactive_resize(struct yasl_surface *s, enum yasl_private_resize_edge edge)
{
    return wayland_surface_start_interactive_resize((struct wayland_surface *)s, edge);
}

static bool impl_surface_show_window_menu(struct yasl_surface *s, struct yasl_private_vector2i pos)
{
    return wayland_surface_show_window_menu((struct wayland_surface *)s, pos);
}

static bool impl_surface_drag_carry(struct yasl_surface *s, struct yasl_private_vector2i offset)
{
    return wayland_surface_drag_carry((struct wayland_surface *)s, offset);
}

static bool impl_surface_place_under_pointer(struct yasl_surface *s, struct yasl_private_vector2i at)
{
    return wayland_surface_place_under_pointer((struct wayland_surface *)s, at);
}

static bool impl_surface_read_pixels(struct yasl_surface *s, uint32_t *out, size_t cap,
                                     struct yasl_private_vector2i *size)
{
    return wayland_surface_read_pixels((struct wayland_surface *)s, out, cap, size);
}

static bool impl_surface_bell(struct yasl_surface *s)
{
    return wayland_surface_bell((struct wayland_surface *)s);
}

static void *impl_display_get_native_handle(struct yasl_private_display *d)
{
    struct wayland_display *w = (struct wayland_display *)d;
    return w ? w->wl : NULL;
}

static void *impl_surface_get_native_handle(struct yasl_surface *s)
{
    struct wayland_surface *w = (struct wayland_surface *)s;
    return w ? w->surface : NULL;
}

const struct yasl_private_backend_ops yasl_private_wayland_backend = {
#if YASL_HAS_EGL
    .surface_create_gl = impl_surface_create_gl,
    .gl_make_current = impl_gl_make_current,
    .gl_get_proc_address = impl_gl_get_proc_address,
    .gl_swap_interval = impl_gl_swap_interval,
    .gl_swap_buffers = impl_gl_swap_buffers,
#endif
    .surface_update_text_input = impl_surface_update_text_input,
    .type = YASL_PRIVATE_BACKEND_WAYLAND,
    .display_open = impl_display_open,
    .display_close = impl_display_close,
    .display_caps = impl_display_caps,
    .display_prepare = impl_display_prepare,
    .display_check = impl_display_check,
    .surface_frame_in_flight = impl_surface_frame_in_flight,
    .display_flush = impl_display_flush,
    .display_modifiers = impl_display_modifiers,
    .display_double_click_ms = impl_display_double_click_ms,
    .display_copy_text = impl_display_copy_text,
    .display_request_paste = impl_display_request_paste,
    .display_drag_start = impl_display_drag_start,
    .display_drag_cancel = impl_display_drag_cancel,
    .surface_create_toplevel = impl_surface_create_toplevel,
    .surface_create_popup = impl_surface_create_popup,
    .surface_popup_reposition = impl_surface_popup_reposition,
    .surface_destroy = impl_surface_destroy,
    .surface_input_queue = impl_surface_input_queue,
    .surface_set_size = impl_surface_set_size,
    .surface_set_fullscreen = impl_surface_set_fullscreen,
    .surface_set_maximized = impl_surface_set_maximized,
    .surface_minimize = impl_surface_minimize,
    .surface_display = impl_surface_display,
    .surface_set_callbacks = impl_surface_set_callbacks,
    .surface_set_title = impl_surface_set_title,
    .surface_schedule_redraw = impl_surface_schedule_redraw,
    .surface_present = impl_surface_present,
    .surface_set_pointer_mode = impl_surface_set_pointer_mode,
    .surface_geometry = impl_surface_geometry,
    .surface_timing = impl_surface_timing,
    .surface_frame_time_ns = impl_surface_frame_time_ns,
    .surface_set_cursor = impl_surface_set_cursor,
    .surface_set_background_color = impl_surface_set_background_color,
    .surface_ime_preedit = impl_surface_ime_preedit,
    .surface_ime_preedit_cursor = impl_surface_ime_preedit_cursor,
    .surface_set_ime_caret = impl_surface_set_ime_caret,
    .surface_start_interactive_move = impl_surface_start_interactive_move,
    .surface_start_interactive_resize = impl_surface_start_interactive_resize,
    .surface_show_window_menu = impl_surface_show_window_menu,
    .surface_drag_carry = impl_surface_drag_carry,
    .surface_place_under_pointer = impl_surface_place_under_pointer,
    .surface_bell = impl_surface_bell,
    .surface_read_pixels = impl_surface_read_pixels,
    .display_get_native_handle = impl_display_get_native_handle,
    .surface_get_native_handle = impl_surface_get_native_handle,
};
