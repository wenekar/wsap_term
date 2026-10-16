#include "internal.h"
#include "common/utf8.h"
#include <errno.h>
#include <math.h>
#include <sys/resource.h>

/* Largest device side a window may use, and the largest pixel count whose
 * 4-byte rows still fit an INT32_MAX buffer. */
#define DEVICE_SIDE_MAX 32767
#define DEVICE_AREA_MAX (INT32_MAX / 4)

static long minor_faults(void)
{
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_minflt : 0;
}

void surface_update_opaque_region(struct wayland_surface *w)
{
    if (!w || !w->opaque || w->logical_width <= 0 || w->logical_height <= 0)
        return;
    struct wl_region *region = wl_compositor_create_region(w->conn->compositor);
    if (!region) return;
    wl_region_add(region, 0, 0, w->logical_width, w->logical_height);
    wl_surface_set_opaque_region(w->surface, region);
    wl_region_destroy(region);
}

static const struct wl_buffer_listener buffer_listener;

int device_px(int logical, uint32_t scale_120)
{
    int64_t px = ((int64_t)logical * scale_120 + YASL_PRIVATE_SCALE_UNIT / 2) / YASL_PRIVATE_SCALE_UNIT;
    return px > 0 && px <= INT_MAX ? (int)px : -1;
}

/* Unclamped device pixels of a logical extent; zero when it is not positive. */
static int64_t device_extent(int logical, uint32_t scale_120)
{
    if (logical <= 0 || !scale_120) return 0;
    return ((int64_t)logical * scale_120 + YASL_PRIVATE_SCALE_UNIT / 2) / YASL_PRIVATE_SCALE_UNIT;
}

/* Shrinks a device size to fit the limits, keeping its aspect ratio. */
static void device_size_fit(int64_t *width, int64_t *height)
{
    double w = (double)*width, h = (double)*height;
    double k = 1.0;
    if (*width > DEVICE_SIDE_MAX) k = DEVICE_SIDE_MAX / w;
    if (*height > DEVICE_SIDE_MAX) k = fmin(k, DEVICE_SIDE_MAX / h);
    if (w * h > DEVICE_AREA_MAX) k = fmin(k, sqrt(DEVICE_AREA_MAX / (w * h)));
    if (k >= 1.0) return;
    *width = (int64_t)floor(w * k);
    *height = (int64_t)floor(h * k);
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}

bool buffer_size(int width, int height, size_t *bytes)
{
    if (width <= 0 || height <= 0 || width > INT32_MAX / 4 ||
        (size_t)height > INT32_MAX / ((size_t)width * 4))
        return false;
    *bytes = (size_t)width * 4 * (size_t)height;
    return true;
}

void buffer_destroy(struct buffer *b)
{
    if (b->proxy) wl_buffer_destroy(b->proxy);
    if (b->pool) wl_shm_pool_destroy(b->pool);
    if (b->pixels) munmap(b->pixels, b->capacity);
    *b = (struct buffer){0};
}

