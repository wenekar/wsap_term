#include "internal.h"
#include <assert.h>
#include <errno.h>

static void display_free(struct wayland_display *c);
static uint64_t reconnect_tick(void *ud, yasl_private_timer_id id, uint64_t interval_ns);

static void display_cancel_read(struct wayland_display *c)
{
    if (!c->reading)
        return;
    wl_display_cancel_read(c->wl);
    c->reading = false;
}

/* A compositor going away is expected; a protocol error is a YASL bug worth reporting. */
static void report_display_error(struct wayland_display *c)
{
    if (wl_display_get_error(c->wl) != EPROTO)
        return;
    uint32_t object_id = 0;
    const struct wl_interface *iface = NULL;
    uint32_t protocol_err = wl_display_get_protocol_error(c->wl, &iface, &object_id);
    fprintf(stderr, "[yasl] protocol error %u on %s (object %u)\n",
            protocol_err, iface ? iface->name : "?", object_id);
}

void display_leave(struct wayland_display *c)
{
    if (--c->depth > 0)
        return;
    keyboard_settle_focus(c);
    while (c->dead) {
        struct wayland_surface *w = c->dead;
        c->dead = w->dead_next;
        free(w);
    }
}

/* Windows outlive the connection. Its objects stay until the next dispatch, after
 * the application has seen DISPLAY_LOST and released GPU objects built on them. */
static void display_lose(struct wayland_display *c)
{
    if (c->lost)
        return;
    c->lost = true;
    c->stale = true;
    atomic_store(&c->base.offline, true);
    report_display_error(c);
    display_cancel_read(c);
    if (c->loop && c->watch > 0)
        yasl_loop_del_fd(c->loop, c->watch);
    c->watch = 0;
    c->depth++;
    conn_drag_end(c);
    for (struct wayland_surface *w = c->surfaces, *next; w; w = next) {
        next = w->conn_next;
        if (w->parent)
            surface_fail(w);
    }
    display_leave(c);
    yasl_private_display_notify_event(&c->base, YASL_PRIVATE_EVENT_DISPLAY_LOST);
    c->reconnect_delay_ns = YASL_PRIVATE_MS_TO_NS(100);
    if (c->loop && !c->reconnect_timer)
        c->reconnect_timer = yasl_loop_add_timer(c->loop, c->reconnect_delay_ns, reconnect_tick, c);
}

void surface_fail(struct wayland_surface *w)
{
    if (w->closed)
        return;
    w->closed = true;
    yasl_private_surface_notify_close(&w->base);
}

/* The display fd is read in wayland_display_check, before any callback. */
static void display_ready(void *ud, int fd, short revents)
{
    (void)ud; (void)fd; (void)revents;
}

static void display_arm(struct wayland_display *c)
{
    while (!c->reading && wl_display_prepare_read(c->wl) != 0) {
        if (wl_display_dispatch_pending(c->wl) < 0) {
            display_lose(c);
            return;
        }
    }
    c->reading = true;
    short events = POLLIN;
    if (wl_display_flush(c->wl) < 0) {
        if (errno != EAGAIN) {
            display_cancel_read(c);
            display_lose(c);
            return;
        }
        events |= POLLOUT;
    }
    yasl_loop_set_fd_events(c->loop, c->watch, events);
}

/* A draw that cannot proceed now, for want of a buffer or a configure, is not
 * work: the release or configure that unblocks it schedules it again. */
static bool display_has_immediate_work(const struct wayland_display *c)
{
    for (const struct wayland_surface *w = c->surfaces; w; w = w->conn_next)
        if (surface_draw_ready(w))
            return true;
    return false;
}

/* Before the wait: dispatch what libwayland already holds, draw due surfaces,
 * and arm the read. While lost, only the reconnect timer runs. */
bool wayland_display_prepare(struct wayland_display *c)
{
    if (c->lost)
        return false;
    c->depth++;
    /* Losing the connection is reported as DISPLAY_LOST, not as a failed wait. */
    if (wl_display_dispatch_pending(c->wl) < 0)
        display_lose(c);
    if (!c->lost)
        wayland_display_flush(c);
    if (!c->lost)
        display_arm(c);
    bool ready = !c->lost && display_has_immediate_work(c);
    display_leave(c);
    return ready;
}

