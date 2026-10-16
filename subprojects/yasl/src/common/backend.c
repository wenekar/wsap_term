#include "backend.h"
#include "loop.h"
#include "common/utf8.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const struct yasl_private_backend_ops *backend_ops(enum yasl_private_backend backend)
{
    switch (backend) {
#if YASL_HAS_WAYLAND
    case YASL_PRIVATE_BACKEND_WAYLAND: return &yasl_private_wayland_backend;
#endif
#if YASL_HAS_X11
    case YASL_PRIVATE_BACKEND_X11: return &yasl_private_x11_backend;
#endif
#if YASL_HAS_COCOA
    case YASL_PRIVATE_BACKEND_COCOA: return &yasl_private_cocoa_backend;
#endif
    default: return NULL;
    }
}

static void resolve_frame_requests(struct yasl_private_display *d);

/* The display drives the loop it runs on; see struct yasl_private_loop_driver. */
static bool driver_prepare(void *ud)
{
    struct yasl_private_display *d = ud;
    bool ready = d->ops->display_prepare(d);
    resolve_frame_requests(d);
    /* Readiness the backends cannot see: events already queued, an error to
     * report, or an owed REFRESH. */
    return ready || yasl_private_events_pending(d) || yasl_private_events_failed(d) ||
           (d->pending && d->pending(d));
}

static int driver_wait(void *ud, int timeout_ms)
{
    struct yasl_private_display *d = ud;
    return d->ops->display_wait(d, timeout_ms);
}

static void driver_check(void *ud)
{
    struct yasl_private_display *d = ud;
    d->ops->display_check(d);
}

static void driver_deliver(void *ud)
{
    struct yasl_private_display *d = ud;
    if (d->deliver)
        d->deliver(d);
}

static struct yasl_private_display *open_backend(enum yasl_private_backend backend,
        struct yasl_loop *loop, const struct yasl_private_display_spec *spec)
{
    const struct yasl_private_backend_ops *ops = backend_ops(backend);
    if (!ops) {
        errno = ENOSYS;
        return NULL;
    }
    errno = 0;
    struct yasl_private_display *d = ops->display_open(loop, spec);
    if (!d && !errno) errno = ENODEV;
    if (!d)
        return NULL;
    struct yasl_private_loop_driver driver = {
        .prepare = driver_prepare,
        .wait = ops->display_wait ? driver_wait : NULL,
        .check = driver_check,
        .deliver = driver_deliver,
        .ud = d,
    };
    if (!yasl_private_events_open(d) || !yasl_private_loop_set_driver(d->loop, &driver)) {
        int error = errno;
        yasl_private_events_close(d);
        ops->display_close(d);
        errno = error;
        return NULL;
    }
    return d;
}

struct yasl_private_display *yasl_private_display_open(struct yasl_loop *loop,
        const struct yasl_private_display_spec *spec)
{
    enum yasl_private_backend backend = spec ? spec->backend : YASL_PRIVATE_BACKEND_AUTO;
    if (backend < YASL_PRIVATE_BACKEND_AUTO || backend > YASL_PRIVATE_BACKEND_COCOA) {
        errno = EINVAL;
        return NULL;
    }
    if (backend != YASL_PRIVATE_BACKEND_AUTO)
        return open_backend(backend, loop, spec);
#if YASL_HAS_COCOA
    return open_backend(YASL_PRIVATE_BACKEND_COCOA, loop, spec);
#else
    const char *session = getenv("XDG_SESSION_TYPE");
    bool wayland = getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET") ||
                   (session && strcmp(session, "wayland") == 0);
    enum yasl_private_backend first = wayland ? YASL_PRIVATE_BACKEND_WAYLAND : YASL_PRIVATE_BACKEND_X11;
    enum yasl_private_backend second = wayland ? YASL_PRIVATE_BACKEND_X11 : YASL_PRIVATE_BACKEND_WAYLAND;
    struct yasl_private_display *d = NULL;
    if (backend_ops(first)) d = open_backend(first, loop, spec);
    if (!d && backend_ops(second)) d = open_backend(second, loop, spec);
    return d;
#endif
}