static bool fd_truncate(int fd, size_t bytes)
{
    int rc;
    do { rc = ftruncate(fd, (off_t)bytes); } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

int buffer_reserve(struct wayland_surface *w, struct buffer *b, size_t needed)
{
    if (needed <= b->capacity) return 0;
    if (b->busy || needed > INT32_MAX) return -1;
    size_t capacity = b->capacity <= INT32_MAX / 2 ? b->capacity * 2 : INT32_MAX;
    if (capacity < needed) capacity = needed;
    int fd = memfd_create("yasl_private_surface_buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (!fd_truncate(fd, capacity) ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
        close(fd);
        return -1;
    }
    uint32_t *pixels = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return -1;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(w->conn->shm, fd, (int32_t)capacity);
    close(fd);
    if (!pool) {
        munmap(pixels, capacity);
        return -1;
    }
    buffer_destroy(b);
    b->pool = pool;
    b->pixels = pixels;
    b->capacity = capacity;
    return 0;
}

int buffer_prepare(struct wayland_surface *w, struct buffer *b, int width, int height,
                   const struct wl_buffer_listener *listener)
{
    if (b->proxy && !b->busy && b->width == width && b->height == height) return 0;
    long faults_before = minor_faults();
    uint64_t t0 = yasl_private_loop_now_ns();
    size_t bytes;
    if (!buffer_size(width, height, &bytes) || buffer_reserve(w, b, bytes) < 0)
        return -1;
    b->owner = w;
    struct wl_buffer *proxy = wl_shm_pool_create_buffer(b->pool, 0,
        width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    if (!proxy) return -1;
    if (b->proxy) wl_buffer_destroy(b->proxy);
    b->proxy = proxy;
    b->width = width;
    b->height = height;
    b->busy = false;
    b->presented = false;
    wl_buffer_add_listener(proxy, listener, b);
    yasl_private_clock_note_resize(&w->clock, yasl_private_loop_now_ns() - t0);
    long faults = minor_faults() - faults_before;
    if (faults > 0) w->clock.resize_minflt += faults;
    return 0;
}

static void publish_surface_scale(struct wayland_surface *w)
{
    if (w->viewport) {
        wp_viewport_set_destination(w->viewport, w->logical_width, w->logical_height);
    } else if (w->scale_120 % YASL_PRIVATE_SCALE_UNIT == 0 &&
               wl_proxy_get_version((struct wl_proxy *)w->surface) >=
               WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
        wl_surface_set_buffer_scale(w->surface, (int32_t)(w->scale_120 / YASL_PRIVATE_SCALE_UNIT));
    }
}

static void surface_request_scale(struct wayland_surface *w, uint32_t scale_120)
{
    if (!scale_120 || scale_120 == w->pending_scale_120) return;
    w->pending_scale_120 = scale_120;
    w->size_pending = true;
    wayland_surface_schedule_redraw(w);
}

int apply_pending_geometry(struct wayland_surface *w)
{
    if (!w->size_pending) return 0;
    if (w->parent && w->pending_scale_120 == w->scale_120) {
        w->size_pending = false;
        return 0;
    }
    bool floating = !w->parent && !(w->base.state &
            (YASL_PRIVATE_STATE_MAXIMIZED | YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_TILED));
    int free_w = floating && w->floating_width > 0 ? w->floating_width : w->logical_width;
    int free_h = floating && w->floating_height > 0 ? w->floating_height : w->logical_height;
    /* Compositor-suggested sizes are raised to the minimum; application sizes are used as given. */
    bool suggested_w = !w->parent && w->req_width > 0;
    bool suggested_h = !w->parent && w->req_height > 0;
    int logical_w = suggested_w ? w->req_width : free_w;
    int logical_h = suggested_h ? w->req_height : free_h;
    if (suggested_w && logical_w < YASL_PRIVATE_MIN_LOGICAL_PX) logical_w = YASL_PRIVATE_MIN_LOGICAL_PX;
    if (suggested_h && logical_h < YASL_PRIVATE_MIN_LOGICAL_PX) logical_h = YASL_PRIVATE_MIN_LOGICAL_PX;
    if (!(w->base.state & YASL_PRIVATE_STATE_FULLSCREEN)) {
        if (w->bound_width > 0 && logical_w > w->bound_width)
            logical_w = w->bound_width;
        if (w->bound_height > 0 && logical_h > w->bound_height)
            logical_h = w->bound_height;
    }
    /* An oversized window is shrunk to what a buffer holds rather than closed. */
    int64_t device_w = device_extent(logical_w, w->pending_scale_120);
    int64_t device_h = device_extent(logical_h, w->pending_scale_120);
    if (device_w <= 0 || device_h <= 0)
        return -1;
    if (device_w > DEVICE_SIDE_MAX || device_h > DEVICE_SIDE_MAX ||
        (double)device_w * (double)device_h > DEVICE_AREA_MAX) {
        device_size_fit(&device_w, &device_h);
        logical_w = yasl_private_scale_to_logical((int)device_w, w->pending_scale_120);
        logical_h = yasl_private_scale_to_logical((int)device_h, w->pending_scale_120);
    }
    int width = (int)device_w;
    int height = (int)device_h;
    size_t bytes;
    if (w->base.presenter == YASL_PRIVATE_PRESENTER_CPU && !buffer_size(width, height, &bytes))
        return -1;
    bool rescaled = w->scale_120 != w->pending_scale_120;
    bool resized = width != w->width || height != w->height;
    w->size_pending = false;
    if (floating) {
        w->floating_width = logical_w;
        w->floating_height = logical_h;
    }
    if (!rescaled && !resized && logical_w == w->logical_width && logical_h == w->logical_height)
        return 0;

    w->scale_120 = w->pending_scale_120;
    w->scale_factor = (double)w->scale_120 / YASL_PRIVATE_SCALE_UNIT;
    w->width = width;
    w->height = height;
    w->base.size = (struct yasl_private_vector2i){width, height};
    w->base.scale_120 = w->scale_120;
    w->logical_width = logical_w;
    w->logical_height = logical_h;
#if YASL_HAS_EGL
    if (w->egl_window && (resized || rescaled))
        wl_egl_window_resize(w->egl_window, width, height, 0, 0);
#endif
    surface_update_opaque_region(w);
    if (resized || rescaled) {
        for (int i = 0; i < NBUF; i++)
            w->buffers[i].presented = false;
    }
    publish_surface_scale(w);
    if (resized || rescaled) {
        yasl_private_surface_notify_configure(&w->base, (struct yasl_private_geometry){
            .size = { .x = width, .y = height }, .scale_120 = w->scale_120,
        });
        if (w->closed || w->destroyed)
            return -1;
    }
    return 0;
}

static void fractional_preferred_scale(void *data,
        struct wp_fractional_scale_v1 *frac_scale, uint32_t scale)
{
    (void)frac_scale;
    surface_request_scale(data, scale);
}

static const struct wp_fractional_scale_v1_listener frac_scale_listener = {
    .preferred_scale = fractional_preferred_scale,
};

void surface_follow_outputs(struct wayland_surface *w)
{
    if (!w->surface) return; /* Not rebuilt yet after a reconnect. */
    uint32_t refresh = 0;
    int32_t max_scale = 0;
    bool need_scale = !w->frac_scale &&
        wl_proxy_get_version((struct wl_proxy *)w->surface) <
        WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION;

    for (const struct wl_conn_output *o = w->conn->outputs; o; o = o->next) {
        if (o->mask & w->outputs) {
            if (o->refresh_ns && (!refresh || o->refresh_ns < refresh))
                refresh = o->refresh_ns;
            if (need_scale && o->scale > max_scale)
                max_scale = o->scale;
        }
    }
    if (!refresh && w->conn->outputs)
        refresh = w->conn->outputs->refresh_ns;
    if (refresh)
        w->clock.refresh_ns = refresh;

    if (need_scale)
        surface_request_scale(w, (uint32_t)max_scale * YASL_PRIVATE_SCALE_UNIT);
}

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)surface;
    struct wayland_surface *w = data;
    w->outputs |= conn_output_mask(w->conn, output);
    surface_follow_outputs(w);
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)surface;
    struct wayland_surface *w = data;
    w->outputs &= ~conn_output_mask(w->conn, output);
    surface_follow_outputs(w);
}