/* After the wait, before any callback: finish the read prepare armed, so
 * callbacks may do roundtrips, and dispatch what it brought. */
void wayland_display_check(struct wayland_display *c)
{
    if (!c->reading)
        return;
    c->depth++;
    short revents = yasl_private_loop_revents(c->loop, c->watch);
    c->reading = false;
    if (!(revents & POLLIN))
        wl_display_cancel_read(c->wl);
    else if (wl_display_read_events(c->wl) < 0)
        revents |= POLLERR;
    if (revents & POLLOUT) {
        if (wl_display_flush(c->wl) >= 0)
            yasl_loop_set_fd_events(c->loop, c->watch, POLLIN);
        else if (errno != EAGAIN)
            display_lose(c);
    }
    if (!c->lost && (wl_display_dispatch_pending(c->wl) < 0 ||
                     (revents & (POLLERR | POLLHUP | POLLNVAL))))
        display_lose(c);
    display_leave(c);
}

void wayland_display_flush(struct wayland_display *c)
{
    if (!c || c->lost)
        return;
    c->depth++;
    for (struct wayland_surface *w = c->surfaces, *next; w; w = next) {
        next = w->conn_next;
        surface_arm_deferred(w);
        surface_draw_frame(w);
    }
    if (!c->lost && c->wl) {
        if (wl_display_flush(c->wl) < 0 && errno != EAGAIN)
            display_lose(c);
    }
    display_leave(c);
}

#define RELEASE(field, destroy) do { if (c->field) { destroy(c->field); c->field = NULL; } } while (0)

static void connection_release(struct wayland_display *c)
{
    for (struct wayland_surface *w = c->surfaces; w; w = w->conn_next)
        surface_release_native(w);
    conn_drag_end(c);
    if (c->loop && c->watch > 0) yasl_loop_del_fd(c->loop, c->watch);
    c->watch = 0;
    if (c->wl) display_cancel_read(c);
    while (c->seats)
        seat_destroy(c->seats);
    RELEASE(toplevel_drag_mgr, xdg_toplevel_drag_manager_v1_destroy);
    RELEASE(primary_selection_mgr, zwp_primary_selection_device_manager_v1_destroy);
    RELEASE(data_manager, wl_data_device_manager_destroy);
    RELEASE(cursor_shape_mgr, wp_cursor_shape_manager_v1_destroy);
    RELEASE(relative_pointer_mgr, zwp_relative_pointer_manager_v1_destroy);
    RELEASE(pointer_constraints, zwp_pointer_constraints_v1_destroy);
    RELEASE(system_bell, xdg_system_bell_v1_destroy);
    RELEASE(cursor_theme, wl_cursor_theme_destroy);
    RELEASE(tablet_mgr, zwp_tablet_manager_v2_destroy);
    RELEASE(text_input_mgr, zwp_text_input_manager_v3_destroy);
    RELEASE(tearing_mgr, wp_tearing_control_manager_v1_destroy);
    RELEASE(fifo_mgr, wp_fifo_manager_v1_destroy);
    RELEASE(commit_timing_mgr, wp_commit_timing_manager_v1_destroy);
    RELEASE(presentation, wp_presentation_destroy);
    RELEASE(scale_mgr, wp_fractional_scale_manager_v1_destroy);
    RELEASE(viewporter, wp_viewporter_destroy);
    RELEASE(decor_mgr, zxdg_decoration_manager_v1_destroy);
    conn_destroy_outputs(c);
    RELEASE(wm_base, xdg_wm_base_destroy);
    RELEASE(shm, wl_shm_destroy);
    RELEASE(compositor, wl_compositor_destroy);
    RELEASE(registry, wl_registry_destroy);
#if YASL_HAS_EGL
    if (c->egl_initialized) eglTerminate(c->egl_display);
    c->egl_initialized = false;
    c->egl_display = EGL_NO_DISPLAY;
    c->egl_api = 0;
#endif
    RELEASE(wl, wl_display_disconnect);
    c->base.native_handle = NULL;
    c->cursor_theme_scale = 0;
    c->pres_clock_known = false;
    c->active_seat = NULL;
}

