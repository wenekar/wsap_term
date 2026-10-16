#include "internal.h"

static void popup_configure(void *data, struct xdg_popup *popup,
        int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data; (void)popup; (void)x; (void)y; (void)width; (void)height;
}

static void popup_done(void *data, struct xdg_popup *popup)
{
    (void)popup;
    surface_fail(data);
}

static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token)
{
    (void)data; (void)popup; (void)token;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

static bool popup_spec_valid(const struct wayland_surface *parent,
                             const struct yasl_private_popup_spec *spec)
{
    if (!parent || !spec) return false;
    if (spec->anchor_edges > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT ||
         spec->gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT ||
         (spec->constraints & ~(unsigned)(XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)))
        return false;
    int64_t x = (int64_t)spec->anchor.x - parent->content.x;
    int64_t y = (int64_t)spec->anchor.y - parent->content.y;
    return x >= INT_MIN && x <= INT_MAX && y >= INT_MIN && y <= INT_MAX &&
           spec->size.x > 0 && spec->size.y > 0 &&
           spec->content_offset.x >= 0 && spec->content_offset.y >= 0 &&
           spec->content_size.x >= 0 && spec->content_size.y >= 0 &&
           (int64_t)spec->content_offset.x + spec->content_size.x <= spec->size.x &&
           (int64_t)spec->content_offset.y + spec->content_size.y <= spec->size.y;
}

static bool popup_apply_spec(struct wayland_surface *w, const struct yasl_private_popup_spec *spec)
{
    uint32_t scale = w->parent->scale_120;
    struct yasl_private_vector2i logical = {
        .x = (int)(((int64_t)spec->size.x * YASL_PRIVATE_SCALE_UNIT + scale - 1) / scale),
        .y = (int)(((int64_t)spec->size.y * YASL_PRIVATE_SCALE_UNIT + scale - 1) / scale),
    };
    int width = device_px(logical.x, scale);
    int height = device_px(logical.y, scale);
    size_t bytes;
    if (logical.x <= 0 || logical.y <= 0 || !buffer_size(width, height, &bytes))
        return false;
    struct yasl_private_vector2i content = spec->content_size;
    if (!content.x) content.x = spec->size.x - spec->content_offset.x;
    if (!content.y) content.y = spec->size.y - spec->content_offset.y;
    bool rescaled = scale != w->scale_120;
    w->scale_120 = w->pending_scale_120 = scale;
    w->scale_factor = (double)scale / YASL_PRIVATE_SCALE_UNIT;
    w->width = width;
    w->height = height;
    w->logical_width = logical.x;
    w->logical_height = logical.y;
    w->content = (struct yasl_private_rect2i){
        .x = spec->content_offset.x, .y = spec->content_offset.y,
        .width = content.x, .height = content.y
    };
    for (int i = 0; i < NBUF; i++)
        w->buffers[i].presented = false;
    if (w->viewport)
        wp_viewport_set_destination(w->viewport, logical.x, logical.y);
    else if ((rescaled || !w->configured) && scale % YASL_PRIVATE_SCALE_UNIT == 0 &&
             wl_proxy_get_version((struct wl_proxy *)w->surface) >=
             WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_set_buffer_scale(w->surface, (int32_t)(scale / YASL_PRIVATE_SCALE_UNIT));
    return true;
}

static void popup_set_geometry(struct wayland_surface *w)
{
    uint32_t scale = w->scale_120;
    int gx = yasl_private_scale_to_logical(w->content.x, scale);
    int gy = yasl_private_scale_to_logical(w->content.y, scale);
    int gw = yasl_private_scale_to_logical(w->content.width, scale);
    int gh = yasl_private_scale_to_logical(w->content.height, scale);
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    if (gx + gw > w->logical_width)
        gx = w->logical_width - gw > 0 ? w->logical_width - gw : 0;
    if (gy + gh > w->logical_height)
        gy = w->logical_height - gh > 0 ? w->logical_height - gh : 0;
    xdg_surface_set_window_geometry(w->xdg_surface, gx, gy, gw, gh);
}

static struct xdg_positioner *popup_positioner(struct wayland_surface *w,
        const struct yasl_private_popup_spec *spec)
{
    struct wayland_surface *parent = w->parent;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(w->conn->wm_base);
    if (!pos)
        return NULL;
    uint32_t scale = parent->scale_120;
    int gw = yasl_private_scale_to_logical(w->content.width, scale);
    int gh = yasl_private_scale_to_logical(w->content.height, scale);
    xdg_positioner_set_size(pos, gw > 0 ? gw : 1, gh > 0 ? gh : 1);
    struct yasl_private_vector2i at = {
        .x = spec->anchor.x - parent->content.x,
        .y = spec->anchor.y - parent->content.y,
    };
    int aw = yasl_private_scale_to_logical(spec->anchor.width, scale);
    int ah = yasl_private_scale_to_logical(spec->anchor.height, scale);
    xdg_positioner_set_anchor_rect(pos,
        yasl_private_scale_to_logical(at.x, scale),
        yasl_private_scale_to_logical(at.y, scale),
        aw > 0 ? aw : 1, ah > 0 ? ah : 1);
    uint32_t anchor = spec->anchor_edges;
    uint32_t gravity = spec->gravity;
    uint32_t constraints = spec->constraints;
    xdg_positioner_set_anchor(pos, anchor);
    xdg_positioner_set_gravity(pos, gravity);
    xdg_positioner_set_constraint_adjustment(pos, constraints);
    if (xdg_positioner_get_version(pos) >= XDG_POSITIONER_SET_REACTIVE_SINCE_VERSION)
        xdg_positioner_set_reactive(pos);
    return pos;
}

struct wayland_surface *wayland_surface_create_popup(struct wayland_surface *parent,
        const struct yasl_private_popup_spec *spec)
{
    struct wayland_display *c = parent ? parent->conn : NULL;
    struct seat *seat = c ? (c->active_seat ? c->active_seat : c->seats) : NULL;
    if (!c || !parent->xdg_surface || parent->closed || !popup_spec_valid(parent, spec)) {
        errno = EINVAL;
        return NULL;
    }
    if (c->lost) {
        errno = ENOTCONN;
        return NULL;
    }
    if (!seat || !seat->input_serial) {
        errno = ESTALE;
        return NULL;
    }

    struct wayland_surface *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->base.display = &c->base;
    w->base.state = YASL_PRIVATE_STATE_CSD;
    w->base.presenter = YASL_PRIVATE_PRESENTER_CPU;
    w->input.leave_ends_press = true;
    w->conn = c;
    w->conn_next = c->surfaces;
    c->surfaces = w;
    w->parent = parent;
    wayland_surface_set_callbacks(w, NULL, NULL);
    w->clock.refresh_ns = parent->clock.refresh_ns;

    w->surface = wl_compositor_create_surface(c->compositor);
    if (!w->surface)
        goto err;
    w->base.native_handle = w->surface;
    w->base.size = (struct yasl_private_vector2i){spec->size.x, spec->size.y};
    w->base.scale_120 = parent->scale_120;
    wl_surface_set_user_data(w->surface, w);
    if (surface_init_scaling(w) < 0)
        goto err;
    w->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, w->surface);
    if (!w->xdg_surface)
        goto err;
    xdg_surface_add_listener(w->xdg_surface, &shell_surface_listener, w);
    if (!popup_apply_spec(w, spec))
        goto err;
    struct xdg_positioner *pos = popup_positioner(w, spec);
    if (!pos)
        goto err;
    w->popup = xdg_surface_get_popup(w->xdg_surface, parent->xdg_surface, pos);
    xdg_positioner_destroy(pos);
    if (!w->popup)
        goto err;
    xdg_popup_add_listener(w->popup, &popup_listener, w);
    popup_set_geometry(w);
    xdg_popup_grab(w->popup, seat->proxy, seat->input_serial);
    w->want_draw = true;
    wl_surface_commit(w->surface);
    return w;

err:
    wayland_surface_destroy(w);
    return NULL;
}

bool wayland_surface_popup_reposition(struct wayland_surface *w, const struct yasl_private_popup_spec *spec)
{
    if (!w || !w->popup || w->closed || !w->conn || w->conn->lost ||
        !popup_spec_valid(w->parent, spec) ||
        xdg_popup_get_version(w->popup) < XDG_POPUP_REPOSITION_SINCE_VERSION)
        return false;
    struct yasl_private_popup_spec old = {
        .size = { .x = w->width, .y = w->height },
        .content_offset = { .x = w->content.x, .y = w->content.y },
        .content_size = { .x = w->content.width, .y = w->content.height },
    };
    if (!popup_apply_spec(w, spec)) {
        popup_apply_spec(w, &old);
        return false;
    }
    struct xdg_positioner *pos = popup_positioner(w, spec);
    if (!pos)
        return false;
    popup_set_geometry(w);
    static uint32_t token;
    xdg_popup_reposition(w->popup, pos, ++token);
    xdg_positioner_destroy(pos);
    wayland_surface_schedule_redraw(w);
    return true;
}
