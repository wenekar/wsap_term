#include "internal.h"

#include <errno.h>

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial);
static const struct xdg_wm_base_listener wm_base_listener;

static void presentation_clock_id(void *data, struct wp_presentation *presentation,
        uint32_t clk_id)
{
    (void)presentation;
    struct wayland_display *c = data;
    struct timespec ts;
    c->pres_clock = (clockid_t)clk_id;
    c->pres_clock_known = clock_gettime(c->pres_clock, &ts) == 0;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t conn_pres_to_mono(const struct wayland_display *c, uint64_t ns)
{
    if (c->pres_clock == CLOCK_MONOTONIC)
        return ns;
    uint64_t mono = clock_ns(CLOCK_MONOTONIC);
    uint64_t pres = clock_ns(c->pres_clock);
    return ns + mono - pres;
}

uint64_t conn_mono_to_pres(const struct wayland_display *c, uint64_t ns)
{
    if (c->pres_clock == CLOCK_MONOTONIC)
        return ns;
    uint64_t mono = clock_ns(CLOCK_MONOTONIC);
    uint64_t pres = clock_ns(c->pres_clock);
    return ns + pres - mono;
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = presentation_clock_id,
};

static void output_geometry(void *data, struct wl_output *output,
        int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
        int32_t subpixel, const char *make, const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)physical_width;
    (void)physical_height; (void)subpixel; (void)make; (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
        int32_t width, int32_t height, int32_t refresh)
{
    (void)output; (void)width; (void)height;
    struct wl_conn_output *o = data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && refresh > 0)
        o->pending_refresh_ns = (uint32_t)(1000000000000ull / (uint32_t)refresh);
}

static void conn_follow_outputs(struct wayland_display *c)
{
    for (struct wayland_surface *w = c->surfaces; w; w = w->conn_next)
        surface_follow_outputs(w);
}

static void output_done(void *data, struct wl_output *output)
{
    (void)output;
    struct wl_conn_output *o = data;
    if (o->pending_scale == o->scale && o->pending_refresh_ns == o->refresh_ns)
        return;
    o->scale = o->pending_scale;
    o->refresh_ns = o->pending_refresh_ns;
    conn_follow_outputs(o->conn);
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    struct wl_conn_output *o = data;
    if (factor > 0)
        o->pending_scale = factor;
}

static void output_name(void *data, struct wl_output *output, const char *name)
{ (void)data; (void)output; (void)name; }

static void output_description(void *data, struct wl_output *output, const char *description)
{ (void)data; (void)output; (void)description; }