static bool connection_open(struct wayland_display *c)
{
    c->wl = wl_display_connect(NULL);
    if (!c->wl) return false;
    c->base.native_handle = c->wl;
#if YASL_HAS_EGL
    c->egl_display = eglGetDisplay((EGLNativeDisplayType)c->wl);
    if (c->egl_display != EGL_NO_DISPLAY) {
        EGLint major = 0, minor = 0;
        c->egl_initialized = eglInitialize(c->egl_display, &major, &minor) == EGL_TRUE;
    }
#endif
    c->registry = wl_display_get_registry(c->wl);
    if (!c->registry) return false;
    wl_registry_add_listener(c->registry, &registry_listener, c);
    if (wl_display_roundtrip(c->wl) < 0 || !c->compositor || !c->shm || !c->wm_base)
        return false;
    surface_init_cursor(c);
    if (c->loop) {
        c->watch = yasl_loop_add_fd(c->loop, wl_display_get_fd(c->wl), POLLIN, display_ready, c);
        if (c->watch <= 0) { c->watch = 0; return false; }
    }
    return display_input_init(c) == 0;
}

/* Recreates every window on a new connection; one that cannot be rebuilt is closed. */
static void display_rebuild(struct wayland_display *c)
{
    for (struct wayland_surface *w = c->surfaces; w && !c->lost; w = w->conn_next) {
        if (w->parent || w->closed || w->destroyed) continue;
        w->req_width = w->req_height = 0;
        if (surface_build_toplevel(w, w->base.state & YASL_PRIVATE_STATE_FULLSCREEN) < 0 && !c->lost)
            surface_fail(w);
    }
#if YASL_HAS_EGL
    for (struct wayland_surface *w = c->surfaces; w && !c->lost; w = w->conn_next)
        if (w->base.presenter == YASL_PRIVATE_PRESENTER_GL && !w->closed && !w->gl_built && !surface_build_gl(w))
            surface_fail(w);
#endif
    /* Seats announced their pointers before these surfaces existed, so lock them now. */
    for (struct wayland_surface *w = c->surfaces; w && !c->lost; w = w->conn_next)
        if (!w->closed && w->base.pointer_mode == YASL_PRIVATE_POINTER_RELATIVE)
            surface_lock_preferred(w);
    for (struct wayland_surface *w = c->surfaces; w && !c->lost; w = w->conn_next)
        if (!w->closed) yasl_private_surface_notify_refresh(&w->base);
}

static uint64_t reconnect_tick(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct wayland_display *c = ud;
    c->depth++;
    if (c->stale) {
        connection_release(c);
        c->stale = false;
    }
    if (!connection_open(c)) {
        connection_release(c);
        display_leave(c);
        uint64_t limit = YASL_PRIVATE_MS_TO_NS(2000);
        c->reconnect_delay_ns = c->reconnect_delay_ns * 2 < limit ? c->reconnect_delay_ns * 2 : limit;
        return c->reconnect_delay_ns;
    }
    c->reconnect_timer = 0;
    c->lost = false;
    yasl_private_display_notify_event(&c->base, YASL_PRIVATE_EVENT_DISPLAY_RESTORED);
    display_rebuild(c);
    /* Windows exist again only after the rebuild; a loss during it stays offline. */
    if (!c->lost)
        atomic_store(&c->base.offline, false);
    display_leave(c);
    return 0;
}

static void display_free(struct wayland_display *c)
{
    connection_release(c);
    if (c->loop && c->reconnect_timer) yasl_loop_del_timer(c->loop, c->reconnect_timer);
    if (c->xkb_ctx) xkb_context_unref(c->xkb_ctx);
    if (c->owns_loop && c->loop) {
        yasl_loop_destroy(c->loop);
        c->loop = NULL;
        c->base.loop = NULL;
    }
    free(c->app_id);
    free(c);
}

struct wayland_display *wayland_display_open(struct yasl_loop *loop, const struct yasl_private_display_spec *spec)
{
    struct wayland_display *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->owner = pthread_self();
    c->base.ops = &yasl_private_wayland_backend;
    c->base.reconnects = true;
    if (!loop) {
        c->loop = yasl_loop_create();
        c->owns_loop = true;
    } else {
        c->loop = loop;
    }
    c->base.loop = c->loop;
    if (!c->loop) goto err;
    if (spec) {
        if (spec->app_id) c->app_id = strdup(spec->app_id);
        c->base.app_id = c->app_id;
    }
    c->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!c->xkb_ctx || !connection_open(c))
        goto err;
    return c;