enum yasl_private_backend yasl_private_display_backend(const struct yasl_private_display *d)
{
    return d ? d->ops->type : YASL_PRIVATE_BACKEND_AUTO;
}

void yasl_private_display_close(struct yasl_private_display *d)
{
    if (!d || !d->ops->display_close)
        return;
    yasl_private_loop_set_driver(d->loop, NULL);
    yasl_private_events_close(d);
    d->ops->display_close(d);
}

uint32_t yasl_private_display_caps(const struct yasl_private_display *d)
{
    if (!d || !d->ops->display_caps)
        return 0;
    return d->ops->display_caps(d);
}

void yasl_private_display_flush(struct yasl_private_display *d)
{
    if (!d || !d->ops->display_flush)
        return;
    d->ops->display_flush(d);
}

int yasl_private_display_modifiers(const struct yasl_private_display *d)
{
    if (!d || !d->ops->display_modifiers)
        return 0;
    return d->ops->display_modifiers(d);
}

int yasl_private_display_double_click_ms(const struct yasl_private_display *d)
{
    int ms = d && d->ops->display_double_click_ms ? d->ops->display_double_click_ms(d) : 0;
    /* Reject nonsense from misconfigured settings as well as no answer. */
    return ms > 0 && ms <= 10000 ? ms : YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT;
}

bool yasl_private_display_drag_start(struct yasl_private_display *d, struct yasl_surface *origin, const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *l, void *ud)
{
    if (!d || !origin || origin->display != d) { errno = EINVAL; return false; }
    if (!d->ops->display_drag_start) { errno = ENOTSUP; return false; }
    if (image && (!image->pixels || image->size.x <= 0 || image->size.y <= 0 ||
        image->size.x > 32767 || image->size.y > 32767 ||
        (int64_t)image->size.x * image->size.y * 4 > INT32_MAX ||
        image->hotspot.x < 0 || image->hotspot.y < 0 ||
        image->hotspot.x >= image->size.x || image->hotspot.y >= image->size.y)) {
        errno = EINVAL;
        return false;
    }
    const struct yasl_private_drag_listener empty = {0};
    errno = 0;
    bool ok = d->ops->display_drag_start(d, origin, image, l ? l : &empty, ud);
    if (!ok && !errno) errno = EIO;
    return ok;
}

void yasl_private_display_drag_cancel(struct yasl_private_display *d)
{
    if (!d || !d->ops->display_drag_cancel)
        return;
    d->ops->display_drag_cancel(d);
}

struct yasl_surface *yasl_private_create_toplevel(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device)
{
    if (!d || !d->ops->surface_create_toplevel || !spec ||
        spec->size.x <= 0 || spec->size.y <= 0 ||
        spec->size.x > 32767 || spec->size.y > 32767 ||
        (int64_t)spec->size.x * spec->size.y * 4 > INT32_MAX) {
        errno = EINVAL;
        return NULL;
    }
    if (graphics_device && !(yasl_private_display_caps(d) & YASL_PRIVATE_CAP_NATIVE_GRAPHICS)) {
        errno = ENOTSUP;
        return NULL;
    }
    errno = 0;
    struct yasl_surface *s = d->ops->surface_create_toplevel(d, spec, graphics_device);
    if (!s && !errno) errno = ENODEV;
    if (s && !yasl_private_events_register(s)) {
        int error = errno;
        d->ops->surface_destroy(s);
        errno = error;
        return NULL;
    }
    if (s) {
        s->userdata = spec->userdata;
        /* Native creation may dispatch events. Attach this window's callbacks
         * only after it finishes, so callers can store the returned surface. */
        yasl_private_surface_set_callbacks(s, spec->cbs, spec->ud);
    }
    return s;
}

struct yasl_surface *yasl_private_surface_create_toplevel(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec)
{
    return yasl_private_create_toplevel(d, spec, NULL);
}

