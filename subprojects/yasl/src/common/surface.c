#define _POSIX_C_SOURCE 200809L
#include "public_internal.h"
#include "common/utf8.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static struct yasl_private_display *display;
static pthread_t owner;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool wake_requested;
static unsigned refresh_depth;
static char *delivered_text; /* Text of the last returned event; freed by the next wait. */
/* The options of the open display; on_event set means callback mode. */
static struct yasl_options active;
static char *active_app_id;
static bool queue_failure_reported;

bool yasl_public_fail(int error)
{
    errno = error > 0 ? error : EIO;
    return false;
}
int yasl_public_error(int error)
{
    yasl_public_fail(error);
    return -errno;
}
int yasl_public_status(bool success)
{
    return success ? 0 : yasl_public_error(errno);
}
struct yasl_private_display *yasl_public_display(void)
{
    return display;
}
bool yasl_public_owner(void)
{
    if (!display)
        return yasl_public_fail(ENODEV);
    if (!pthread_equal(owner, pthread_self()))
        return yasl_public_fail(EPERM);
    return true;
}
bool yasl_public_surface(yasl_surface *s, bool mutation)
{
    if (!yasl_public_owner())
        return false;
    if (!s || s->display != display)
        return yasl_public_fail(EINVAL);
    if (mutation && refresh_depth)
        return yasl_public_fail(EBUSY);
    return true;
}
bool yasl_public_connected(yasl_surface *s)
{
    if (!yasl_public_surface(s, false))
        return false;
    return !atomic_load(&display->offline) || yasl_public_fail(ENOTCONN);
}
/* Runs a private call that reports failure through errno. */
#define STATUS(call) (errno = 0, yasl_public_status((call) || yasl_public_fail(errno)))

static bool refresh_ready(yasl_surface *s);
static int take_event(yasl_event *out);
/* An owed REFRESH is an event the queue does not hold. */
static bool public_pending(struct yasl_private_display *d)
{
    for (yasl_surface *s = d->surfaces; s; s = s->event_next)
        if (s->refresh_owed && refresh_ready(s))
            return true;
    return false;
}
/* Callback mode: runs in each loop step, after the display is read and before
 * descriptor callbacks. Bounded, so a handler that keeps making events cannot
 * starve the loop's other sources; the rest go in the next step, which does
 * not wait while they are pending. */
static void deliver_events(struct yasl_private_display *d)
{
    if (yasl_private_events_error(d) && !queue_failure_reported) {
        queue_failure_reported = true;
        active.on_event(active.userdata, &(yasl_event){
            .type = YASL_EVENT_DISPLAY_LOST, .action = YASL_LOST_FINAL, .time_ns = yasl_time_ns()});
    }
    for (int i = 0; i < 256; i++) {
        yasl_event event;
        if (take_event(&event) <= 0)
            break;
        active.on_event(active.userdata, &event);
        free(delivered_text);
        delivered_text = NULL;
    }
}
static bool same_options(const struct yasl_options *o)
{
    const char *app_id = o->app_id ? o->app_id : "";
    return o->backend == active.backend && o->loop == active.loop &&
           o->on_event == active.on_event && o->userdata == active.userdata &&
           !strcmp(app_id, active_app_id ? active_app_id : "");
}
int yasl_init(const struct yasl_options *options)
{
    struct yasl_options o = options ? *options : (struct yasl_options){0};
    if (display) {
        if (!yasl_public_owner())
            return -errno;
        return yasl_public_error(same_options(&o) ? EALREADY : EBUSY);
    }
    if ((unsigned)o.backend > YASL_BACKEND_COCOA || !o.loop != !o.on_event)
        return yasl_public_error(EINVAL);
    if (o.loop && !yasl_private_loop_owned(o.loop))
        return yasl_public_error(EPERM);
    char *app_id = o.app_id ? strdup(o.app_id) : NULL;
    if (o.app_id && !app_id)
        return yasl_public_error(ENOMEM);
    struct yasl_private_display_spec spec = {
        .app_id = o.app_id,
        .backend = (enum yasl_private_backend)o.backend,
    };
    errno = 0;
    struct yasl_private_display *d = yasl_private_display_open(o.loop, &spec);
    if (!d) {
        free(app_id);
        return yasl_public_error(errno);
    }
    d->pending = public_pending;
    d->deliver = o.on_event ? deliver_events : NULL;
    active = o;
    active.app_id = NULL;
    active_app_id = app_id;
    queue_failure_reported = false;
    pthread_mutex_lock(&lifecycle_mutex);
    owner = pthread_self();
    display = d;
    pthread_mutex_unlock(&lifecycle_mutex);
    return 0;
}
bool yasl_public_initialize(void)
{
    return display ? yasl_public_owner() : yasl_init(NULL) == 0;
}
static void close_display(void)
{
    pthread_mutex_lock(&lifecycle_mutex);
    struct yasl_private_display *d = display;
    display = NULL;
    pthread_mutex_unlock(&lifecycle_mutex);
    yasl_private_display_close(d);
}
int yasl_shutdown(void)
{
    if (display && !yasl_public_owner())
        return -errno;
    if (refresh_depth || (display && yasl_private_loop_stepping(display->loop)))
        return yasl_public_error(EBUSY);
    if (display) {
        for (yasl_surface *s = display->surfaces; s; s = s->event_next)
            if (s->vulkan)
                return yasl_public_error(EBUSY);
        while (display->surfaces)
            yasl_surface_destroy(display->surfaces);
        close_display();
    }
    free(delivered_text);
    delivered_text = NULL;
    free(active_app_id);
    active_app_id = NULL;
    active = (struct yasl_options){0};
    atomic_store(&wake_requested, false);
#if YASL_HAS_VULKAN
    extern void yasl_private_vulkan_reset(void);
    yasl_private_vulkan_reset();
#endif
    return 0;
}
enum yasl_backend yasl_get_backend(void)
{
    return display && yasl_public_owner() ? (enum yasl_backend)display->ops->type
                                          : YASL_BACKEND_AUTO;
}
unsigned yasl_get_features(void)
{
    if (!display || !yasl_public_owner())
        return 0;
    static const struct {
        uint32_t cap;
        unsigned feature;
    } map[] = {
        {YASL_PRIVATE_CAP_POPUPS, YASL_FEATURE_POPUPS},
        {YASL_PRIVATE_CAP_DRAG, YASL_FEATURE_DRAG},
        {YASL_PRIVATE_CAP_DRAG_CARRY, YASL_FEATURE_DRAG_CARRY},
        {YASL_PRIVATE_CAP_PLACE_WINDOW, YASL_FEATURE_PLACE_WINDOW},
        {YASL_PRIVATE_CAP_PRIMARY_SELECTION, YASL_FEATURE_PRIMARY_SELECTION},
        {YASL_PRIVATE_CAP_TABLET, YASL_FEATURE_TABLET},
        {YASL_PRIVATE_CAP_TEXT_INPUT, YASL_FEATURE_TEXT_INPUT},
        {YASL_PRIVATE_CAP_RELATIVE_POINTER, YASL_FEATURE_LOCKED_CURSOR},
    };
    uint32_t caps = yasl_private_display_caps(display);
    unsigned features = display->ops->surface_create_gl ? YASL_FEATURE_OPENGL : 0;
#if YASL_HAS_VULKAN
    features |= YASL_FEATURE_VULKAN;
#endif
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (caps & map[i].cap)
            features |= map[i].feature;
    return features;
}

