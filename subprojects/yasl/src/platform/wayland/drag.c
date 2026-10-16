#include "internal.h"

static void icon_clear(struct drag_icon *icon)
{
    if (icon->viewport) wp_viewport_destroy(icon->viewport);
    if (icon->surface) wl_surface_destroy(icon->surface);
    if (icon->buffer) wl_buffer_destroy(icon->buffer);
    if (icon->pool) wl_shm_pool_destroy(icon->pool);
    if (icon->mapping) munmap(icon->mapping, icon->size);
    if (icon->pool) close(icon->fd);
    *icon = (struct drag_icon){0};
}

static bool icon_create(struct wayland_display *c, const struct yasl_private_drag_image *image)
{
    struct drag_icon *icon = &c->drag.icon;
    size_t bytes;
    if (!image->pixels || !buffer_size(image->size.x, image->size.y, &bytes))
        return false;
    int fd = memfd_create("yasl_private_drag_icon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return false;
    void *mapping = MAP_FAILED;
    if (ftruncate(fd, (off_t)bytes) == 0 &&
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0)
        mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return false;
    }
    memcpy(mapping, image->pixels, bytes);
    icon->mapping = mapping;
    icon->size = bytes;
    icon->pool = wl_shm_create_pool(c->shm, fd, (int32_t)bytes);
    if (!icon->pool) {
        close(fd);
        icon_clear(icon);
        return false;
    }
    icon->fd = fd;
    icon->buffer = wl_shm_pool_create_buffer(icon->pool, 0, image->size.x,
        image->size.y, image->size.x * 4, WL_SHM_FORMAT_ARGB8888);
    icon->surface = wl_compositor_create_surface(c->compositor);
    if (!icon->buffer || !icon->surface) {
        icon_clear(icon);
        return false;
    }
    return true;
}

static void icon_show(struct wayland_display *c, const struct yasl_private_drag_image *image)
{
    struct drag_icon *icon = &c->drag.icon;
    uint32_t scale = c->drag.scale_120;
    if (c->viewporter) {
        icon->viewport = wp_viewporter_get_viewport(c->viewporter, icon->surface);
        if (icon->viewport)
            wp_viewport_set_destination(icon->viewport,
                yasl_private_scale_to_logical(image->size.x, scale),
                yasl_private_scale_to_logical(image->size.y, scale));
    } else if (scale % YASL_PRIVATE_SCALE_UNIT == 0 &&
               wl_proxy_get_version((struct wl_proxy *)icon->surface) >=
               WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_set_buffer_scale(icon->surface, (int32_t)(scale / YASL_PRIVATE_SCALE_UNIT));
    }
    if (wl_proxy_get_version((struct wl_proxy *)icon->surface) >=
        WL_SURFACE_OFFSET_SINCE_VERSION)
        wl_surface_offset(icon->surface,
                          -yasl_private_scale_to_logical(image->hotspot.x, scale),
                          -yasl_private_scale_to_logical(image->hotspot.y, scale));
    wl_surface_attach(icon->surface, icon->buffer, 0, 0);
    wl_surface_damage_buffer(icon->surface, 0, 0, image->size.x, image->size.y);
    wl_surface_commit(icon->surface);
}

static void drag_clear(struct wayland_display *c, bool ended)
{
    struct conn_drag *d = &c->drag;
    if (d->toplevel) {
        if (ended)
            xdg_toplevel_drag_v1_destroy(d->toplevel);
        else
            wl_proxy_destroy((struct wl_proxy *)d->toplevel);
    }
    if (d->source)
        wl_data_source_destroy(d->source);
    icon_clear(&d->icon);
    *d = (struct conn_drag){0};
}

static void drag_finish(struct wayland_display *c, enum yasl_private_drag_end how, bool ended)
{
    struct yasl_private_drag_listener l = c->drag.listener;
    void *ud = c->drag.ud;
    drag_clear(c, ended);
    if (l.end)
        l.end(ud, how);
}

void conn_drag_end(struct wayland_display *c)
{
    if (c && c->drag.source)
        drag_finish(c, YASL_PRIVATE_DRAG_CANCELLED, false);
}

static void source_target(void *ud, struct wl_data_source *s, const char *mime) { (void)ud; (void)s; (void)mime; }
static void source_send(void *ud, struct wl_data_source *s, const char *mime, int32_t fd) { (void)ud; (void)s; (void)mime; close(fd); }
static void source_action(void *ud, struct wl_data_source *s, uint32_t action) { (void)ud; (void)s; (void)action; }

static void source_cancelled(void *ud, struct wl_data_source *s)
{
    (void)s;
    struct wayland_display *c = ud;
    bool released = c->drag.dropped;
    drag_finish(c, released ? YASL_PRIVATE_DRAG_DROPPED : YASL_PRIVATE_DRAG_CANCELLED, true);
}

static void source_dropped(void *ud, struct wl_data_source *s)
{
    (void)s;
    ((struct wayland_display *)ud)->drag.dropped = true;
}