static bool popup_spec_valid(const struct yasl_private_popup_spec *spec)
{
    return spec && spec->size.x > 0 && spec->size.y > 0 &&
        spec->size.x <= 32767 && spec->size.y <= 32767 &&
        (int64_t)spec->size.x * spec->size.y * 4 <= INT32_MAX &&
        spec->anchor.width >= 0 && spec->anchor.height >= 0 &&
        spec->content_offset.x >= 0 && spec->content_offset.y >= 0 &&
        spec->content_size.x >= 0 && spec->content_size.y >= 0 &&
        (int64_t)spec->content_offset.x + spec->content_size.x <= spec->size.x &&
        (int64_t)spec->content_offset.y + spec->content_size.y <= spec->size.y;
}

struct yasl_surface *yasl_private_surface_create_popup(struct yasl_surface *parent, const struct yasl_private_popup_spec *spec)
{
    if (!parent || !popup_spec_valid(spec)) {
        errno = EINVAL;
        return NULL;
    }
    if (!(yasl_private_display_caps(parent->display) & YASL_PRIVATE_CAP_POPUPS) ||
        !parent->display->ops->surface_create_popup) {
        errno = ENOTSUP;
        return NULL;
    }
    errno = 0;
    struct yasl_surface *s = parent->display->ops->surface_create_popup(parent, spec);
    if (!s && !errno) errno = ENODEV;
    if (s) {
        s->popup = true;
        if (!yasl_private_events_register(s)) {
            int error = errno;
            parent->display->ops->surface_destroy(s);
            errno = error;
            return NULL;
        }
    }
    return s;
}

bool yasl_private_surface_popup_reposition(struct yasl_surface *popup, const struct yasl_private_popup_spec *spec)
{
    if (!popup || !popup_spec_valid(spec)) {
        errno = EINVAL;
        return false;
    }
    if (!(yasl_private_display_caps(popup->display) & YASL_PRIVATE_CAP_POPUPS) ||
        !popup->display->ops->surface_popup_reposition) {
        errno = ENOTSUP;
        return false;
    }
    errno = 0;
    bool result = popup->display->ops->surface_popup_reposition(popup, spec);
    if (!result && !errno) errno = ENODEV;
    return result;
}

void yasl_private_surface_destroy(struct yasl_surface *s)
{
    if (!s || !s->display->ops->surface_destroy)
        return;
    yasl_private_events_unregister(s);
    yasl_private_surface_clear_timers(s);
    input_queue_clear(&s->input);
    s->display->ops->surface_destroy(s);
}

struct yasl_private_display *yasl_private_surface_display(const struct yasl_surface *s)
{
    return s ? s->display : NULL;
}

bool yasl_private_surface_set_callbacks(struct yasl_surface *s, const struct yasl_private_window_cbs *cbs, void *ud)
{
    if (!s) { errno = EINVAL; return false; }
    s->callbacks = cbs ? *cbs : (struct yasl_private_window_cbs){0};
    s->ud = ud;
    if (s->display && s->display->ops && s->display->ops->surface_set_callbacks)
        s->display->ops->surface_set_callbacks(s, cbs, ud);
    return true;
}

void yasl_private_surface_set_title(struct yasl_surface *s, const char *title)
{
    if (!s || !s->display->ops->surface_set_title)
        return;
    s->display->ops->surface_set_title(s, title);
}

void yasl_private_surface_schedule_redraw(struct yasl_surface *s)
{
    if (!s || !s->display->ops->surface_schedule_redraw)
        return;
    s->display->ops->surface_schedule_redraw(s);
}