/* Surfaces */
static double scale_of(const yasl_surface *s)
{
    return s && s->scale_120 ? (double)s->scale_120 / 120.0 : 1.0;
}
static int logical(int pixels, double scale)
{
    return (int)lround(pixels / scale);
}
static bool size_valid(int width, int height)
{
    return width > 0 && height > 0 && width <= 32767 && height <= 32767 &&
           (unsigned long long)width * (unsigned)height <= INT32_MAX / 4;
}
/* Keeps the overlap of the old canvas and zeroes the rest. */
static bool canvas_fit(yasl_surface *s, int w, int h)
{
    if (s->canvas && s->canvas_width == w && s->canvas_height == h)
        return true;
    if (w <= 0 || h <= 0)
        return yasl_public_fail(EAGAIN);
    uint32_t *pixels = calloc((size_t)w * (size_t)h, sizeof(*pixels));
    if (!pixels)
        return yasl_public_fail(ENOMEM);
    int keep_w = w < s->canvas_width ? w : s->canvas_width;
    int keep_h = h < s->canvas_height ? h : s->canvas_height;
    for (int y = 0; s->canvas && y < keep_h; y++)
        memcpy(pixels + (size_t)y * w, s->canvas + (size_t)y * s->canvas_width,
               (size_t)keep_w * sizeof(*pixels));
    free(s->canvas);
    s->canvas = pixels;
    s->canvas_width = w;
    s->canvas_height = h;
    return true;
}
/* CPU presenter: every drawn frame shows the canvas; before the first present there is none. */
static void draw_canvas(yasl_surface *s, struct yasl_private_frame *frame, void *ud)
{
    (void)ud;
    if (s->presenter != YASL_PRIVATE_PRESENTER_CPU || !frame->target.pixels || !s->canvas)
        return;
    int w = frame->target.size.x, h = frame->target.size.y;
    if (!canvas_fit(s, w, h))
        return;
    memcpy(frame->target.pixels, s->canvas, (size_t)w * (size_t)h * sizeof(uint32_t));
    struct yasl_private_damage_list damage = s->cpu_damage.n != 0 ? s->cpu_damage : (struct yasl_private_damage_list){.n = -1};
    yasl_private_surface_present(s, &frame->target, &damage);
    s->cpu_damage = (struct yasl_private_damage_list){0};
}
static void register_surface(yasl_surface *s, unsigned flags, yasl_surface *parent, void *userdata)
{
    s->creation_flags = flags;
    s->parent = parent;
    s->userdata = userdata;
    if (flags & YASL_PRIVATE_SURFACE_CPU)
        s->presenter = YASL_PRIVATE_PRESENTER_CPU;
    struct yasl_private_window_cbs cb = {.draw = draw_canvas};
    yasl_private_surface_set_callbacks(s, &cb, NULL);
    s->reported_state = s->state;
    yasl_private_surface_notify_configure(s, yasl_private_surface_geometry(s));
}
/* Translates zero-means-default GL options into the private attribute list. */
static bool gl_attributes(const struct yasl_gl_options *gl, int attrs[static 31])
{
    int major = gl->major, minor = gl->minor;
    if (!major && !minor) {
        major = 3;
        minor = gl->es ? 0 : 2;
    }
    bool profiles = !gl->es && (major > 3 || (major == 3 && minor >= 2));
    int profile = !profiles          ? YASL_PRIVATE_GL_ANY
                  : gl->compatibility ? YASL_PRIVATE_GL_COMPATIBILITY
                                      : YASL_PRIVATE_GL_CORE;
    int depth = gl->no_depth_stencil ? 0 : 24, stencil = gl->no_depth_stencil ? 0 : 8;
    const int list[] = {
        YASL_PRIVATE_GL_MAJOR, major, YASL_PRIVATE_GL_MINOR, minor,
        YASL_PRIVATE_GL_PROFILE, profile,
        YASL_PRIVATE_GL_API, gl->es ? YASL_PRIVATE_GL_OPENGL_ES : YASL_PRIVATE_GL_OPENGL,
        YASL_PRIVATE_GL_RED_BITS, 8, YASL_PRIVATE_GL_GREEN_BITS, 8,
        YASL_PRIVATE_GL_BLUE_BITS, 8, YASL_PRIVATE_GL_ALPHA_BITS, 8,
        YASL_PRIVATE_GL_DEPTH_BITS, depth, YASL_PRIVATE_GL_STENCIL_BITS, stencil,
        YASL_PRIVATE_GL_SAMPLES, gl->samples, YASL_PRIVATE_GL_DEBUG, gl->debug,
        YASL_PRIVATE_GL_FORWARD_COMPAT, 0, YASL_PRIVATE_GL_SRGB, gl->srgb,
        YASL_PRIVATE_GL_END,
    };
    memcpy(attrs, list, sizeof(list));
    struct yasl_private_gl_options parsed;
    return yasl_private_gl_parse_options(attrs, &parsed) || yasl_public_fail(EINVAL);
}
static yasl_surface *create_gl(const struct yasl_private_window_spec *spec, const int *attrs,
                               yasl_surface *share)
{
    if (!display->ops->surface_create_gl)
        return yasl_public_fail(ENOTSUP), NULL;
    errno = 0;
    yasl_surface *s = display->ops->surface_create_gl(display, spec, attrs, share);
    if (!s)
        return yasl_public_fail(errno), NULL;
    if (!yasl_private_events_register(s)) {
        int error = errno;
        display->ops->surface_destroy(s);
        return yasl_public_fail(error), NULL;
    }
    s->presenter = YASL_PRIVATE_PRESENTER_GL;
    return s;
}
yasl_surface *yasl_surface_create(const struct yasl_surface_options *o)
{
    if (!o || (unsigned)o->type > YASL_SURFACE_NATIVE || !size_valid(o->width, o->height))
        return yasl_public_fail(EINVAL), NULL;
    if (refresh_depth)
        return yasl_public_fail(EBUSY), NULL;
    unsigned flags = (o->type == YASL_SURFACE_CPU ? YASL_PRIVATE_SURFACE_CPU : 0u) |
                     (o->undecorated ? YASL_PRIVATE_SURFACE_UNDECORATED : 0u) |
                     (o->fixed_size ? YASL_PRIVATE_SURFACE_FIXED_SIZE : 0u);
    int attrs[31];
    yasl_surface *share = o->type == YASL_SURFACE_GL ? o->gl.share : NULL;
    if (o->type == YASL_SURFACE_GL && !gl_attributes(&o->gl, attrs))
        return NULL;
    if (share && (!display || share->display != display ||
                  share->presenter != YASL_PRIVATE_PRESENTER_GL))
        return yasl_public_fail(EINVAL), NULL;
    bool fresh = !display;
    if (!yasl_public_initialize())
        return NULL;
    struct yasl_private_window_spec spec = {
        .title = o->title,
        .size = {o->width, o->height},
        .flags = flags,
        .external_gpu = o->type != YASL_SURFACE_CPU,
        .tearing = o->tearing,
    };
    yasl_surface *s = o->type == YASL_SURFACE_GL ? create_gl(&spec, attrs, share)
                                                 : yasl_private_surface_create_toplevel(display, &spec);
    if (!s) {
        int error = errno;
        if (fresh)
            close_display();
        return yasl_public_fail(error), NULL;
    }
    register_surface(s, flags, NULL, o->userdata);
    s->refresh = o->refresh;
    return s;
}
int yasl_surface_destroy(yasl_surface *s)
{
    if (!s)
        return 0;
    if (!yasl_public_surface(s, true))
        return -errno;
    for (yasl_surface *p = display->surfaces; p; p = p->event_next)
        for (yasl_surface *ancestor = p; ancestor; ancestor = ancestor->parent)
            if (ancestor == s && p->vulkan)
                return yasl_public_error(EBUSY);
    for (;;) {
        yasl_surface *child = NULL;
        for (yasl_surface *p = display->surfaces; p && !child; p = p->event_next)
            if (p->parent == s)
                child = p;
        if (!child)
            break;
        yasl_surface_destroy(child);
    }
    free(s->canvas);
    s->canvas = NULL;
    free(s->text_surrounding);
    s->text_surrounding = NULL;
    yasl_private_surface_destroy(s);
    return 0;
}
struct yasl_surface_info yasl_surface_get_info(const yasl_surface *s)
{
    if (!s || !yasl_public_surface((yasl_surface *)s, false))
        return (struct yasl_surface_info){.scale = 1.0};
    double scale = scale_of(s);
    return (struct yasl_surface_info){
        .width = logical(s->size.x, scale),
        .height = logical(s->size.y, scale),
        .pixel_width = s->size.x,
        .pixel_height = s->size.y,
        .scale = scale,
        .state = s->state,
    };
}
void *yasl_surface_get_userdata(const yasl_surface *s)
{
    return s && yasl_public_surface((yasl_surface *)s, false) ? s->userdata : NULL;
}
int yasl_surface_set_userdata(yasl_surface *s, void *ud)
{
    if (!yasl_public_surface(s, false))
        return -errno;
    s->userdata = ud;
    return 0;
}
int yasl_surface_set_title(yasl_surface *s, const char *title)
{
    if (!yasl_public_surface(s, false))
        return -errno;
    if (!title)
        return yasl_public_error(EINVAL);
    if (!s->display->ops->surface_set_title)
        return yasl_public_error(ENOTSUP);
    s->display->ops->surface_set_title(s, title);
    return 0;
}
int yasl_surface_request(yasl_surface *s, enum yasl_request request)
{
    if (!yasl_public_connected(s))
        return -errno;
    switch (request) {
    case YASL_REQUEST_RESTORE: {
        uint32_t state = s->state;
        errno = 0;
        bool ok = !(state & YASL_PRIVATE_STATE_FULLSCREEN) || yasl_private_surface_set_fullscreen(s, false);
        if (ok && (state & YASL_PRIVATE_STATE_MAXIMIZED || !(state & YASL_PRIVATE_STATE_FULLSCREEN)))
            ok = yasl_private_surface_set_maximized(s, false);
        return yasl_public_status(ok || yasl_public_fail(errno));
    }
    case YASL_REQUEST_FULLSCREEN:
        return STATUS(yasl_private_surface_set_fullscreen(s, true));
    case YASL_REQUEST_MAXIMIZE:
        return STATUS(yasl_private_surface_set_maximized(s, true));
    case YASL_REQUEST_MINIMIZE:
        return STATUS(yasl_private_surface_minimize(s));
    }
    return yasl_public_error(EINVAL);
}
int yasl_surface_request_size(yasl_surface *s, int w, int h)
{
    if (!yasl_public_connected(s))
        return -errno;
    double scale = scale_of(s);
    if (!size_valid(w, h) || !size_valid((int)ceil(w * scale), (int)ceil(h * scale)))
        return yasl_public_error(EINVAL);
    return STATUS(yasl_private_surface_set_size(s, (struct yasl_private_vector2i){w, h}));
}
int yasl_surface_request_frame(yasl_surface *s)
{
    if (!yasl_public_connected(s))
        return -errno;
    yasl_private_surface_request_shown_frame(s);
    return 0;
}
int yasl_surface_set_cursor(yasl_surface *s, enum yasl_cursor cursor)
{
    if (!yasl_public_surface(s, false))
        return -errno;
    if ((unsigned)cursor > YASL_CURSOR_LOCKED)
        return yasl_public_error(EINVAL);
    bool locked = cursor == YASL_CURSOR_LOCKED;
    errno = 0;
    if (!yasl_private_surface_set_pointer_mode(s, locked ? YASL_PRIVATE_POINTER_RELATIVE
                                                         : YASL_PRIVATE_POINTER_NORMAL))
        return yasl_public_error(errno);
    if (locked)
        return 0;
    if (!s->display->ops->surface_set_cursor)
        return yasl_public_error(ENOTSUP);
    yasl_private_surface_set_cursor(s, (enum yasl_private_cursor)cursor);
    return 0;
}