static void source_finished(void *ud, struct wl_data_source *s)
{
    (void)s;
    drag_finish(ud, YASL_PRIVATE_DRAG_DROPPED, true);
}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dropped,
    .dnd_finished = source_finished,
    .action = source_action,
};

/* A seat whose press is still held over the origin window; its serial starts the drag. */
static struct seat *drag_seat(struct wayland_display *c, struct wayland_surface *origin)
{
    struct wayland_surface *root = surface_root(origin);
    struct seat *active = c->active_seat ? c->active_seat : c->seats;
    if (active && seat_held_press_serial(active) &&
        surface_root(active->press_surface) == root)
        return active;
    for (struct seat *s = c->seats; s; s = s->next)
        if (seat_held_press_serial(s) && surface_root(s->press_surface) == root)
            return s;
    return NULL;
}

bool wayland_display_drag_start(struct wayland_display *c, struct wayland_surface *origin,
                             const struct yasl_private_drag_image *image,
                             const struct yasl_private_drag_listener *l, void *ud)
{
    if (!c || !origin || origin->conn != c || !l) {
        errno = EINVAL;
        return false;
    }
    if (c->drag.source) {
        errno = EBUSY;
        return false;
    }
    struct seat *seat = drag_seat(c, origin);
    if (!seat) {
        errno = ESTALE;
        return false;
    }
    struct wl_data_device *device = clipboard_data_device(seat->clipboard);
    if (!device || wl_data_device_get_version(device) < 3) {
        errno = ENOTSUP;
        return false;
    }
    c->drag.scale_120 = origin->scale_120;
    if (image && !icon_create(c, image)) {
        errno = ENOMEM;
        return false;
    }
    struct wl_data_source *s = wl_data_device_manager_create_data_source(c->data_manager);
    if (!s) {
        icon_clear(&c->drag.icon);
        errno = ENOMEM;
        return false;
    }
    c->drag.source = s;
    c->drag.seat = seat;
    c->drag.listener = *l;
    c->drag.ud = ud;
    wl_data_source_add_listener(s, &source_listener, c);
    wl_data_source_offer(s, clipboard_drag_mime());
    wl_data_source_set_actions(s, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    if (c->toplevel_drag_mgr)
        c->drag.toplevel = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(
                c->toplevel_drag_mgr, s);
    wl_data_device_start_drag(device, s, seat->press_surface->surface,
                              c->drag.icon.surface, seat_held_press_serial(seat));
    if (image)
        icon_show(c, image);
    wl_display_flush(c->wl);
    return true;
}

void wayland_display_drag_cancel(struct wayland_display *c)
{
    if (c && c->drag.source)
        drag_finish(c, YASL_PRIVATE_DRAG_CANCELLED, false);
}

bool wayland_surface_drag_carry(struct wayland_surface *w, struct yasl_private_vector2i offset)
{
    struct wayland_display *c = w ? w->conn : NULL;
    if (!c || !c->drag.source || !w->toplevel) {
        errno = EINVAL;
        return false;
    }
    if (!c->drag.toplevel) {
        errno = ENOTSUP;
        return false;
    }
    uint32_t scale = c->drag.scale_120;
    xdg_toplevel_drag_v1_attach(c->drag.toplevel, w->toplevel,
                                yasl_private_scale_to_logical(offset.x, scale),
                                yasl_private_scale_to_logical(offset.y, scale));
    c->drag.carrying = true;
    return true;
}

bool wayland_surface_place_under_pointer(struct wayland_surface *w, struct yasl_private_vector2i at)
{
    (void)w;
    (void)at;
    errno = ENOTSUP;
    return false;
}

static struct yasl_private_vector2i drag_device_pos(const struct wayland_surface *w, double x, double y)
{
    double dx = x * w->scale_factor, dy = y * w->scale_factor;
    return (struct yasl_private_vector2i){
        .x = (int)(dx < 0 ? dx - 0.5 : dx + 0.5),
        .y = (int)(dy < 0 ? dy - 0.5 : dy + 0.5),
    };
}

void conn_drop_target(struct wayland_display *c, enum yasl_private_drop_event ev,
                      struct wl_surface *on, double x, double y)
{
    (void)c;
    struct wayland_surface *w = on ?
        (struct wayland_surface *)wl_surface_get_user_data(on) : NULL;
    if (!w || w->parent || w->closed)
        return;
    yasl_private_surface_notify_drop(&w->base, ev, drag_device_pos(w, x, y));
}

void conn_drag_target(struct wayland_display *c, enum yasl_private_drag_event ev,
                      struct wl_surface *on, double x, double y)
{
    (void)c;
    struct wayland_surface *w = on ?
        (struct wayland_surface *)wl_surface_get_user_data(on) : NULL;
    if (!w || w->parent || w->closed)
        return;
    yasl_private_surface_notify_drag(&w->base, ev, drag_device_pos(w, x, y));
}