bool yasl_private_surface_present(struct yasl_surface *s, const struct yasl_private_framebuffer *fb, const struct yasl_private_damage_list *damage)
{
    if (!s || !s->display->ops->surface_present || !fb || !damage ||
        !fb->generation || fb->generation != s->frame_generation ||
        fb->size.x <= 0 || fb->size.y <= 0 ||
        damage->n < -1 || damage->n > YASL_PRIVATE_DAMAGE_MAX)
        return false;
    struct yasl_private_damage_list clipped;
    clipped.n = damage->n;
    if (damage->n < 0) {
        clipped.bounds = damage->bounds;
    } else {
        clipped.n = 0;
        clipped.bounds = (struct yasl_private_rect2i){0};
        for (int i = 0; i < damage->n; i++) {
            struct yasl_private_rect2i r = damage->rects[i];
            if (r.width < 0 || r.height < 0) return false;
            int64_t right = (int64_t)r.x + r.width;
            int64_t bottom = (int64_t)r.y + r.height;
            int64_t x = r.x > 0 ? r.x : 0, y = r.y > 0 ? r.y : 0;
            if (right > fb->size.x) right = fb->size.x;
            if (bottom > fb->size.y) bottom = fb->size.y;
            if (right > x && bottom > y)
                clipped.rects[clipped.n++] = (struct yasl_private_rect2i){
                    (int)x, (int)y, (int)(right - x), (int)(bottom - y),
                };
        }
    }
    if (!s->display->ops->surface_present(s, fb, &clipped))
        return false;
    atomic_store(&s->presented_ns, yasl_private_loop_now_ns());
    return true;
}

bool yasl_private_surface_read_pixels(struct yasl_surface *s, uint32_t *out, size_t cap,
                              struct yasl_private_vector2i *size)
{
    if (!size) return false;
    *size = (struct yasl_private_vector2i){0};
    if (!s || (!out && cap) || !s->display->ops->surface_read_pixels) return false;
    return s->display->ops->surface_read_pixels(s, out, cap, size);
}

bool yasl_private_surface_set_pointer_mode(struct yasl_surface *s, enum yasl_private_pointer_mode mode)
{
    if (!s || (mode != YASL_PRIVATE_POINTER_NORMAL && mode != YASL_PRIVATE_POINTER_RELATIVE)) {
        errno = EINVAL;
        return false;
    }
    if (mode == s->pointer_mode)
        return true;
    const struct yasl_private_backend_ops *ops = s->display ? s->display->ops : NULL;
    if (!ops || !ops->surface_set_pointer_mode) {
        if (mode == YASL_PRIVATE_POINTER_NORMAL) return true;
        errno = ENOTSUP;
        return false;
    }
    if (!ops->surface_set_pointer_mode(s, mode))
        return false;
    s->pointer_mode = mode;
    return true;
}

struct yasl_private_geometry yasl_private_surface_geometry(const struct yasl_surface *s)
{
    if (!s)
        return (struct yasl_private_geometry){.scale_120 = YASL_PRIVATE_SCALE_UNIT};
    if (s->scale_120)
        return (struct yasl_private_geometry){s->size, s->scale_120};
    if (s->display && s->display->ops && s->display->ops->surface_geometry)
        return s->display->ops->surface_geometry(s);
    return (struct yasl_private_geometry){s->size, YASL_PRIVATE_SCALE_UNIT};
}

uint32_t yasl_private_surface_state(const struct yasl_surface *s)
{
    return s ? s->state : 0;
}

struct yasl_private_timing yasl_private_surface_timing(const struct yasl_surface *s)
{
    if (!s)
        return (struct yasl_private_timing){0};
    if (s->display && s->display->ops && s->display->ops->surface_timing)
        return s->display->ops->surface_timing(s);
    struct yasl_private_timing t = {
        .refresh_ns = s->clock.refresh_ns,
        .last_present_ns = s->clock.last_present_ns,
        .next_present_ns = yasl_private_clock_next_present_ns(&s->clock),
        .frames_skipped = s->frames_skipped,
        .source = s->clock.source,
    };
    yasl_private_clock_resize_stats(&s->clock, &t.resize);
    return t;
}