int yasl_surface_set_background_color(yasl_surface *s, uint32_t bg)
{
    if (!yasl_public_surface(s, false))
        return -errno;
    yasl_private_surface_set_background_color(s, bg);
    return 0;
}
void yasl_private_surface_notify_refresh(yasl_surface *s)
{
    if (!s || !s->display)
        return;
    /* modal holds only inside the system's own loop, within the wait. */
    if (s->refresh && s->display->modal && !refresh_depth) {
        ++refresh_depth;
        s->refresh(s);
        --refresh_depth;
        return;
    }
    struct yasl_private_event ev = {.type = YASL_PRIVATE_EVENT_REFRESH, .surface = s};
    yasl_private_events_push(s->display, s, &ev);
}

/* Text input */
static char *copy_text(const char *data, size_t length)
{
    if (length == SIZE_MAX || (!data && length)) {
        errno = EINVAL;
        return NULL;
    }
    char *p = malloc(length + 1);
    if (!p) {
        errno = ENOMEM;
        return NULL;
    }
    if (length)
        memcpy(p, data, length);
    p[length] = 0;
    return p;
}
static bool logical_rect(yasl_surface *s, double x, double y, double w, double h,
                         struct yasl_private_rect2i *r)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w < 0 || h < 0)
        return yasl_public_fail(EINVAL);
    double scale = scale_of(s);
    if (fabs(x) * scale >= INT_MAX || fabs(y) * scale >= INT_MAX || w * scale >= INT_MAX ||
        h * scale >= INT_MAX)
        return yasl_public_fail(EOVERFLOW);
    *r = (struct yasl_private_rect2i){
        yasl_private_scale_to_device(x, s->scale_120), yasl_private_scale_to_device(y, s->scale_120),
        yasl_private_scale_to_device(w, s->scale_120), yasl_private_scale_to_device(h, s->scale_120)};
    return true;
}
static bool utf8_boundary(const char *text, size_t length, size_t at)
{
    return at >= length || ((unsigned char)text[at] & 0xc0) != 0x80;
}
static bool text_update(yasl_surface *s)
{
    if (!s->display->ops->surface_update_text_input)
        return true;
    errno = 0;
    return s->display->ops->surface_update_text_input(s) || yasl_public_fail(errno);
}
int yasl_surface_set_text_input(yasl_surface *s, const struct yasl_text_input *in)
{
    if (!yasl_public_surface(s, false))
        return -errno;
    struct yasl_private_rect2i caret = {0};
    char *copy = NULL;
    if (in) {
        if ((unsigned)in->purpose > YASL_TEXT_TERMINAL || (!in->text && in->length) ||
            in->selection_begin > in->selection_end || in->selection_end > in->length ||
            in->length > INT32_MAX || (in->length && memchr(in->text, 0, in->length)))
            return yasl_public_error(EINVAL);
        if (!yasl_private_utf8_validate(in->text ? in->text : "", in->length) ||
            !utf8_boundary(in->text, in->length, in->selection_begin) ||
            !utf8_boundary(in->text, in->length, in->selection_end))
            return yasl_public_error(EILSEQ);
        if (!logical_rect(s, in->caret_x, in->caret_y, in->caret_width, in->caret_height, &caret))
            return -errno;
        if (!(copy = copy_text(in->text, in->length)))
            return yasl_public_error(errno);
    }
    struct text_state {
        bool active;
        unsigned purpose;
        char *surrounding;
        size_t length, begin, end;
        uint64_t revision, generation;
    } old = {s->text_input_active, s->text_purpose, s->text_surrounding, s->text_length,
             s->text_begin, s->text_end, s->text_revision, s->text_generation};
    bool active = in != NULL;
    unsigned purpose = in ? (unsigned)in->purpose : s->text_purpose;
    if (active != old.active || purpose != old.purpose)
        ++s->text_generation;
    s->text_input_active = active;
    s->text_purpose = purpose;
    s->text_surrounding = copy;
    s->text_length = in ? in->length : 0;
    s->text_begin = in ? in->selection_begin : 0;
    s->text_end = in ? in->selection_end : 0;
    s->text_revision = in ? in->revision : old.revision;
    if (!text_update(s)) {
        int error = errno;
        free(copy);
        s->text_input_active = old.active;
        s->text_purpose = old.purpose;
        s->text_surrounding = old.surrounding;
        s->text_length = old.length;
        s->text_begin = old.begin;
        s->text_end = old.end;
        s->text_revision = old.revision;
        s->text_generation = old.generation;
        return yasl_public_error(error);
    }
    free(old.surrounding);
    if (active && s->display->ops->surface_set_ime_caret)
        yasl_private_surface_set_ime_caret(s, caret);
    return 0;
}