static void surface_preferred_scale(void *data, struct wl_surface *surface, int32_t factor)
{
    (void)surface;
    struct wayland_surface *w = data;
    if (w->frac_scale || w->parent || factor <= 0)
        return;
    surface_request_scale(w, (uint32_t)factor * YASL_PRIVATE_SCALE_UNIT);
}

static void surface_preferred_transform(void *data, struct wl_surface *surface,
        uint32_t transform)
{
    (void)data; (void)surface; (void)transform;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_scale,
    .preferred_buffer_transform = surface_preferred_transform,
};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    struct buffer *b = data;
    b->busy = false;
    if (b->owner && b->owner->want_draw)
        wayland_surface_schedule_redraw(b->owner);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t ms)
{
    struct wayland_surface *w = data;
    wl_callback_destroy(cb);
    w->frame_cb = NULL;

    uint64_t now = yasl_private_loop_now_ns();
    if (!w->conn->pres_clock_known) {
        yasl_private_clock_presented(&w->clock, yasl_private_clock_extend_ms(ms, now), w->sample_refresh);
        w->clock.source = YASL_PRIVATE_TIMING_FRAME_CALLBACK;
    }

    yasl_private_surface_notify_frame_shown(&w->base);
    if (w->base.presenter != YASL_PRIVATE_PRESENTER_CPU)
        yasl_private_surface_notify_refresh(&w->base);
    else if (w->want_draw)
        wayland_surface_schedule_redraw(w);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

void wayland_surface_arm_frame(struct wayland_surface *w)
{
    if (!w->configured || w->closed || w->frame_cb)
        return;
    w->frame_cb = wl_surface_frame(w->surface);
    if (!w->frame_cb) { surface_fail(w); return; }
    w->sample_refresh = w->clock.last_present_ns &&
        yasl_private_loop_now_ns() - w->clock.last_present_ns <
            (uint64_t)(w->clock.refresh_ns / 2);
    if (wl_callback_add_listener(w->frame_cb, &frame_listener, w) < 0) {
        wl_callback_destroy(w->frame_cb);
        w->frame_cb = NULL;
        surface_fail(w);
    }
}

static const int8_t free_buf_lut[4][8] = {
    [0] = { 1, 1, 2, 2, 1, 1, 0, -1 },
    [1] = { 0, 2, 0, 2, 0, 1, 0, -1 },
    [2] = { 0, 1, 0, 2, 0, 1, 0, -1 },
    [3] = { 0, 1, 0, 2, 0, 1, 0, -1 },
};

int buffer_pick_free_index(const struct buffer *bufs, int cur)
{
    unsigned busy = ((unsigned)bufs[0].busy) |
                    (((unsigned)bufs[1].busy) << 1) |
                    (((unsigned)bufs[2].busy) << 2);
    return free_buf_lut[(unsigned)cur & 3][busy];
}

static void feedback_destroy(struct latency_feedback *feedback)
{
    struct latency_feedback **link = &feedback->window->latency_feedbacks;
    while (*link && *link != feedback) link = &(*link)->next;
    if (*link) *link = feedback->next;
    wp_presentation_feedback_destroy(feedback->proxy);
    if (feedback->in_pool)
        *feedback = (struct latency_feedback){0};
    else
        free(feedback);
}

void surface_feedback_destroy(struct wayland_surface *w)
{
    while (w->latency_feedbacks) feedback_destroy(w->latency_feedbacks);
}

static void sync_output(void *data, struct wp_presentation_feedback *fb,
                        struct wl_output *output)
{
    (void)data; (void)fb; (void)output;
}

static void presented(void *data, struct wp_presentation_feedback *fb,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
        uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    (void)seq_hi; (void)seq_lo; (void)flags;
    struct latency_feedback *feedback = data;
    struct wayland_surface *w = feedback->window;
    if (w->conn->pres_clock_known) {
        uint64_t ns = conn_pres_to_mono(w->conn,
            (((uint64_t)tv_sec_hi << 32) | tv_sec_lo) * 1000000000ull + tv_nsec);
        if (refresh) w->clock.refresh_ns = refresh;
        w->clock.last_present_ns = ns;
        w->clock.source = YASL_PRIVATE_TIMING_PRESENTATION;
    }
    (void)fb;
    feedback_destroy(feedback);
}

static void discarded(void *data, struct wp_presentation_feedback *fb)
{
    struct latency_feedback *feedback = data;
    feedback->window->frames_skipped++;
    (void)fb;
    feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = sync_output,
    .presented = presented,
    .discarded = discarded,
};

void surface_request_presentation_feedback(struct wayland_surface *w)
{
    if (!w || !w->conn->presentation)
        return;
    struct wp_presentation_feedback *fb =
        wp_presentation_feedback(w->conn->presentation, w->surface);
    if (!fb)
        return;
    struct latency_feedback *feedback = NULL;
    for (int i = 0; i < 8; i++) {
        if (!w->latency_pool[i].window) {
            feedback = &w->latency_pool[i];
            feedback->in_pool = true;
            break;
        }
    }
    if (!feedback) {
        feedback = calloc(1, sizeof(*feedback));
        if (!feedback) {
            wp_presentation_feedback_destroy(fb);
            return;
        }
    }
    feedback->window = w;
    feedback->proxy = fb;
    feedback->next = w->latency_feedbacks;
    w->latency_feedbacks = feedback;
    wp_presentation_feedback_add_listener(fb, &feedback_listener, feedback);
}

/* A swap on a rendering thread only sets frame_deferred. The owner arms the frame
 * callback and feedback here, then commits so that the callback can fire without
 * waiting for the application's next swap. */
void surface_arm_deferred(struct wayland_surface *w)
{
    if (!atomic_exchange(&w->frame_deferred, false) || w->closed)
        return;
    bool armed = !w->frame_cb;
    wayland_surface_arm_frame(w);
    surface_request_presentation_feedback(w);
    if (armed && w->frame_cb && !w->closed)
        wl_surface_commit(w->surface);
}

static void commit_buffer(struct wayland_surface *w, int idx)
{
    wl_surface_attach(w->surface, w->buffers[idx].proxy, 0, 0);

    const struct yasl_private_damage_list *d = &w->damage;
    if (d->n < 0 || !w->buffers[idx].presented)
        wl_surface_damage_buffer(w->surface, 0, 0, w->width, w->height);
    else
        wl_damage_list(w->surface, d);

    if (!w->parent) {
        surface_request_presentation_feedback(w);
        if (w->conn->fifo_mgr && !w->fifo)
            w->fifo = wp_fifo_manager_v1_get_fifo(w->conn->fifo_mgr, w->surface);
        if (w->conn->commit_timing_mgr && !w->commit_timer)
            w->commit_timer = wp_commit_timing_manager_v1_get_timer(
                    w->conn->commit_timing_mgr, w->surface);
    }
    if (w->fifo) {
        wp_fifo_v1_wait_barrier(w->fifo);
        wp_fifo_v1_set_barrier(w->fifo);
    }
    if (w->commit_timer && w->conn->pres_clock_known && w->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        uint64_t now = yasl_private_loop_now_ns();
        uint64_t next = yasl_private_clock_next_present_ns(&w->clock);
        bool soon = next > now && next - now < (uint64_t)(w->clock.refresh_ns * 1.5);
        next = conn_mono_to_pres(w->conn, next);
        if (soon)
            wp_commit_timer_v1_set_timestamp(w->commit_timer,
                (uint32_t)(next / 1000000000ull >> 32),
                (uint32_t)(next / 1000000000ull),
                (uint32_t)(next % 1000000000ull));
    }

    if (w->has_pending_serial) {
        xdg_surface_ack_configure(w->xdg_surface, w->pending_serial);
        w->has_pending_serial = false;
    }
    wl_surface_commit(w->surface);
    w->buffers[idx].busy = true;
    w->buffers[idx].presented = true;
}

bool wayland_surface_present(struct wayland_surface *w, const struct yasl_private_framebuffer *fb,
                          const struct yasl_private_damage_list *damage)
{
    if (!w || w->base.presenter != YASL_PRIVATE_PRESENTER_CPU ||
        w->closed || !w->frame_active || w->frame_presented || !fb ||
        !damage || fb->pixels != w->frame.target.pixels ||
        fb->size.x != w->width || fb->size.y != w->height)
        return false;

    w->damage.n = damage->n;
    w->damage.bounds = damage->bounds;
    if (damage->n > 0)
        memcpy(w->damage.rects, damage->rects, (size_t)damage->n * sizeof(damage->rects[0]));
    wayland_surface_arm_frame(w);
    if (w->closed)
        return false;
    commit_buffer(w, w->cur);
    w->frame_presented = true;
    w->shown = w->cur;
    w->has_shown = true;
    return true;
}

bool wayland_surface_read_pixels(struct wayland_surface *w, uint32_t *out, size_t cap,
                                 struct yasl_private_vector2i *size)
{
    if (!w || w->closed || w->frame_active || !w->has_shown) return false;
    const struct buffer *b = &w->buffers[w->shown];
    if (!b->pixels) return false;
    return yasl_private_copy_frame(b->pixels, (struct yasl_private_vector2i){b->width, b->height}, out, cap, size);
}

bool surface_draw_ready(const struct wayland_surface *w)
{
    return w->want_draw && w->configured && !w->closed &&
           w->base.presenter == YASL_PRIVATE_PRESENTER_CPU &&
           buffer_pick_free_index(w->buffers, w->cur) >= 0;
}

void surface_draw_frame(struct wayland_surface *w)
{
    if (w->closed || !w->configured || w->base.presenter != YASL_PRIVATE_PRESENTER_CPU)
        return;
    if (apply_pending_geometry(w) < 0) {
        surface_fail(w);
        return;
    }
    if (w->closed || !w->cbs->draw || !w->want_draw)
        return;

    int idx = buffer_pick_free_index(w->buffers, w->cur);
    if (idx < 0) {
        w->frames_skipped++;
        return;
    }
    /* Drawing reuses the presented buffer only after the compositor released it. */
    if (w->has_shown && idx == w->shown)
        w->has_shown = false;
    if (buffer_prepare(w, &w->buffers[idx], w->width, w->height, &buffer_listener) < 0) {
        w->frames_skipped++;
        return;
    }
    w->cur = idx;
    w->want_draw = false;

    if (w->parent)
        memset(w->buffers[idx].pixels, 0, (size_t)w->width * (size_t)w->height * sizeof(uint32_t));
    w->damage = (struct yasl_private_damage_list){0};
    w->frame = (struct yasl_private_frame){
        .surface = &w->base,
        .target = {
            .generation = yasl_private_frame_begin(&w->base),
            .pixels = w->buffers[idx].pixels,
            .size = { .x = w->width, .y = w->height },
        },
    };
    w->frame_active = true;
    w->frame_presented = false;
    yasl_private_surface_draw(&w->base, &w->frame);
    if (w->closed || w->destroyed)
        return;
    w->frame_active = false;
    if (!w->frame_presented)
        w->frames_skipped++;
}

int surface_init_scaling(struct wayland_surface *w)
{
    wl_surface_add_listener(w->surface, &surface_listener, w);
    if (w->conn->viewporter) {
        w->viewport = wp_viewporter_get_viewport(w->conn->viewporter, w->surface);
        if (!w->viewport) return -1;
    }
    if (w->conn->scale_mgr && w->viewport && !w->parent) {
        w->frac_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
                w->conn->scale_mgr, w->surface);
        if (!w->frac_scale) return -1;
        wp_fractional_scale_v1_add_listener(w->frac_scale, &frac_scale_listener, w);
    }
    return 0;
}

void wayland_surface_set_title(struct wayland_surface *w, const char *title)
{
    if (!w || !title) return;
    char *copy = yasl_private_utf8_sanitize_dup(title);
    if (!copy) return;
    free(w->title);
    w->title = copy;
    if (w->toplevel) xdg_toplevel_set_title(w->toplevel, copy);
}

struct yasl_private_timing wayland_surface_timing(const struct wayland_surface *w)
{
    struct yasl_private_timing t = {
        .refresh_ns = w->clock.refresh_ns,
        .last_present_ns = w->clock.last_present_ns,
        .next_present_ns = yasl_private_clock_next_present_ns(&w->clock),
        .frames_skipped = w->frames_skipped,
        .source = w->clock.source,
    };
    yasl_private_clock_resize_stats(&w->clock, &t.resize);
    return t;
}