uint64_t yasl_private_surface_frame_time_ns(struct yasl_surface *s, uint64_t now)
{
    if (!s)
        return 0;
    if (s->display && s->display->ops && s->display->ops->surface_frame_time_ns)
        return s->display->ops->surface_frame_time_ns(s, now);
    return yasl_private_clock_frame_timestamp_ns(&s->clock, now);
}

void yasl_private_surface_set_cursor(struct yasl_surface *s, enum yasl_private_cursor cursor)
{
    if (!s || !s->display->ops->surface_set_cursor)
        return;
    s->display->ops->surface_set_cursor(s, cursor);
}

void yasl_private_surface_set_background_color(struct yasl_surface *s, uint32_t bg)
{
    if (!s)
        return;
    s->bg_color = bg;
    if (s->display && s->display->ops->surface_set_background_color)
        s->display->ops->surface_set_background_color(s, bg);
}

const char *yasl_private_surface_ime_preedit(const struct yasl_surface *s)
{
    if (!s || !s->display->ops->surface_ime_preedit)
        return 0;
    return s->display->ops->surface_ime_preedit(s);
}

bool yasl_private_surface_ime_preedit_cursor(const struct yasl_surface *s, int *begin, int *end)
{
    if (!s || !begin || !end || !s->display->ops->surface_ime_preedit_cursor)
        return 0;
    return s->display->ops->surface_ime_preedit_cursor(s, begin, end);
}

void yasl_private_surface_set_ime_caret(struct yasl_surface *s, struct yasl_private_rect2i caret)
{
    if (!s || !s->display->ops->surface_set_ime_caret)
        return;
    s->display->ops->surface_set_ime_caret(s, caret);
}

/* Backend operations that may be missing: ENOTSUP when absent, EIO when a backend
 * fails without saying why. */
#define OPTIONAL_OP(s, op, ...)                                                          \
    do {                                                                                 \
        if (!(s)) { errno = EINVAL; return false; }                                      \
        if (!(s)->display->ops->op) { errno = ENOTSUP; return false; }                   \
        errno = 0;                                                                       \
        bool ok_ = (s)->display->ops->op(__VA_ARGS__);                                   \
        if (!ok_ && !errno) errno = EIO;                                                 \
        return ok_;                                                                      \
    } while (0)

bool yasl_private_surface_start_interactive_move(struct yasl_surface *s)
{
    OPTIONAL_OP(s, surface_start_interactive_move, s);
}

bool yasl_private_surface_start_interactive_resize(struct yasl_surface *s, enum yasl_private_resize_edge edge)
{
    if (edge != YASL_PRIVATE_EDGE_TOP && edge != YASL_PRIVATE_EDGE_BOTTOM &&
        edge != YASL_PRIVATE_EDGE_LEFT && edge != YASL_PRIVATE_EDGE_RIGHT &&
        edge != YASL_PRIVATE_EDGE_TOP_LEFT && edge != YASL_PRIVATE_EDGE_TOP_RIGHT &&
        edge != YASL_PRIVATE_EDGE_BOTTOM_LEFT && edge != YASL_PRIVATE_EDGE_BOTTOM_RIGHT) {
        errno = EINVAL;
        return false;
    }
    OPTIONAL_OP(s, surface_start_interactive_resize, s, edge);
}

bool yasl_private_surface_show_window_menu(struct yasl_surface *s, struct yasl_private_vector2i pos)
{
    OPTIONAL_OP(s, surface_show_window_menu, s, pos);
}

bool yasl_private_surface_drag_carry(struct yasl_surface *s, struct yasl_private_vector2i offset)
{
    OPTIONAL_OP(s, surface_drag_carry, s, offset);
}

bool yasl_private_surface_place_under_pointer(struct yasl_surface *s, struct yasl_private_vector2i at)
{
    OPTIONAL_OP(s, surface_place_under_pointer, s, at);
}

void yasl_private_surface_clear_timers(struct yasl_surface *s)
{
    if (!s || !s->timers) return;
    struct yasl_loop *loop = s->display ? s->display->loop : NULL;
    for (int i = 0; i < s->timer_count; i++) {
        if (loop && s->timers[i]) {
            yasl_loop_del_timer(loop, s->timers[i]);
        }
    }
    free(s->timers);
    s->timers = NULL;
    s->timer_count = 0;
    s->timer_capacity = 0;
}