/* Window management */
int yasl_surface_begin_move_resize(yasl_surface *s, unsigned edges)
{
    if (!yasl_public_connected(s))
        return -errno;
    if ((edges & ~15u) || (edges & 3u) == 3u || (edges & 12u) == 12u)
        return yasl_public_error(EINVAL);
    if (!edges)
        return STATUS(yasl_private_surface_start_interactive_move(s));
    return STATUS(yasl_private_surface_start_interactive_resize(s, (enum yasl_private_resize_edge)edges));
}
int yasl_surface_show_window_menu(yasl_surface *s, double x, double y)
{
    struct yasl_private_rect2i r;
    if (!yasl_public_connected(s) || !logical_rect(s, x, y, 0, 0, &r))
        return -errno;
    return STATUS(yasl_private_surface_show_window_menu(s, (struct yasl_private_vector2i){r.x, r.y}));
}

/* Popups */
static bool popup_spec(yasl_surface *parent, const struct yasl_popup_options *o,
                       struct yasl_private_popup_spec *out)
{
    /* YASL_EDGE_* bits to xdg_positioner anchor/gravity values; -1 rejects opposite edges. */
    static const signed char placement[16] = {0, 1, 2, -1, 3, 5, 6, -1, 4, 7, 8, -1, -1, -1, -1, -1};
    if (!o || !size_valid(o->width, o->height) || o->anchor > 15 || o->gravity > 15 ||
        placement[o->anchor] < 0 || placement[o->gravity] < 0 || (o->adjust & ~63u))
        return yasl_public_fail(EINVAL);
    double scale = scale_of(parent);
    if (!size_valid((int)ceil(o->width * scale), (int)ceil(o->height * scale)))
        return yasl_public_fail(EINVAL);
    *out = (struct yasl_private_popup_spec){
        .size = {yasl_private_scale_to_device(o->width, parent->scale_120),
                 yasl_private_scale_to_device(o->height, parent->scale_120)},
        .anchor_edges = (unsigned)placement[o->anchor],
        .gravity = (unsigned)placement[o->gravity],
        .constraints = o->adjust ? o->adjust
                                 : YASL_ADJUST_FLIP_X | YASL_ADJUST_FLIP_Y | YASL_ADJUST_SLIDE_X |
                                       YASL_ADJUST_SLIDE_Y,
    };
    return logical_rect(parent, o->x, o->y, o->anchor_width, o->anchor_height, &out->anchor);
}
yasl_surface *yasl_popup_create(yasl_surface *parent, const struct yasl_popup_options *o)
{
    struct yasl_private_popup_spec spec;
    if (!yasl_public_connected(parent) || !yasl_public_surface(parent, true) ||
        !popup_spec(parent, o, &spec))
        return NULL;
    errno = 0;
    yasl_surface *s = yasl_private_surface_create_popup(parent, &spec);
    if (!s)
        return yasl_public_fail(errno), NULL;
    register_surface(s, YASL_PRIVATE_SURFACE_CPU, parent, o->userdata);
    return s;
}
int yasl_popup_reposition(yasl_surface *s, const struct yasl_popup_options *o)
{
    struct yasl_private_popup_spec spec;
    if (!yasl_public_connected(s))
        return -errno;
    if (!s->parent)
        return yasl_public_error(EINVAL);
    if (!popup_spec(s->parent, o, &spec))
        return -errno;
    return STATUS(yasl_private_surface_popup_reposition(s, &spec));
}