static const struct wl_output_listener output_listener = {
    .name = output_name,
    .description = output_description,
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void conn_bind_output(struct wayland_display *c, struct wl_registry *registry,
        uint32_t name, uint32_t version)
{
    struct wl_conn_output *o = calloc(1, sizeof(*o));
    if (!o) return;
    o->proxy = wl_registry_bind(registry, name, &wl_output_interface,
            version < 3 ? version : 3);
    if (!o->proxy) {
        free(o);
        return;
    }
    o->conn = c;
    o->name = name;
    o->scale = o->pending_scale = 1;
    o->mask = ~c->output_masks & (c->output_masks + 1);
    c->output_masks |= o->mask;
    struct wl_conn_output **tail = &c->outputs;
    while (*tail)
        tail = &(*tail)->next;
    *tail = o;
    wl_output_add_listener(o->proxy, &output_listener, o);
}

static void output_destroy(struct wl_conn_output *o)
{
    if (wl_output_get_version(o->proxy) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
        wl_output_release(o->proxy);
    else
        wl_output_destroy(o->proxy);
    o->conn->output_masks &= ~o->mask;
    free(o);
}

static bool conn_remove_output(struct wayland_display *c, uint32_t name)
{
    struct wl_conn_output **link = &c->outputs;
    while (*link && (*link)->name != name)
        link = &(*link)->next;
    struct wl_conn_output *o = *link;
    if (!o)
        return false;
    *link = o->next;
    for (struct wayland_surface *w = c->surfaces; w; w = w->conn_next)
        w->outputs &= ~o->mask;
    output_destroy(o);
    conn_follow_outputs(c);
    return true;
}

void conn_destroy_outputs(struct wayland_display *c)
{
    while (c->outputs) {
        struct wl_conn_output *o = c->outputs;
        c->outputs = o->next;
        output_destroy(o);
    }
}

uint64_t conn_output_mask(const struct wayland_display *c, const struct wl_output *proxy)
{
    for (const struct wl_conn_output *o = c->outputs; o; o = o->next)
        if (o->proxy == proxy)
            return o->mask;
    return 0;
}

uint32_t conn_output_scale_120(const struct wayland_display *c, uint64_t outputs)
{
    int32_t scale = 0;
    for (const struct wl_conn_output *o = c->outputs; o; o = o->next)
        if ((o->mask & outputs) && o->scale > scale)
            scale = o->scale;
    return (uint32_t)scale * YASL_PRIVATE_SCALE_UNIT;
}

static void registry_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct wayland_display *c = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        c->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 6 ? version : 6);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        c->shm = wl_registry_bind(registry, name,
                &wl_shm_interface, version < 2 ? version : 2);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        conn_bind_output(c, registry, name, version);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        c->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                                           version < 3 ? version : 3);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        c->wm_base = wl_registry_bind(registry, name,
                &xdg_wm_base_interface, version < 7 ? version : 7);
        if (c->wm_base)
            xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        c->decor_mgr = wl_registry_bind(registry, name,
                &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        c->primary_selection_mgr = wl_registry_bind(registry, name,
                &zwp_primary_selection_device_manager_v1_interface, 1);
        for (struct seat *s = c->seats; s; s = s->next)
            if (s->clipboard)
                clipboard_set_primary_manager(s->clipboard, c->primary_selection_mgr);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        c->cursor_shape_mgr = wl_registry_bind(registry, name,
                &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        c->relative_pointer_mgr = wl_registry_bind(registry, name,
                &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        c->pointer_constraints = wl_registry_bind(registry, name,
                &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, xdg_system_bell_v1_interface.name) == 0) {
        c->system_bell = wl_registry_bind(registry, name, &xdg_system_bell_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        c->scale_mgr = wl_registry_bind(registry, name,
                &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        c->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, zwp_tablet_manager_v2_interface.name) == 0) {
        c->tablet_mgr = wl_registry_bind(registry, name,
                &zwp_tablet_manager_v2_interface, 1);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        c->text_input_mgr = wl_registry_bind(registry, name,
                &zwp_text_input_manager_v3_interface, 1);
        for (struct seat *seat = c->seats; seat; seat = seat->next)
            seat_ensure_text_input(seat);
    } else if (strcmp(interface, wp_tearing_control_manager_v1_interface.name) == 0) {
        c->tearing_mgr = wl_registry_bind(registry, name,
                &wp_tearing_control_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
        c->presentation = wl_registry_bind(registry, name,
                &wp_presentation_interface, 1);
        if (c->presentation)
            wp_presentation_add_listener(c->presentation, &presentation_listener, c);
    } else if (strcmp(interface, wp_fifo_manager_v1_interface.name) == 0) {
        c->fifo_mgr = wl_registry_bind(registry, name,
                &wp_fifo_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_commit_timing_manager_v1_interface.name) == 0) {
        c->commit_timing_mgr = wl_registry_bind(registry, name,
                &wp_commit_timing_manager_v1_interface, 1);
    } else if (strcmp(interface, xdg_toplevel_drag_manager_v1_interface.name) == 0) {
        c->toplevel_drag_mgr = wl_registry_bind(registry, name,
                &xdg_toplevel_drag_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat_bind(c, registry, name, version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
        uint32_t name)
{
    (void)registry;
    struct wayland_display *c = data;
    if (c && !conn_remove_output(c, name))
        seat_remove(c, name);
}

const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void set_state(struct wayland_surface *w, uint32_t state)
{
    if (state == w->base.state) return;
    w->base.state = state;
    yasl_private_surface_notify_state(&w->base, state);
}

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
        int32_t width, int32_t height, struct wl_array *states)
{
    (void)toplevel;
    struct wayland_surface *w = data;

    uint32_t state = w->base.state & ~(YASL_PRIVATE_STATE_FULLSCREEN |
            YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_ACTIVATED |
            YASL_PRIVATE_STATE_SUSPENDED | YASL_PRIVATE_STATE_TILED);
    uint32_t *s;
    wl_array_for_each(s, states) {
        switch (*s) {
        case XDG_TOPLEVEL_STATE_FULLSCREEN: state |= YASL_PRIVATE_STATE_FULLSCREEN; break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED: state |= YASL_PRIVATE_STATE_MAXIMIZED; break;
        case XDG_TOPLEVEL_STATE_ACTIVATED: state |= YASL_PRIVATE_STATE_ACTIVATED; break;
        case XDG_TOPLEVEL_STATE_SUSPENDED: state |= YASL_PRIVATE_STATE_SUSPENDED; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            state |= YASL_PRIVATE_STATE_TILED;
            break;
        default: break;
        }
    }

    w->req_width = width;
    w->req_height = height;
    set_state(w, state);
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct wayland_surface *w = data;
    if (w && !w->closed)
        yasl_private_surface_notify_close(&w->base);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
        int32_t width, int32_t height)
{
    (void)toplevel;
    struct wayland_surface *w = data;
    w->bound_width = width;
    w->bound_height = height;
    if ((width > 0 && w->logical_width > width) ||
        (height > 0 && w->logical_height > height)) {
        w->size_pending = true;
        wayland_surface_schedule_redraw(w);
    }
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
        struct wl_array *capabilities)
{
    (void)data; (void)toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct wayland_surface *w = data;
    w->pending_serial = serial;
    w->has_pending_serial = true;
    w->size_pending = true;
    w->configured = true;
    /* CPU surfaces acknowledge with their SHM commit. External presenters
     * commit through EGL/Vulkan, so apply the configure and acknowledge it
     * before renderer code can submit that first native buffer. */
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        xdg_surface_ack_configure(xdg_surface, serial);
        w->has_pending_serial = false;
        if (apply_pending_geometry(w) < 0) {
            surface_fail(w);
            return;
        }
    }
    wayland_surface_schedule_redraw(w);
}

const struct xdg_surface_listener shell_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_decoration_configure(void *data,
        struct zxdg_toplevel_decoration_v1 *toplevel_decor, uint32_t mode)
{
    (void)toplevel_decor;
    struct wayland_surface *w = data;
    uint32_t state = w->base.state;
    if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
        state &= ~YASL_PRIVATE_STATE_CSD;
    else
        state |= YASL_PRIVATE_STATE_CSD;
    set_state(w, state);
}

static const struct zxdg_toplevel_decoration_v1_listener toplevel_decoration_listener = {
    .configure = toplevel_decoration_configure,
};

int surface_init_shell(struct wayland_surface *w, const char *title)
{
    w->xdg_surface = xdg_wm_base_get_xdg_surface(w->conn->wm_base, w->surface);
    if (!w->xdg_surface) return -1;
    xdg_surface_add_listener(w->xdg_surface, &shell_surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    if (!w->toplevel) return -1;
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);

    wayland_surface_set_title(w, title ? title : "YASL");

    const char *app_id = w->conn->app_id ? w->conn->app_id : "YASL";
    xdg_toplevel_set_app_id(w->toplevel, app_id);
    xdg_toplevel_set_min_size(w->toplevel, YASL_PRIVATE_MIN_LOGICAL_PX, YASL_PRIVATE_MIN_LOGICAL_PX);

    if (w->conn->decor_mgr) {
        w->toplevel_decor = zxdg_decoration_manager_v1_get_toplevel_decoration(
                w->conn->decor_mgr, w->toplevel);
        if (!w->toplevel_decor) return -1;
        zxdg_toplevel_decoration_v1_add_listener(w->toplevel_decor,
                &toplevel_decoration_listener, w);
        zxdg_toplevel_decoration_v1_set_mode(w->toplevel_decor,
                (w->base.creation_flags & YASL_PRIVATE_SURFACE_UNDECORATED)
                    ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                    : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    return 0;
}

bool wayland_surface_bell(struct wayland_surface *w)
{
    if (!w || w->closed || !w->conn || !w->surface) return false;
    if (!w->conn->system_bell) {
        errno = ENOTSUP; /* The compositor lacks xdg-system-bell. */
        return false;
    }
    xdg_system_bell_v1_ring(w->conn->system_bell, w->surface);
    return true;
}