yasl_private_timer_id yasl_private_surface_add_timer(struct yasl_surface *s, uint64_t interval_ns,
                                     yasl_private_timer_cb cb, void *ud)
{
    if (!s || !s->display || !s->display->loop || !cb || interval_ns == 0)
        return 0;

    yasl_private_timer_id id = yasl_loop_add_timer(s->display->loop, interval_ns, cb, ud);
    if (!id) return 0;

    /* A one-shot timer retires itself in the loop. Drop retired ids before
     * the list grows, so the list holds only armed timers. */
    if (s->timer_count >= s->timer_capacity) {
        int kept = 0;
        for (int i = 0; i < s->timer_count; i++)
            if (yasl_private_loop_timer_armed(s->display->loop, s->timers[i]))
                s->timers[kept++] = s->timers[i];
        s->timer_count = kept;
    }
    if (s->timer_count >= s->timer_capacity) {
        int new_cap = s->timer_capacity ? s->timer_capacity * 2 : 4;
        yasl_private_timer_id *nt = realloc(s->timers, (size_t)new_cap * sizeof(*nt));
        if (!nt) {
            yasl_loop_del_timer(s->display->loop, id);
            return 0;
        }
        s->timers = nt;
        s->timer_capacity = new_cap;
    }
    s->timers[s->timer_count++] = id;
    return id;
}

bool yasl_private_surface_del_timer(struct yasl_surface *s, yasl_private_timer_id id)
{
    if (!s || id == 0) return false;
    for (int i = 0; i < s->timer_count; i++) {
        if (s->timers[i] == id) {
            s->timers[i] = s->timers[--s->timer_count];
            return s->display && yasl_loop_del_timer(s->display->loop, id);
        }
    }
    return false;
}

struct yasl_loop *yasl_private_display_loop(struct yasl_private_display *d)
{
    return d ? d->loop : NULL;
}

bool yasl_private_frame_present(struct yasl_private_frame *frame, const struct yasl_private_rect2i *rects, size_t count)
{
    if (!frame || !frame->surface || (!rects && count)) { errno = EINVAL; return false; }
    struct yasl_surface *s = frame->surface;
    if (!s->drawing || s->active_frame != frame) { errno = EINVAL; return false; }
    if (s->frame_presented) { errno = EALREADY; return false; }
    for (size_t i = 0; i < count; i++) {
        if (rects[i].width < 0 || rects[i].height < 0) { errno = EINVAL; return false; }
    }
    struct yasl_private_damage_list damage = {0};
    if (!rects || count > YASL_PRIVATE_DAMAGE_MAX) damage.n = -1;
    else {
        damage.n = (int)count;
        for (size_t i = 0; i < count; i++) damage.rects[i] = rects[i];
    }
    errno = 0;
    bool ok = yasl_private_surface_present(s, &frame->target, &damage);
    if (ok) s->frame_presented = true;
    if (!ok && !errno) errno = EIO;
    return ok;
}

void yasl_private_surface_request_frame(struct yasl_surface *s)
{
    if (!s) return;
    yasl_private_surface_schedule_redraw(s);
    yasl_private_loop_wake(s->display->loop);
}

static uint64_t refresh_interval_ns(struct yasl_surface *s)
{
    double refresh = yasl_private_surface_timing(s).refresh_ns;
    return refresh >= 1000000.0 && refresh <= 1000000000.0 ? (uint64_t)refresh : 16666667;
}

void yasl_private_surface_notify_frame_shown(struct yasl_surface *s)
{
    if (!s || !s->frame_requested)
        return;
    s->frame_requested = false;
    s->frame_answered_ns = yasl_private_loop_now_ns();
    s->refresh_owed = true;
}

static void resolve_frame_request(struct yasl_surface *s);