/* Clipboard, drag and drop */
int yasl_clipboard_set(enum yasl_selection selection, const char *text)
{
    if (!yasl_public_owner())
        return -errno;
    if (atomic_load(&display->offline))
        return yasl_public_error(ENOTCONN);
    return STATUS(yasl_private_clipboard_set(display, (enum yasl_private_selection)selection, text));
}
int yasl_clipboard_request(yasl_surface *s, enum yasl_selection selection)
{
    if (!yasl_public_connected(s))
        return -errno;
    return STATUS(yasl_private_clipboard_request(s, (enum yasl_private_selection)selection) != 0);
}
/* A drag that drops on no surface would end unheard, so its origin hears
 * the end. ud is the origin's id: the origin may be gone by then. */
static void drag_ended(void *ud, enum yasl_private_drag_end how)
{
    (void)how;
    if (!display || display->drag_dropped)
        return;
    yasl_surface *origin = yasl_private_events_find(display, (uint64_t)(uintptr_t)ud);
    if (origin)
        yasl_private_surface_notify_drag(origin, YASL_PRIVATE_DRAG_CANCEL,
                                         (struct yasl_private_vector2i){0, 0});
}
int yasl_drag_start(yasl_surface *s, const struct yasl_drag_image *source)
{
    if (!yasl_public_connected(s))
        return -errno;
    struct yasl_private_drag_image image = {0};
    if (source)
        image = (struct yasl_private_drag_image){
            .pixels = source->pixels,
            .size = {source->pixel_width, source->pixel_height},
            .hotspot = {source->hotspot_x, source->hotspot_y}};
    display->drag_dropped = false;
    const struct yasl_private_drag_listener listener = {.end = drag_ended};
    return STATUS(yasl_private_display_drag_start(display, s, source ? &image : NULL, &listener,
                                                  (void *)(uintptr_t)s->id));
}
int yasl_drag_cancel(void)
{
    if (!yasl_public_owner())
        return -errno;
    yasl_private_display_drag_cancel(display);
    return 0;
}
int yasl_drag_carry(yasl_surface *s, double x, double y)
{
    struct yasl_private_rect2i r;
    if (!yasl_public_connected(s) || !logical_rect(s, x, y, 0, 0, &r))
        return -errno;
    return STATUS(yasl_private_surface_drag_carry(s, (struct yasl_private_vector2i){r.x, r.y}));
}
int yasl_surface_place_under_pointer(yasl_surface *s, double x, double y)
{
    struct yasl_private_rect2i r;
    if (!yasl_public_connected(s) || !logical_rect(s, x, y, 0, 0, &r))
        return -errno;
    return STATUS(yasl_private_surface_place_under_pointer(s, (struct yasl_private_vector2i){r.x, r.y}));
}
size_t yasl_drop_next_path(const char **cursor, char *out, size_t cap)
{
    return yasl_private_drop_next_path(cursor, out, cap);
}