err:
    display_free(c);
    return NULL;
}

void wayland_display_close(struct wayland_display *c)
{
    if (!c) return;
    assert(!c->surfaces && "destroy every surface before its display");
    display_free(c);
}

uint32_t wayland_display_caps(const struct wayland_display *c)
{
    if (!c) return 0;
    uint32_t caps = YASL_PRIVATE_CAP_POPUPS | YASL_PRIVATE_CAP_CSD |
        YASL_PRIVATE_CAP_FRAME_PACING | YASL_PRIVATE_CAP_NATIVE_GRAPHICS;
    if (c->tablet_mgr) caps |= YASL_PRIVATE_CAP_TABLET;
    if (c->text_input_mgr) caps |= YASL_PRIVATE_CAP_TEXT_INPUT;
    struct seat *s = c->active_seat ? c->active_seat : c->seats;
    const struct wl_data_device *device = s ? clipboard_data_device(s->clipboard) : NULL;
    if (device && wl_data_device_get_version((struct wl_data_device *)device) >= 3)
        caps |= YASL_PRIVATE_CAP_DRAG;
    if (c->toplevel_drag_mgr)
        caps |= YASL_PRIVATE_CAP_DRAG_CARRY;
    if (c->primary_selection_mgr)
        caps |= YASL_PRIVATE_CAP_PRIMARY_SELECTION;
    if (c->relative_pointer_mgr && c->pointer_constraints)
        caps |= YASL_PRIVATE_CAP_RELATIVE_POINTER;
    return caps;
}

static void conn_link(struct wayland_display *c, struct wayland_surface *w)
{
    w->base.display = &c->base;
    /* The compositor sends leave during a press only when it takes the pointer. */
    w->input.leave_ends_press = true;
    w->conn = c;
    w->conn_next = c->surfaces;
    c->surfaces = w;
}

static void conn_unlink(struct wayland_surface *w)
{
    struct wayland_display *c = w->conn;
    if (!c) return;
    for (struct wayland_surface **p = &c->surfaces; *p; p = &(*p)->conn_next) {
        if (*p == w) {
            *p = w->conn_next;
            break;
        }
    }
    for (struct seat *s = c->seats; s; s = s->next)
        seat_forget_surface(s, w);
    w->conn = NULL;
}


static bool spec_valid(const struct yasl_private_window_spec *spec)
{
    size_t bytes;
    if (!spec || spec->size.x <= 0 || spec->size.y <= 0)
        return false;
    return spec->external_gpu && !(spec->flags & YASL_PRIVATE_SURFACE_CPU)
        ? spec->size.x <= INT32_MAX && spec->size.y <= INT32_MAX
        : buffer_size(spec->size.x, spec->size.y, &bytes);
}