static uint64_t frame_interval_done(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct yasl_surface *s = ud;
    s->frame_timer = 0;
    /* A present since the timer was set moves the next frame later. */
    resolve_frame_request(s);
    return 0;
}

/* The frame a request waits for is the next one the application presents,
 * which is often after the request. So the loop decides at the start of each
 * step, after the callbacks that draw have run. */
static void resolve_frame_request(struct yasl_surface *s)
{
    if (!s->frame_requested || yasl_private_loop_timer_armed(s->display->loop, s->frame_timer))
        return;
    /* The system reports when the frame in flight is shown. */
    const struct yasl_private_backend_ops *ops = s->display->ops;
    if (ops->surface_frame_in_flight && ops->surface_frame_in_flight(s))
        return;
    /* Otherwise one frame per refresh interval, counted from the last
     * present or answer, so a request with no present cannot spin. */
    uint64_t last = atomic_load(&s->presented_ns);
    if (s->frame_answered_ns > last)
        last = s->frame_answered_ns;
    uint64_t interval = refresh_interval_ns(s);
    uint64_t since = yasl_private_loop_now_ns() - last;
    if (!last || since >= interval) {
        yasl_private_surface_notify_frame_shown(s);
        return;
    }
    s->frame_timer = yasl_private_surface_add_timer(s, interval - since, frame_interval_done, s);
    /* Without a timer, an early frame is better than none. */
    if (!s->frame_timer)
        yasl_private_surface_notify_frame_shown(s);
}

void yasl_private_surface_request_shown_frame(struct yasl_surface *s)
{
    s->frame_requested = true;
}

static void resolve_frame_requests(struct yasl_private_display *d)
{
    for (struct yasl_surface *s = d->surfaces; s; s = s->event_next)
        resolve_frame_request(s);
}

static uint64_t pacing_tick(void *ud, yasl_private_timer_id id, uint64_t interval)
{
    (void)id; (void)interval;
    struct yasl_surface *s = ud;
    yasl_private_surface_request_frame(s);
    return refresh_interval_ns(s);
}

bool yasl_private_surface_set_pacing(struct yasl_surface *s, yasl_private_pacing_mode mode)
{
    if (!s || (mode != YASL_PRIVATE_PACING_ON_DEMAND && mode != YASL_PRIVATE_PACING_CONTINUOUS)) {
        errno = EINVAL;
        return false;
    }
    if (s->pacing_timer) {
        yasl_private_surface_del_timer(s, s->pacing_timer);
        s->pacing_timer = 0;
    }
    if (mode == YASL_PRIVATE_PACING_CONTINUOUS && !(yasl_private_display_caps(s->display) & YASL_PRIVATE_CAP_FRAME_PACING)) {
        s->pacing_timer = yasl_private_surface_add_timer(s, 16666667, pacing_tick, s);
        if (!s->pacing_timer) {
            s->pacing = YASL_PRIVATE_PACING_ON_DEMAND;
            errno = ENOMEM;
            return false;
        }
    }
    s->pacing = mode;
    if (mode == YASL_PRIVATE_PACING_CONTINUOUS) yasl_private_surface_request_frame(s);
    return true;
}

void yasl_private_surface_draw(struct yasl_surface *s, struct yasl_private_frame *frame)
{
    if (!s || !frame || !s->callbacks.draw) return;
    struct yasl_private_display *d = s->display;
    uint64_t id = s->id;
    frame->surface = s;
    frame->target.stride_bytes = frame->target.size.x * (int)sizeof(uint32_t);
    frame->scale_120 = s->scale_120;
    frame->frame_index = s->draw_index++;
    frame->target_time_ns = yasl_private_surface_frame_time_ns(s, yasl_private_loop_now_ns());
    s->drawing = true;
    s->frame_presented = false;
    s->active_frame = frame;
    s->callbacks.draw(s, frame, s->ud);
    /* The callback may destroy s. Lookup by identity before accessing it. */
    struct yasl_surface *live = id ? yasl_private_events_find(d, id) : NULL;
    if (live) {
        live->drawing = false;
        live->active_frame = NULL;
        if (live->pacing == YASL_PRIVATE_PACING_CONTINUOUS &&
            (yasl_private_display_caps(d) & YASL_PRIVATE_CAP_FRAME_PACING))
            yasl_private_surface_schedule_redraw(live);
    }
}