/* Events */
static const char *deliver_text(const char *data, size_t length)
{
    free(delivered_text);
    delivered_text = copy_text(data, length);
    return delivered_text;
}
/* Folds native size, scale and state changes into one WINDOW snapshot; unchanged values report
 * nothing. */
static bool window_event(yasl_surface *s, struct yasl_private_vector2i size, uint32_t scale_120,
                         uint32_t state, yasl_event *e)
{
    bool resized = size.x != s->reported_size.x || size.y != s->reported_size.y ||
                   scale_120 != s->reported_scale_120;
    if (!resized && state == s->reported_state)
        return false;
    s->reported_size = size;
    s->reported_scale_120 = scale_120;
    s->reported_state = state;
    if (resized)
        s->refresh_owed = true;
    double scale = scale_120 ? scale_120 / 120.0 : 1.0;
    e->type = YASL_EVENT_WINDOW;
    e->window = (struct yasl_surface_info){
        .width = logical(size.x, scale),
        .height = logical(size.y, scale),
        .pixel_width = size.x,
        .pixel_height = size.y,
        .scale = scale,
        .state = state,
    };
    return true;
}
static bool refresh_ready(yasl_surface *s)
{
    return s->reported_size.x > 0 && s->reported_size.y > 0;
}
static bool with_text(yasl_event *e, const char *data, size_t length)
{
    e->length = length;
    e->text = deliver_text(data, length);
    return e->text != NULL;
}
/* IME offsets: -1 (unknown) places the cursor at the end of the text. */
static size_t ime_offset(int offset, size_t length)
{
    return offset < 0 || (size_t)offset > length ? length : (size_t)offset;
}
static bool convert_event(const struct yasl_private_event *p, yasl_event *e)
{
    *e = (yasl_event){.surface = p->surface, .time_ns = p->time_ns};
    double scale = scale_of(p->surface);
    switch (p->type) {
    case YASL_PRIVATE_EVENT_WINDOW_CLOSE:
        e->type = YASL_EVENT_CLOSE;
        return true;
    case YASL_PRIVATE_EVENT_DISPLAY_QUIT:
        e->type = YASL_EVENT_QUIT;
        return true;
    case YASL_PRIVATE_EVENT_DISPLAY_REOPEN:
        e->type = YASL_EVENT_REOPEN;
        return true;
    case YASL_PRIVATE_EVENT_DISPLAY_LOST:
        e->type = YASL_EVENT_DISPLAY_LOST;
        e->action = display->reconnects ? YASL_LOST_RECONNECTING : YASL_LOST_FINAL;
        return true;
    case YASL_PRIVATE_EVENT_DISPLAY_RESTORED:
        e->type = YASL_EVENT_DISPLAY_RESTORED;
        return true;
    case YASL_PRIVATE_EVENT_REFRESH:
        if (!p->surface)
            return false;
        if (!refresh_ready(p->surface)) {
            p->surface->refresh_owed = true;
            return false;
        }
        p->surface->refresh_owed = false;
        e->type = YASL_EVENT_REFRESH;
        return true;
    case YASL_PRIVATE_EVENT_INPUT_RESET:
        e->type = YASL_EVENT_INPUT_RESET;
        return true;
    case YASL_PRIVATE_EVENT_FOCUS:
        /* Activation comes from the window state; losing focus releases held input. */
        if (p->focus.focused)
            return false;
        e->type = YASL_EVENT_INPUT_RESET;
        return true;
    case YASL_PRIVATE_EVENT_WINDOW_STATE:
        return p->surface && window_event(p->surface, p->surface->reported_size,
                                          p->surface->reported_scale_120, p->state.flags, e);
    case YASL_PRIVATE_EVENT_WINDOW_RESIZE:
        /* The current geometry, not the queued one: resizes made while the
         * application was away, as during a macOS live resize, fold into one
         * event, and an application that applied yasl_surface_get_info in its
         * refresh callback never steps back through older sizes. */
        return p->surface && window_event(p->surface, p->surface->size, p->surface->scale_120,
                                          p->surface->reported_state, e);
    case YASL_PRIVATE_EVENT_KEY:
        e->type = YASL_EVENT_KEY;
        e->action = (unsigned)p->key.action;
        e->mods = p->key.mods;
        e->key = (struct yasl_key_event){
            .key = p->key.key, .codepoint = p->key.codepoint,
            .shifted = p->key.shifted, .native_code = p->key.native_code,
            .text_input = p->key.text_input, .consumed_mods = p->key.consumed_mods,
            .text_id = p->key.text_id,
        };
        return true;
    case YASL_PRIVATE_EVENT_TEXT:
        e->type = YASL_EVENT_TEXT;
        e->ime.key_id = p->text_key_id;
        return with_text(e, p->text.data, p->text.len);
    case YASL_PRIVATE_EVENT_DROP:
        e->type = YASL_EVENT_DROP;
        e->action = YASL_DROP_RELEASE;
        return with_text(e, p->text.data, p->text.len);
    case YASL_PRIVATE_EVENT_PASTE:
        e->type = YASL_EVENT_CLIPBOARD;
        e->action = (unsigned)p->paste.status;
        return with_text(e, p->paste.data, p->paste.len);
    case YASL_PRIVATE_EVENT_COMPOSITION:
        e->type = YASL_EVENT_COMPOSITION;
        e->ime = (struct yasl_ime_event){
            .begin = ime_offset(p->composition.begin, p->composition.len),
            .end = ime_offset(p->composition.end, p->composition.len),
            .revision = p->composition.revision,
        };
        return with_text(e, p->composition.data, p->composition.len);
    case YASL_PRIVATE_EVENT_TEXT_EDIT:
        e->type = YASL_EVENT_TEXT_EDIT;
        e->ime = (struct yasl_ime_event){
            .begin = p->text_edit.begin, .end = p->text_edit.end,
            .revision = p->text_edit.revision, .key_id = p->text_key_id,
        };
        return with_text(e, p->text_edit.data, p->text_edit.len);
    case YASL_PRIVATE_EVENT_PEN:
        e->type = YASL_EVENT_PEN;
        e->action = p->pen.action;
        e->x = p->pen.x / scale;
        e->y = p->pen.y / scale;
        e->pen = (struct yasl_pen_event){
            .device_id = p->pen.device_id,
            .tool_id = p->pen.tool_id,
            .pressure = p->pen.pressure,
            .tilt_x = p->pen.tilt_x,
            .tilt_y = p->pen.tilt_y,
            .rotation = p->pen.rotation,
            .distance = p->pen.distance,
            .tangential = p->pen.tangential,
            .wheel_degrees = p->pen.wheel_degrees,
            .tool = (enum yasl_pen_tool)p->pen.tool,
            .axes = p->pen.axes_valid,
            .buttons = p->pen.buttons,
            .button = p->pen.changed_button,
            .in_contact = p->pen.in_contact,
        };
        return true;
    case YASL_PRIVATE_EVENT_POINTER_SCROLL: {
        unsigned valid = p->pointer.scroll_valid ? p->pointer.scroll_valid : YASL_PRIVATE_SCROLL_LINES;
        bool lines = valid & YASL_PRIVATE_SCROLL_LINES, pixels = valid & YASL_PRIVATE_SCROLL_PIXELS;
        e->type = YASL_EVENT_SCROLL;
        e->mods = p->pointer.mods;
        e->x = p->pointer.pos.x / scale;
        e->y = p->pointer.pos.y / scale;
        e->scroll = (struct yasl_scroll_event){
            .lines_x = lines ? p->pointer.scroll.x : 0,
            .lines_y = lines ? p->pointer.scroll.y : 0,
            .pixels_x = pixels ? p->pointer.pixel_scroll.x / scale : 0,
            .pixels_y = pixels ? p->pointer.pixel_scroll.y / scale : 0,
            .source = (enum yasl_scroll_source)p->pointer.scroll_source,
            .phase = (enum yasl_scroll_phase)p->pointer.scroll_phase,
            .momentum = p->pointer.scroll_momentum,
        };
        return true;
    }
    case YASL_PRIVATE_EVENT_POINTER_DOWN:
    case YASL_PRIVATE_EVENT_POINTER_UP:
    case YASL_PRIVATE_EVENT_POINTER_MOTION:
    case YASL_PRIVATE_EVENT_POINTER_LEAVE:
        e->type = YASL_EVENT_POINTER;
        e->action = p->type == YASL_PRIVATE_EVENT_POINTER_DOWN    ? YASL_POINTER_DOWN
                    : p->type == YASL_PRIVATE_EVENT_POINTER_UP    ? YASL_POINTER_UP
                    : p->type == YASL_PRIVATE_EVENT_POINTER_LEAVE ? YASL_POINTER_LEAVE
                                                                  : YASL_POINTER_MOVE;
        e->mods = p->pointer.mods;
        e->x = (p->pointer.precise_position ? p->pointer.precise_x : p->pointer.pos.x) / scale;
        e->y = (p->pointer.precise_position ? p->pointer.precise_y : p->pointer.pos.y) / scale;
        e->pointer = (struct yasl_pointer_event){p->pointer.delta.x / scale, p->pointer.delta.y / scale,
                                                 p->pointer.button, p->pointer.buttons};
        return true;
    case YASL_PRIVATE_EVENT_DRAG:
        e->type = YASL_EVENT_DRAG;
        e->action = p->drag.action == YASL_PRIVATE_DRAG_OVER     ? YASL_DROP_OVER
                    : p->drag.action == YASL_PRIVATE_DRAG_LEAVE  ? YASL_DROP_LEAVE
                    : p->drag.action == YASL_PRIVATE_DRAG_CANCEL ? YASL_DROP_CANCEL
                                                                 : YASL_DROP_RELEASE;
        e->x = p->drag.pos.x / scale;
        e->y = p->drag.pos.y / scale;
        return true;
    case YASL_PRIVATE_EVENT_DROP_HOVER:
        e->type = YASL_EVENT_DROP;
        e->action = p->drop.action == YASL_PRIVATE_DROP_LEAVE ? YASL_DROP_LEAVE : YASL_DROP_OVER;
        e->x = p->drop.pos.x / scale;
        e->y = p->drop.pos.y / scale;
        return true;
    default:
        return false;
    }
}
/* Queued events go first: input reaches the application before it draws, and
 * a queued REFRESH also pays one owed to the same surface. */