int surface_build_toplevel(struct wayland_surface *w, bool fullscreen)
{
    struct wayland_display *c = w->conn;
    w->surface = wl_compositor_create_surface(c->compositor);
    if (!w->surface) return -1;
    w->base.native_handle = w->surface;
    wl_surface_set_user_data(w->surface, w);
    if (surface_init_scaling(w) < 0) return -1;
    if (surface_init_shell(w, w->title) < 0) return -1;
    if (w->base.creation_flags & YASL_PRIVATE_SURFACE_FIXED_SIZE) {
        xdg_toplevel_set_min_size(w->toplevel, w->logical_width, w->logical_height);
        xdg_toplevel_set_max_size(w->toplevel, w->logical_width, w->logical_height);
    }
    if (fullscreen)
        xdg_toplevel_set_fullscreen(w->toplevel, NULL);
    else if (w->base.state & YASL_PRIVATE_STATE_MAXIMIZED)
        xdg_toplevel_set_maximized(w->toplevel);
    if (w->tearing && c->tearing_mgr) {
        w->tearing_control = wp_tearing_control_manager_v1_get_tearing_control(
                c->tearing_mgr, w->surface);
        if (w->tearing_control)
            wp_tearing_control_v1_set_presentation_hint(w->tearing_control,
                    WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
    }
    w->size_pending = true;
    wl_surface_commit(w->surface);
    if (wl_display_roundtrip(c->wl) < 0) {
        display_lose(c);
        return -1;
    }
    if (apply_pending_geometry(w) < 0 || w->closed) return -1;
    return 0;
}

void surface_release_native(struct wayland_surface *w)
{
    surface_unlock_pointer(w);
    w->ime_seat = NULL;
#define DROP(field, destroy) do { if (w->field) { destroy(w->field); w->field = NULL; } } while (0)
    DROP(tearing_control, wp_tearing_control_v1_destroy);
#if YASL_HAS_EGL
    struct wayland_display *c = w->conn;
    if (w->egl_context) {
        if (eglGetCurrentContext() == w->egl_context)
            eglMakeCurrent(c->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(c->egl_display, w->egl_context);
        w->egl_context = 0;
    }
    if (w->egl_surface) eglDestroySurface(c->egl_display, w->egl_surface);
    w->egl_surface = 0;
    DROP(egl_window, wl_egl_window_destroy);
    w->gl_built = false;
#endif
    DROP(fifo, wp_fifo_v1_destroy);
    DROP(commit_timer, wp_commit_timer_v1_destroy);
    surface_feedback_destroy(w);
    DROP(frame_cb, wl_callback_destroy);
    for (int i = 0; i < NBUF; i++) buffer_destroy(&w->buffers[i]);
    DROP(frac_scale, wp_fractional_scale_v1_destroy);
    DROP(viewport, wp_viewport_destroy);
    DROP(toplevel_decor, zxdg_toplevel_decoration_v1_destroy);
    DROP(toplevel, xdg_toplevel_destroy);
    DROP(popup, xdg_popup_destroy);
    DROP(xdg_surface, xdg_surface_destroy);
    if (w->surface) {
        wl_surface_set_user_data(w->surface, NULL);
        wl_surface_destroy(w->surface);
        w->surface = NULL;
    }
#undef DROP
    w->base.native_handle = NULL;
    /* A Vulkan surface built on the old wl_surface is gone; allow a new one. */
    if (w->base.presenter == YASL_PRIVATE_PRESENTER_VULKAN)
        w->base.presenter = YASL_PRIVATE_PRESENTER_NONE;
    w->configured = w->has_pending_serial = w->has_shown = false;
    w->frame_active = w->want_draw = false;
    atomic_store(&w->frame_deferred, false);
    w->outputs = 0;
}

struct wayland_surface *wayland_surface_create_toplevel(struct wayland_display *c,
                                                  const struct yasl_private_window_spec *spec, void *graphics_device)
{
    (void)graphics_device;
    if (c && c->lost) { errno = ENOTCONN; return NULL; }
    if (!c || !spec_valid(spec))
        return NULL;
    int width = spec->size.x, height = spec->size.y;
    display_cancel_read(c);
    struct wayland_surface *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    c->depth++;
    conn_link(c, w);
    w->base.state = YASL_PRIVATE_STATE_CSD;
    w->base.creation_flags = spec->flags;
    w->base.presenter = (spec->external_gpu && !(spec->flags & YASL_PRIVATE_SURFACE_CPU))
        ? YASL_PRIVATE_PRESENTER_NONE : YASL_PRIVATE_PRESENTER_CPU;
    wayland_surface_set_callbacks(w, NULL, NULL);
    wayland_surface_set_title(w, spec->title ? spec->title : "YASL");
    w->cursor = YASL_PRIVATE_CURSOR_TEXT;
    w->floating_width = width;
    w->floating_height = height;
    w->logical_width = width;
    w->logical_height = height;
    w->scale_120 = YASL_PRIVATE_SCALE_UNIT;
    w->pending_scale_120 = YASL_PRIVATE_SCALE_UNIT;
    w->scale_factor = 1.0;
    w->tearing = spec->tearing;
    w->opaque = spec->fullscreen;
    w->clock.refresh_ns = c->outputs && c->outputs->refresh_ns
                        ? c->outputs->refresh_ns : YASL_PRIVATE_REFRESH_DEFAULT_NS;
    w->base.size = (struct yasl_private_vector2i){width, height};
    w->base.scale_120 = YASL_PRIVATE_SCALE_UNIT;
    if (surface_build_toplevel(w, spec->fullscreen) < 0) goto err;
    display_leave(c);
    return w;

err:
    wayland_surface_destroy(w);
    display_leave(c);
    return NULL;
}

void wayland_surface_set_callbacks(struct wayland_surface *w, const struct yasl_private_window_cbs *cbs, void *ud)
{
    if (w && !w->closed) {
        w->base.callbacks = cbs ? *cbs : (struct yasl_private_window_cbs){0};
        w->base.ud = ud;
        w->cbs = &w->base.callbacks;
        w->ud = ud;
    }
}

void wayland_surface_destroy(struct wayland_surface *w)
{
    if (!w || w->destroyed) return;
    for (struct wayland_surface *o = w->conn ? w->conn->surfaces : NULL; o; o = o->conn_next)
        assert(o->parent != w && "destroy a popup before its parent");
    surface_release_native(w);
    input_queue_clear(&w->input);
    free(w->ime_preedit);
    free(w->ime_commit);
    free(w->ime_shown);
    free(w->title);
    struct wayland_display *c = w->conn;
#if YASL_HAS_EGL
    for (struct wayland_surface *o = c ? c->surfaces : NULL; o; o = o->conn_next)
        if (o->gl_share == w) o->gl_share = NULL;
#endif
    conn_unlink(w);
    static const struct yasl_private_window_cbs no_callbacks = {0};
    *w = (struct wayland_surface){
        .base = w->base,
        .conn_next = w->conn_next,
        .cbs = &no_callbacks,
        .closed = true,
        .destroyed = true,
    };
    if (c && c->depth > 0) {
        w->dead_next = c->dead;
        c->dead = w;
    } else {
        free(w);
    }
}

void wayland_surface_schedule_redraw(struct wayland_surface *w)
{
    if (!w || w->closed) return;
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        yasl_private_surface_notify_refresh(&w->base);
        return;
    }
    w->want_draw = true;
    if (w->conn)
        yasl_private_loop_wake(w->conn->loop);
}

struct yasl_private_geometry wayland_surface_geometry(const struct wayland_surface *w)
{
    return (struct yasl_private_geometry){
        .size = w ? (struct yasl_private_vector2i){w->width, w->height} : (struct yasl_private_vector2i){0},
        .scale_120 = w ? w->scale_120 : YASL_PRIVATE_SCALE_UNIT,
    };
}

void wayland_surface_set_background_color(struct wayland_surface *w, uint32_t bg)
{
    (void)w; (void)bg;
}

/* The serial of the press a window request cites; it must still be held. */
static uint32_t held_press_serial(struct wayland_surface *s, struct seat **seat)
{
    *seat = s->conn->active_seat ? s->conn->active_seat : s->conn->seats;
    uint32_t serial = seat_held_press_serial(*seat);
    if (!serial || !(*seat)->proxy) {
        errno = ESTALE;
        return 0;
    }
    return serial;
}

bool wayland_surface_start_interactive_move(struct wayland_surface *s)
{
    if (!s || !s->toplevel || !s->conn) {
        errno = EINVAL;
        return false;
    }
    struct seat *seat;
    uint32_t serial = held_press_serial(s, &seat);
    if (!serial) return false;
    xdg_toplevel_move(s->toplevel, seat->proxy, serial);
    return true;
}

bool wayland_surface_start_interactive_resize(struct wayland_surface *s, enum yasl_private_resize_edge edge)
{
    if (!s || !s->toplevel || !s->conn || edge == YASL_PRIVATE_EDGE_NONE) {
        errno = EINVAL;
        return false;
    }
    struct seat *seat;
    uint32_t serial = held_press_serial(s, &seat);
    if (!serial) return false;
    xdg_toplevel_resize(s->toplevel, seat->proxy, serial, (uint32_t)edge);
    return true;
}

bool wayland_surface_show_window_menu(struct wayland_surface *s, struct yasl_private_vector2i pos)
{
    if (!s || !s->toplevel || !s->conn) {
        errno = EINVAL;
        return false;
    }
    struct seat *seat;
    uint32_t serial = held_press_serial(s, &seat);
    if (!serial) return false;
    uint32_t scale = s->scale_120 ? s->scale_120 : YASL_PRIVATE_SCALE_UNIT;
    xdg_toplevel_show_window_menu(s->toplevel, seat->proxy, serial,
                                  yasl_private_scale_to_logical(pos.x, scale),
                                  yasl_private_scale_to_logical(pos.y, scale));
    return true;
}