static bool window_valid(struct yasl_surface *s)
{
    if (!s || s->popup) { errno = EINVAL; return false; }
    return true;
}

bool yasl_private_surface_set_size(struct yasl_surface *s, struct yasl_private_vector2i size)
{
    if (!window_valid(s)) return false;
    if (size.x <= 0 || size.y <= 0 || size.x > 32767 || size.y > 32767 ||
        (int64_t)size.x * size.y * 4 > INT32_MAX) { errno = EINVAL; return false; }
    if (!s->display->ops->surface_set_size) { errno = ENOTSUP; return false; }
    errno = 0;
    bool ok = s->display->ops->surface_set_size(s, size);
    if (!ok && !errno) errno = EIO;
    return ok;
}

bool yasl_private_surface_set_fullscreen(struct yasl_surface *s, bool enabled)
{
    if (!window_valid(s)) return false;
    if (!s->display->ops->surface_set_fullscreen) { errno = ENOTSUP; return false; }
    errno = 0;
    bool ok = s->display->ops->surface_set_fullscreen(s, enabled);
    if (!ok && !errno) errno = EIO;
    return ok;
}

bool yasl_private_surface_set_maximized(struct yasl_surface *s, bool enabled)
{
    if (!window_valid(s)) return false;
    if (!s->display->ops->surface_set_maximized) { errno = ENOTSUP; return false; }
    errno = 0;
    bool ok = s->display->ops->surface_set_maximized(s, enabled);
    if (!ok && !errno) errno = EIO;
    return ok;
}

bool yasl_private_surface_minimize(struct yasl_surface *s)
{
    if (!window_valid(s)) return false;
    if (!s->display->ops->surface_minimize) { errno = ENOTSUP; return false; }
    errno = 0;
    bool ok = s->display->ops->surface_minimize(s);
    if (!ok && !errno) errno = EIO;
    return ok;
}

static bool selection_valid(struct yasl_private_display *d, enum yasl_private_selection selection)
{
    if (!d || (selection != YASL_PRIVATE_CLIPBOARD && selection != YASL_PRIVATE_PRIMARY)) {
        errno = EINVAL; return false;
    }
    if (selection == YASL_PRIVATE_PRIMARY && !(yasl_private_display_caps(d) & YASL_PRIVATE_CAP_PRIMARY_SELECTION)) {
        errno = ENOTSUP; return false;
    }
    return true;
}

bool yasl_private_clipboard_set(struct yasl_private_display *d, enum yasl_private_selection selection, const char *text)
{
    if (!selection_valid(d, selection)) return false;
    if (!text) { errno = EINVAL; return false; }
    if (!yasl_private_utf8_validate(text, strlen(text))) { errno = EILSEQ; return false; }
    if (!d->ops->display_copy_text) { errno = ENOTSUP; return false; }
    errno = 0;
    bool ok = d->ops->display_copy_text(d, selection, text);
    if (!ok && !errno) errno = EIO;
    return ok;
}

uint64_t yasl_private_clipboard_request(struct yasl_surface *s, enum yasl_private_selection selection)
{
    if (!s) { errno = EINVAL; return 0; }
    struct yasl_private_display *d = s->display;
    if (!selection_valid(d, selection)) return 0;
    if (!d->ops->display_request_paste) { errno = ENOTSUP; return 0; }
    uint64_t id = yasl_private_events_request_id(d);
    if (!id) return 0;
    errno = 0;
    if (!d->ops->display_request_paste(d, selection, s, id)) {
        if (!errno) errno = EIO;
        return 0;
    }
    return id;
}