static int take_event(yasl_event *out)
{
    struct yasl_private_event p;
    while (yasl_private_display_poll_event(display, &p)) {
        /* Input method events from an older text field are stale. */
        if (p.surface && ((p.type == YASL_PRIVATE_EVENT_COMPOSITION &&
                           p.composition.generation != p.surface->text_generation) ||
                          (p.type == YASL_PRIVATE_EVENT_TEXT_EDIT &&
                           p.text_edit.generation != p.surface->text_generation)))
            continue;
        yasl_event event;
        errno = 0;
        if (convert_event(&p, &event)) {
            *out = event;
            return 1;
        }
        if (errno)
            return yasl_public_error(errno);
    }
    for (yasl_surface *s = display->surfaces; s; s = s->event_next)
        if (s->refresh_owed && refresh_ready(s)) {
            s->refresh_owed = false;
            *out = (yasl_event){.type = YASL_EVENT_REFRESH, .surface = s, .time_ns = yasl_time_ns()};
            return 1;
        }
    return 0;
}
/* Internal wakeups, such as a redraw request, keep waiting; only an event, the deadline or
 * yasl_wake end the wait. */
static int pump(yasl_event *out, int timeout)
{
    uint64_t deadline = timeout > 0 ? yasl_time_ns() + (uint64_t)timeout * 1000000u : 0;
    for (;;) {
        int n = take_event(out);
        if (n)
            return n;
        int error = yasl_private_events_error(display);
        if (error)
            return yasl_public_error(error);
        if (atomic_exchange(&wake_requested, false))
            return 0;
        /* A loss that does not reconnect ends the waits once its event is out. */
        if (atomic_load(&display->offline) && !display->reconnects)
            return yasl_public_error(ENOTCONN);
        int wait = timeout;
        if (timeout > 0) {
            uint64_t now = yasl_time_ns();
            if (now >= deadline)
                return 0;
            wait = (int)((deadline - now + 999999u) / 1000000u);
        }
        int stepped = yasl_loop_step(display->loop, wait);
        if (stepped < 0)
            return yasl_public_error(-stepped);
        if (timeout == 0)
            return take_event(out);
    }
}
int yasl_wait_event(yasl_event *out, int timeout)
{
    if (!out)
        return yasl_public_error(EINVAL);
    *out = (yasl_event){.type = YASL_EVENT_NONE};
    if (!yasl_public_owner())
        return -errno;
    if (active.on_event || refresh_depth || yasl_private_loop_stepping(display->loop))
        return yasl_public_error(EBUSY);
    free(delivered_text);
    delivered_text = NULL;
    int n = pump(out, timeout);
    if (n < 0)
        *out = (yasl_event){.type = YASL_EVENT_NONE};
    return n;
}
void yasl_wake(void)
{
    atomic_store(&wake_requested, true);
    pthread_mutex_lock(&lifecycle_mutex);
    if (display && display->loop)
        yasl_private_loop_kick(display->loop);
    pthread_mutex_unlock(&lifecycle_mutex);
}
uint64_t yasl_time_ns(void)
{
    return yasl_private_loop_now_ns();
}

/* CPU rendering */
uint32_t *yasl_cpu_pixels(yasl_surface *s, int *pw, int *ph)
{
    if (pw)
        *pw = 0;
    if (ph)
        *ph = 0;
    if (!yasl_public_surface(s, false))
        return NULL;
    if (s->presenter != YASL_PRIVATE_PRESENTER_CPU)
        return yasl_public_fail(ENOTSUP), NULL;
    if (!canvas_fit(s, s->size.x, s->size.y))
        return NULL;
    if (pw)
        *pw = s->canvas_width;
    if (ph)
        *ph = s->canvas_height;
    return s->canvas;
}
int yasl_cpu_present_damage(yasl_surface *s, const struct yasl_damage *damage, int count)
{
    if (!yasl_public_connected(s))
        return -errno;
    if (s->presenter != YASL_PRIVATE_PRESENTER_CPU)
        return yasl_public_error(ENOTSUP);
    if (!canvas_fit(s, s->size.x, s->size.y))
        return -errno;
    if (damage && count > 0) {
        s->cpu_damage.n = count <= 16 ? count : 16;
        for (int i = 0; i < s->cpu_damage.n; i++) {
            s->cpu_damage.rects[i] = (struct yasl_private_rect2i){
                .x = damage[i].x, .y = damage[i].y,
                .width = damage[i].width, .height = damage[i].height,
            };
        }
    } else {
        s->cpu_damage.n = -1;
    }
    yasl_private_surface_request_frame(s);
    yasl_private_display_flush(s->display);
    return 0;
}
int yasl_cpu_present(yasl_surface *s)
{
    return yasl_cpu_present_damage(s, NULL, 0);
}

/* OpenGL; these may run on a rendering thread, so they read only immutable or atomic state. */
static bool gl_surface(yasl_surface *s)
{
    if (!display)
        return yasl_public_fail(ENODEV);
    if (!s || s->display != display || s->presenter != YASL_PRIVATE_PRESENTER_GL)
        return yasl_public_fail(EINVAL);
    return !atomic_load(&display->offline) || yasl_public_fail(ENOTCONN);
}
int yasl_gl_make_current(yasl_surface *s)
{
    if (!s && (!display || !display->ops->gl_make_current))
        return 0; /* Nothing can be current. */
    if (s && !gl_surface(s))
        return -errno;
    if (!display->ops->gl_make_current)
        return yasl_public_error(ENOTSUP);
    return STATUS(display->ops->gl_make_current(display, s));
}
yasl_gl_proc yasl_gl_get_proc_address(const char *name)
{
    if (!display)
        return yasl_public_fail(ENODEV), NULL;
    if (!name || !*name)
        return yasl_public_fail(EINVAL), NULL;
    if (atomic_load(&display->offline))
        return yasl_public_fail(ENOTCONN), NULL;
    if (!display->ops->gl_get_proc_address)
        return yasl_public_fail(ENOTSUP), NULL;
    yasl_gl_proc p = display->ops->gl_get_proc_address(display, name);
    if (!p)
        yasl_public_fail(ENOENT);
    return p;
}
int yasl_gl_set_swap_interval(yasl_surface *s, int interval)
{
    if (!gl_surface(s))
        return -errno;
    if (!display->ops->gl_swap_interval)
        return yasl_public_error(ENOTSUP);
    return STATUS(display->ops->gl_swap_interval(display, s, interval));
}
int yasl_gl_present(yasl_surface *s)
{
    if (!gl_surface(s))
        return -errno;
    if (!display->ops->gl_swap_buffers)
        return yasl_public_error(ENOTSUP);
    int status = STATUS(display->ops->gl_swap_buffers(display, s));
    if (status == 0)
        atomic_store(&s->presented_ns, yasl_private_loop_now_ns());
    return status;
}
