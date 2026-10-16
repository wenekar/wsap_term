#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SCROLL_PX_PER_LINE 10.0

struct tablet_tool {
    struct tablet_tool *next;
    struct seat *seat;
    struct zwp_tablet_tool_v2 *proxy;
    struct wp_cursor_shape_device_v1 *shape_dev;
    uint32_t proximity_serial;
    uint64_t device_id, tool_id;
    uint32_t axes_valid, axes_changed, capability_mask;
    double x, y;
    double pressure, tilt_x, tilt_y, rotation, distance, tangential, wheel_degrees;
    unsigned changed_button;
    unsigned pending_actions[8], pending_buttons[8], pending_action_count;
    bool pending_contacts[8];
    unsigned kind;
    bool in_contact, pen_in_proximity;
    bool eraser;
};

struct tablet_device {
    struct tablet_device *next;
    struct zwp_tablet_v2 *proxy;
    uint64_t id;
};

struct tablet_pad {
    struct tablet_pad *next;
    struct zwp_tablet_pad_v2 *proxy;
};

static struct yasl_private_vector2d device_precise(const struct wayland_surface *w,
                                                   wl_fixed_t x, wl_fixed_t y)
{
    return (struct yasl_private_vector2d){
        .x = wl_fixed_to_double(x) * w->scale_factor,
        .y = wl_fixed_to_double(y) * w->scale_factor,
    };
}

static struct yasl_private_vector2i device_round(struct yasl_private_vector2d p)
{
    return (struct yasl_private_vector2i){ .x = (int)lrint(p.x), .y = (int)lrint(p.y) };
}

void seat_note_input(struct seat *s, uint32_t serial)
{
    s->input_serial = serial;
    s->conn->active_seat = s;
}

uint32_t seat_held_press_serial(const struct seat *s)
{
    if (!s) return 0;
    bool held = s->pointer_buttons != 0 || (s->tool && s->tool->in_contact);
    /* A touch contact is a press too; touch_down records its serial. */
    for (int i = 0; !held && i < SEAT_TOUCH_POINTS; i++)
        held = s->touch_points[i].active;
    return held ? s->press_serial : 0;
}

static void push_pointer(struct seat *s, struct wayland_surface *w,
                         enum yasl_private_event_type type, struct yasl_private_pointer_event ev)
{
    ev.mods = (uint32_t)seat_modifiers(s);
    struct yasl_private_event input_ev = {
        .type = type,
        .surface = &w->base,
        .pointer = ev,
    };
    input_queue_push(&surface_root(w)->input, &input_ev);
}

static unsigned scroll_source_of(uint32_t source)
{
    switch (source) {
    case WL_POINTER_AXIS_SOURCE_WHEEL: return 1;
    case WL_POINTER_AXIS_SOURCE_WHEEL_TILT: return 1;
    case WL_POINTER_AXIS_SOURCE_FINGER: return 2;
    case WL_POINTER_AXIS_SOURCE_CONTINUOUS: return 3;
    default: return 0;
    }
}

static bool cursor_theme_for(struct wayland_display *c, int scale)
{
    if (c->cursor_theme && c->cursor_theme_scale == scale)
        return true;
    struct wl_cursor_theme *theme = wl_cursor_theme_load(c->cursor_theme_name,
            c->cursor_size * scale, c->shm);
    if (!theme)
        return c->cursor_theme != NULL;
    if (c->cursor_theme)
        wl_cursor_theme_destroy(c->cursor_theme);
    c->cursor_theme = theme;
    c->cursor_theme_scale = scale;
    return true;
}

static bool seat_cursor_surface(struct seat *s)
{
    struct wayland_display *c = s->conn;
    if (s->cursor_surface)
        return true;
    s->cursor_surface = wl_compositor_create_surface(c->compositor);
    if (!s->cursor_surface)
        return false;
    if (c->viewporter)
        s->cursor_viewport = wp_viewporter_get_viewport(c->viewporter, s->cursor_surface);
    return true;
}

static void set_cursor_theme_fallback(struct seat *s, struct wayland_surface *w,
                                      enum yasl_private_cursor shape)
{
    struct wayland_display *c = s->conn;
    int scale = (int)((w->scale_120 + YASL_PRIVATE_SCALE_UNIT - 1) / YASL_PRIVATE_SCALE_UNIT);
    if (scale < 1) scale = 1;
    if (!seat_cursor_surface(s) || !cursor_theme_for(c, scale)) return;
    static const char *names[][2] = {
        {"default", "left_ptr"},
        {"text", "xterm"},
        {"pointer", "hand2"},
        {"crosshair", "crosshair"},
        {"move", "move"},
        {"nwse-resize", "nw-resize"},
        {"nesw-resize", "ne-resize"},
        {"ew-resize", "ew-resize"},
        {"ns-resize", "ns-resize"},
    };
    int idx = (unsigned)shape < YASL_PRIVATE_CURSOR_HIDDEN ? (int)shape : 0;
    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(c->cursor_theme, names[idx][0]);
    if (!cursor) cursor = wl_cursor_theme_get_cursor(c->cursor_theme, names[idx][1]);
    if (!cursor || !cursor->image_count) return;
    struct wl_cursor_image *img = cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(img);
    if (!buffer) return;

    int theme_scale = c->cursor_theme_scale;
    if (s->cursor_viewport) {
        uint32_t scale_120 = (uint32_t)theme_scale * YASL_PRIVATE_SCALE_UNIT;
        wp_viewport_set_destination(s->cursor_viewport,
                (int)(((int64_t)(int)img->width * YASL_PRIVATE_SCALE_UNIT + scale_120 - 1) /
                      scale_120),
                (int)(((int64_t)(int)img->height * YASL_PRIVATE_SCALE_UNIT + scale_120 - 1) /
                      scale_120));
    } else {
        if (img->width % (uint32_t)theme_scale || img->height % (uint32_t)theme_scale)
            theme_scale = 1;
        wl_surface_set_buffer_scale(s->cursor_surface, theme_scale);
    }
    int shown = s->cursor_viewport ? c->cursor_theme_scale : theme_scale;
    wl_pointer_set_cursor(s->pointer, s->pointer_serial, s->cursor_surface,
            (int32_t)img->hotspot_x / shown, (int32_t)img->hotspot_y / shown);
    wl_surface_attach(s->cursor_surface, buffer, 0, 0);
    wl_surface_damage_buffer(s->cursor_surface, 0, 0,
            (int32_t)img->width, (int32_t)img->height);
    wl_surface_commit(s->cursor_surface);
}

static uint32_t cursor_shape_of(enum yasl_private_cursor shape)
{
    switch (shape) {
    case YASL_PRIVATE_CURSOR_TEXT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case YASL_PRIVATE_CURSOR_LINK: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case YASL_PRIVATE_CURSOR_CROSSHAIR: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case YASL_PRIVATE_CURSOR_MOVE: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
    case YASL_PRIVATE_CURSOR_RESIZE_EW: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
    case YASL_PRIVATE_CURSOR_RESIZE_NS: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
    case YASL_PRIVATE_CURSOR_RESIZE_NWSE: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
    case YASL_PRIVATE_CURSOR_RESIZE_NESW: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
    default: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

/* Relative mode hides the pointer but keeps w->cursor for the return to NORMAL. */
static void seat_set_cursor(struct seat *s, struct wayland_surface *w)
{
    enum yasl_private_cursor shape = w->relative ? YASL_PRIVATE_CURSOR_HIDDEN : w->cursor;
    if (!s->pointer || !s->pointer_serial || s->pointer_focus != w ||
        s->cursor_shape == (int)shape)
        return;
    if (shape == YASL_PRIVATE_CURSOR_HIDDEN)
        wl_pointer_set_cursor(s->pointer, s->pointer_serial, NULL, 0, 0);
    else if (s->cursor_shape_dev)
        wp_cursor_shape_device_v1_set_shape(s->cursor_shape_dev,
                s->pointer_serial, cursor_shape_of(shape));
    else
        set_cursor_theme_fallback(s, w, shape);
    s->cursor_shape = shape;
}

static void tool_set_cursor(struct tablet_tool *t, enum yasl_private_cursor shape)
{
    if (!t->proximity_serial)
        return;
    if (shape == YASL_PRIVATE_CURSOR_HIDDEN)
        zwp_tablet_tool_v2_set_cursor(t->proxy, t->proximity_serial, NULL, 0, 0);
    else if (t->shape_dev)
        wp_cursor_shape_device_v1_set_shape(t->shape_dev, t->proximity_serial,
                                            cursor_shape_of(shape));
}

void wayland_surface_set_cursor(struct wayland_surface *w, enum yasl_private_cursor shape)
{
    if (!w || !w->conn || w->destroyed || (unsigned)shape > YASL_PRIVATE_CURSOR_HIDDEN) return;
    w->cursor = shape;
    for (struct seat *s = w->conn->seats; s; s = s->next) {
        seat_set_cursor(s, w);
        if (s->tool && s->tablet_focus == w)
            tool_set_cursor(s->tool, shape);
    }
}

static void press_queue_flush(struct seat *s, struct press_queue *q,
                              struct wayland_surface *only, uint64_t *buttons,
                              struct yasl_private_pointer_event like)
{
    int kept = 0;
    int count = q->n;
    for (int i = 0; i < count; i++) {
        struct pending_press p = q->ev[(q->head + i) & 7];
        if (only && p.surface != only) {
            q->ev[kept++] = p;
            continue;
        }
        if (!p.surface) continue;
        uint64_t mask = 1ull << p.button;
        *buttons = (*buttons & ~mask) | ((uint64_t)(!p.release) << p.button);
        struct yasl_private_pointer_event ev = like;
        ev.pos = p.pos;
        ev.precise_x = p.precise.x;
        ev.precise_y = p.precise.y;
        ev.precise_position = true;
        ev.button = (enum yasl_private_pointer_button)p.button;
        enum yasl_private_event_type type = p.release ? YASL_PRIVATE_EVENT_POINTER_UP : YASL_PRIVATE_EVENT_POINTER_DOWN;
        push_pointer(s, p.surface, type, ev);
    }
    q->head = 0;
    q->n = kept;
}

static void press_queue_push(struct press_queue *q, struct wayland_surface *w,
                             struct yasl_private_vector2i pos, struct yasl_private_vector2d precise,
                             int button, bool release)
{
    if (q->n == 8) {
        q->head = (q->head + 1) & 7;
        q->n--;
    }
    q->ev[(q->head + q->n) & 7] = (struct pending_press){
        .surface = w, .pos = pos, .precise = precise, .button = button, .release = release,
    };
    q->n++;
}

static void press_queue_forget(struct press_queue *q, struct wayland_surface *w)
{
    for (int i = 0; i < q->n; i++) {
        int idx = (q->head + i) & 7;
        if (q->ev[idx].surface == w)
            q->ev[idx].surface = NULL;
    }
}

void seat_pointer_reset(struct seat *s)
{
    s->pointer_buttons = 0;
    s->pointer_serial = 0;
    s->motion_pending = false;
    memset(s->axis, 0, sizeof(s->axis));
    memset(s->axis_pixels, 0, sizeof(s->axis_pixels));
    memset(s->axis_value120, 0, sizeof(s->axis_value120));
    memset(s->axis_discrete, 0, sizeof(s->axis_discrete));
    memset(s->axis_has_value120, 0, sizeof(s->axis_has_value120));
    memset(s->axis_has_discrete, 0, sizeof(s->axis_has_discrete));
    s->axis_stop_pending = false;
    s->scroll_gesture = false;
    s->axis_phase = 0;
    s->axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
    s->cursor_shape = -1;
}

static void pointer_frame(void *data, struct wl_pointer *pointer);

static void pointer_event_done(struct seat *s)
{
    if (s->pointer && wl_pointer_get_version(s->pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        pointer_frame(s, s->pointer);
}

static void flush_pending_motion(struct seat *s, struct wayland_surface *w)
{
    if (!s->motion_pending) return;
    s->motion_pending = false;
    push_pointer(s, w, YASL_PRIVATE_EVENT_POINTER_MOTION, (struct yasl_private_pointer_event){
        .pos = s->pointer_pos,
        .precise_x = s->pointer_precise.x,
        .precise_y = s->pointer_precise.y,
        .precise_position = true,
    });
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface,
        wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    (void)pointer;
    struct seat *s = data;
    struct wayland_surface *w = surface ?
        (struct wayland_surface *)wl_surface_get_user_data(surface) : NULL;
    s->pointer_focus = w;
    if (!w) return;
    s->pointer_precise = device_precise(w, surface_x, surface_y);
    s->pointer_pos = device_round(s->pointer_precise);
    s->pointer_serial = serial;
    s->cursor_shape = -1;
    seat_set_cursor(s, w);
    s->motion_pending = true;
    pointer_event_done(s);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface)
{
    (void)pointer; (void)serial; (void)surface;
    struct seat *s = data;
    struct wayland_surface *w = s->pointer_focus;
    s->pointer_focus = NULL;
    if (!w) return;
    if (s->ptr_left)
        pointer_frame(s, s->pointer);
    s->ptr_left = w;
    s->ptr_left_pos = s->pointer_pos;
    s->ptr_left_precise = s->pointer_precise;
    s->ptr_left_motion = s->motion_pending;
    s->motion_pending = false;
    pointer_event_done(s);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
        uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    (void)pointer; (void)time;
    struct seat *s = data;
    struct wayland_surface *w = s->pointer_focus;
    if (!w) return;
    s->pointer_precise = device_precise(w, surface_x, surface_y);
    s->pointer_pos = device_round(s->pointer_precise);
    s->motion_pending = true;
    pointer_event_done(s);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer; (void)time;
    struct seat *s = data;
    struct wayland_surface *w = s->pointer_focus;
    if (unlikely(!w)) return;

    static const int8_t btn_map[8] = {
        [0] = 0, /* BTN_LEFT (0x110) */
        [1] = 2, /* BTN_RIGHT (0x111) */
        [2] = 1, /* BTN_MIDDLE (0x112) */
        [3] = 3, /* BTN_SIDE (0x113) */
        [4] = 4, /* BTN_EXTRA (0x114) */
        [5] = 4, /* BTN_FORWARD (0x115) */
        [6] = 3, /* BTN_BACK (0x116) */
        [7] = -1,
    };
    unsigned idx = button - 0x110;
    if (unlikely(idx >= 7)) return;
    int b = btn_map[idx];
    if (unlikely(b < 0)) return;
    seat_note_input(s, serial);
    bool release = (state == WL_POINTER_BUTTON_STATE_RELEASED);
    if (!release) {
        s->press_serial = serial;
        s->press_surface = w;
    }
    press_queue_push(&s->ptr_presses, w, s->pointer_pos, s->pointer_precise, b, release);
    pointer_event_done(s);
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer; (void)time;
    struct seat *s = data;
    if (unlikely(!s->pointer_focus)) return;
    double v = wl_fixed_to_double(value);
    if (likely(axis < 2)) {
        s->axis[axis] += v;
        s->axis_pixels[axis] += v;
        if (s->axis_source == WL_POINTER_AXIS_SOURCE_FINGER ||
            s->axis_source == WL_POINTER_AXIS_SOURCE_CONTINUOUS) {
            if (!s->scroll_gesture) s->axis_phase = 1u;
            else if (s->axis_phase != 1u) s->axis_phase = 2u;
            s->scroll_gesture = true;
        }
    }
    pointer_event_done(s);
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
    (void)pointer;
    ((struct seat *)data)->axis_source = axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    /* A stop ends a gesture, not the offsets already received in this frame. */
    (void)pointer; (void)time; (void)axis;
    ((struct seat *)data)->axis_stop_pending = true;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)pointer;
    struct seat *s = data;
    if (axis < 2) {
        s->axis_discrete[axis] += discrete;
        s->axis_has_discrete[axis] = true;
    }
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
    (void)pointer;
    struct seat *s = data;
    if (axis < 2) {
        s->axis_value120[axis] += value120;
        s->axis_has_value120[axis] = true;
    }
}

static void pointer_frame_scroll(struct seat *s, struct wayland_surface *w)
{
    double lines[2];
    for (int axis = 0; axis < 2; axis++) {
        /* A wheel may provide all three representations. Pick one, never sum
         * them. value120 preserves high-resolution detents on both axes. */
        lines[axis] = s->axis_has_value120[axis] ? s->axis_value120[axis] / 120.0 :
            s->axis_has_discrete[axis] ? (double)s->axis_discrete[axis] :
            s->axis[axis] / SCROLL_PX_PER_LINE;
        s->axis[axis] = s->axis_value120[axis] = 0;
        s->axis_discrete[axis] = 0;
        s->axis_has_value120[axis] = s->axis_has_discrete[axis] = false;
    }
    bool stopped = s->axis_stop_pending;
    unsigned source = scroll_source_of(s->axis_source);
    unsigned phase = source == 2 || source == 3
        ? (stopped ? 3u : s->axis_phase) : 0u;
    bool has_pixels = (source == 2 || source == 3) &&
        (s->axis_pixels[0] != 0 || s->axis_pixels[1] != 0);
    if (lines[0] != 0 || lines[1] != 0 || stopped) {
        push_pointer(s, w, YASL_PRIVATE_EVENT_POINTER_SCROLL,
                (struct yasl_private_pointer_event){
                    .pos = s->pointer_pos,
                    .scroll = {lines[1], lines[0]},
                    .pixel_scroll = {s->axis_pixels[1] * w->scale_factor,
                                     s->axis_pixels[0] * w->scale_factor},
                    .scroll_valid = 1u | (has_pixels ? 2u : 0u),
                    .scroll_source = source,
                    .scroll_phase = phase,
                });
    }
    memset(s->axis_pixels, 0, sizeof(s->axis_pixels));
    s->axis_stop_pending = false;
    if (stopped) s->scroll_gesture = false;
    s->axis_phase = 0;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)pointer;
    struct seat *s = data;
    struct wayland_surface *left = s->ptr_left;
    if (left) {
        s->ptr_left = NULL;
        if (s->ptr_left_motion)
            push_pointer(s, left, YASL_PRIVATE_EVENT_POINTER_MOTION, (struct yasl_private_pointer_event){
                .pos = s->ptr_left_pos,
                .precise_x = s->ptr_left_precise.x,
                .precise_y = s->ptr_left_precise.y,
                .precise_position = true,
            });
        press_queue_flush(s, &s->ptr_presses, left, &s->pointer_buttons, (struct yasl_private_pointer_event){0});
        push_pointer(s, left, YASL_PRIVATE_EVENT_POINTER_LEAVE, (struct yasl_private_pointer_event){
            .pos = s->ptr_left_pos,
        });
        struct yasl_private_vector2i pos = s->pointer_pos;
        struct yasl_private_vector2d precise = s->pointer_precise;
        bool motion = s->motion_pending;
        uint32_t serial = s->pointer_focus ? s->pointer_serial : 0;
        int shape = s->pointer_focus ? s->cursor_shape : -1;
        seat_pointer_reset(s);
        s->pointer_pos = pos;
        s->pointer_precise = precise;
        s->motion_pending = motion;
        s->pointer_serial = serial;
        s->cursor_shape = shape;
    }
    struct wayland_surface *w = s->pointer_focus;
    if (!w) {
        s->ptr_presses.n = 0;
        return;
    }

    flush_pending_motion(s, w);
    press_queue_flush(s, &s->ptr_presses, NULL, &s->pointer_buttons, (struct yasl_private_pointer_event){0});

    pointer_frame_scroll(s, w);
}

#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis, uint32_t direction)
{ (void)data; (void)pointer; (void)axis; (void)direction; }
#endif
#ifdef WL_POINTER_WARP_SINCE_VERSION
static void pointer_warp(void *data, struct wl_pointer *pointer, wl_fixed_t x, wl_fixed_t y)
{ (void)data; (void)pointer; (void)x; (void)y; }
#endif

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *pointer,
        uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy,
        wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    (void)pointer; (void)utime_hi; (void)utime_lo; (void)dx_unaccel; (void)dy_unaccel;
    struct seat *s = data;
    struct wayland_surface *w = s->pointer_focus;
    /* Unlocked movement arrives as wl_pointer.motion; the queue derives its delta. */
    if (!w || !w->pointer_locked || w->lock_seat != s) return;
    flush_pending_motion(s, w);
    push_pointer(s, w, YASL_PRIVATE_EVENT_POINTER_MOTION, (struct yasl_private_pointer_event){
        .pos = s->pointer_pos,
        .precise_x = s->pointer_precise.x,
        .precise_y = s->pointer_precise.y,
        .precise_position = true,
        .delta = {wl_fixed_to_double(dx) * w->scale_factor,
                  wl_fixed_to_double(dy) * w->scale_factor},
    });
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = relative_motion,
};

static void pointer_locked(void *data, struct zwp_locked_pointer_v1 *lock)
{
    (void)lock;
    struct wayland_surface *w = data;
    w->pointer_locked = true;
    surface_root(w)->input.relative = true;
}

static void pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *lock)
{
    (void)lock;
    struct wayland_surface *w = data;
    w->pointer_locked = false;
    surface_root(w)->input.relative = false;
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    .locked = pointer_locked,
    .unlocked = pointer_unlocked,
};

/* A persistent lock re-engages each time the compositor focuses the surface. */
static void surface_lock_pointer(struct wayland_surface *w, struct seat *s)
{
    struct wayland_display *c = w->conn;
    if (!w->relative || w->pointer_lock || w->destroyed || !w->surface || !s->pointer)
        return;
    w->pointer_lock = zwp_pointer_constraints_v1_lock_pointer(c->pointer_constraints,
            w->surface, s->pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    if (!w->pointer_lock)
        return;
    w->lock_seat = s;
    zwp_locked_pointer_v1_add_listener(w->pointer_lock, &locked_pointer_listener, w);
}

void surface_unlock_pointer(struct wayland_surface *w)
{
    if (!w->pointer_lock)
        return;
    zwp_locked_pointer_v1_destroy(w->pointer_lock);
    w->pointer_lock = NULL;
    w->lock_seat = NULL;
    if (w->pointer_locked) {
        w->pointer_locked = false;
        surface_root(w)->input.relative = false;
    }
}

static void seat_lock_surfaces(struct seat *s)
{
    for (struct wayland_surface *w = s->conn->surfaces; w; w = w->conn_next)
        surface_lock_pointer(w, s);
}

void surface_lock_preferred(struct wayland_surface *w)
{
    struct wayland_display *c = w->conn;
    /* Prefer the seat that last delivered input; others lock once it goes away. */
    struct seat *seat = c->active_seat && c->active_seat->pointer ? c->active_seat : NULL;
    for (struct seat *s = c->seats; s && !seat; s = s->next)
        if (s->pointer) seat = s;
    if (seat)
        surface_lock_pointer(w, seat);
}

bool wayland_surface_set_pointer_mode(struct wayland_surface *w, enum yasl_private_pointer_mode mode)
{
    if (!w || !w->conn || w->destroyed)
        return false;
    struct wayland_display *c = w->conn;
    bool relative = mode == YASL_PRIVATE_POINTER_RELATIVE;
    if (relative && (!c->relative_pointer_mgr || !c->pointer_constraints))
        return false;
    w->relative = relative;
    if (relative) {
        surface_lock_preferred(w);
    } else {
        surface_unlock_pointer(w);
    }
    for (struct seat *s = c->seats; s; s = s->next)
        seat_set_cursor(s, w);
    return true;
}

static const struct wl_pointer_listener pointer_listener = {
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    .axis_relative_direction = pointer_axis_relative_direction,
#endif
#ifdef WL_POINTER_WARP_SINCE_VERSION
    .warp = pointer_warp,
#endif
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
};

static struct touch_point *touch_find(struct seat *s, int32_t id)
{
    for (int i = 0; i < SEAT_TOUCH_POINTS; i++)
        if (s->touch_points[i].active && s->touch_points[i].id == id)
            return &s->touch_points[i];
    return NULL;
}

static struct yasl_private_vector2i touch_pos(const struct touch_point *p)
{
    double scale = p->surface->scale_factor;
    return (struct yasl_private_vector2i){
        .x = (int)lrint(p->x * scale), .y = (int)lrint(p->y * scale),
    };
}



static void touch_report(struct seat *s, struct touch_point *p, int button,
                         enum yasl_private_event_type type)
{
    push_pointer(s, p->surface, type, (struct yasl_private_pointer_event){
        .pos = touch_pos(p), .button = (enum yasl_private_pointer_button)button,
        .device = YASL_PRIVATE_PTR_DEV_TOUCH,
    });
}

static void touch_down(void *data, struct wl_touch *touch,
        uint32_t serial, uint32_t time, struct wl_surface *surface,
        int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)touch; (void)time;
    struct seat *s = data;
    struct wayland_surface *w = surface ?
        (struct wayland_surface *)wl_surface_get_user_data(surface) : NULL;
    if (!w || touch_find(s, id)) return;
    struct touch_point *p = NULL;
    for (int i = 0; i < SEAT_TOUCH_POINTS && !p; i++)
        if (!s->touch_points[i].active)
            p = &s->touch_points[i];
    if (!p) return;
    seat_note_input(s, serial);
    s->press_serial = serial;
    s->press_surface = w;
    *p = (struct touch_point){
        .surface = w, .id = id, .active = true, .down = true,
        .x = wl_fixed_to_double(x), .y = wl_fixed_to_double(y),
    };
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id)
{
    (void)touch; (void)serial; (void)time;
    struct touch_point *p = touch_find(data, id);
    if (p) p->up = true;
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)touch; (void)time;
    struct touch_point *p = touch_find(data, id);
    if (!p) return;
    p->x = wl_fixed_to_double(x);
    p->y = wl_fixed_to_double(y);
    p->moved = true;
}

static void touch_frame(void *data, struct wl_touch *touch)
{
    (void)touch;
    struct seat *s = data;
    int active_n = 0;
    double centroid_sum = 0;
    int centroid_n = 0;
    for (int i = 0; i < SEAT_TOUCH_POINTS; i++) {
        struct touch_point *p = &s->touch_points[i];
        if (!p->active) continue;
        if (p->down && !s->touch_primary && !s->touch_scrolling) {
            s->touch_primary = p;
            s->touch_pressed = true;
            touch_report(s, p, 0, YASL_PRIVATE_EVENT_POINTER_DOWN);
        }
        if (!p->up) {
            active_n++;
            if (s->touch_primary && p->surface == s->touch_primary->surface) {
                centroid_sum += p->y;
                centroid_n++;
            }
        }
    }
    struct touch_point *primary = s->touch_primary;
    if (primary && !s->touch_scrolling && active_n >= 2) {
        if (s->touch_pressed)
            touch_report(s, primary, 0, YASL_PRIVATE_EVENT_POINTER_UP);
        s->touch_pressed = false;
        s->touch_scrolling = true;
        s->touch_scroll_y = centroid_n ? centroid_sum / centroid_n : 0;
    } else if (s->touch_scrolling && primary && active_n >= 2) {
        double y = centroid_n ? centroid_sum / centroid_n : 0;
        double dy = y - s->touch_scroll_y;
        s->touch_scroll_y = y;
        if (dy != 0.0) {
            double lines = dy / SCROLL_PX_PER_LINE;
            push_pointer(s, primary->surface, YASL_PRIVATE_EVENT_POINTER_SCROLL, (struct yasl_private_pointer_event){
                .pos = touch_pos(primary),
                .scroll = {0, -lines},
                .device = YASL_PRIVATE_PTR_DEV_TOUCH,
            });
        }
    } else if (primary && primary->moved && s->touch_pressed) {
        touch_report(s, primary, 0, YASL_PRIVATE_EVENT_POINTER_MOTION);
    }
    for (int i = 0; i < SEAT_TOUCH_POINTS; i++) {
        struct touch_point *p = &s->touch_points[i];
        if (!p->active) continue;
        if (p->up) {
            active_n--;
            if (p == s->touch_primary) {
                if (s->touch_pressed)
                    touch_report(s, p, 0, YASL_PRIVATE_EVENT_POINTER_UP);
                s->touch_pressed = false;
                s->touch_primary = NULL;
            }
            *p = (struct touch_point){0};
        } else {
            p->down = p->moved = false;
        }
    }
    if (active_n <= 0)
        s->touch_scrolling = false;
}

static void touch_cancel(void *data, struct wl_touch *touch)
{
    (void)touch;
    struct seat *s = data;
    if (s->touch_primary && s->touch_pressed)
        touch_report(s, s->touch_primary, 0, YASL_PRIVATE_EVENT_POINTER_UP);
    memset(s->touch_points, 0, sizeof(s->touch_points));
    s->touch_primary = NULL;
    s->touch_pressed = false;
    s->touch_scrolling = false;
}

static void touch_shape(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{ (void)data; (void)touch; (void)id; (void)major; (void)minor; }
static void touch_orientation(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t orientation)
{ (void)data; (void)touch; (void)id; (void)orientation; }

static const struct wl_touch_listener touch_listener = {
    .shape = touch_shape, .orientation = touch_orientation,
    .down = touch_down, .up = touch_up, .motion = touch_motion,
    .frame = touch_frame, .cancel = touch_cancel,
};

static struct yasl_private_pointer_event tool_event(const struct seat *s)
{
    return (struct yasl_private_pointer_event){
        .device = s->tool && s->tool->eraser ? YASL_PRIVATE_PTR_DEV_ERASER : YASL_PRIVATE_PTR_DEV_PEN,
        .pen = s->pen,
    };
}

static void push_pen_sample(struct tablet_tool *tool, struct wayland_surface *w,
                            unsigned action, uint32_t changed)
{
    struct seat *s = tool->seat;
    if (!w) return;
    uint64_t buttons = s->tool_buttons;
    for (int i = 0; i < s->tool_presses.n; i++) {
        const struct pending_press *press = &s->tool_presses.ev[(s->tool_presses.head + i) & 7];
        if (press->surface && press->button >= 0 && press->button < 32) {
            uint64_t bit = 1ull << press->button;
            buttons = (buttons & ~bit) | ((uint64_t)!press->release << press->button);
        }
    }
    if (action == YASL_PRIVATE_PEN_LEAVE || action == YASL_PRIVATE_PEN_CANCEL)
        buttons = 0;
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_PEN,
        .surface = &w->base,
        .pen = {
            .device_id = tool->device_id,
            .tool_id = tool->tool_id,
            .x = tool->x,
            .y = tool->y,
            .pressure = tool->pressure,
            .tilt_x = tool->tilt_x,
            .tilt_y = tool->tilt_y,
            .rotation = tool->rotation,
            .distance = tool->distance,
            .tangential = tool->tangential,
            .wheel_degrees = tool->wheel_degrees,
            .axes_valid = tool->axes_valid,
            .axes_changed = changed,
            .buttons = (uint32_t)buttons,
            .changed_button = tool->changed_button,
            .in_contact = tool->in_contact,
            .tool = tool->kind,
            .action = action,
        },
    };
    input_queue_push(&surface_root(w)->input, &ev);
}

static void pen_action_push(struct tablet_tool *tool, unsigned action, unsigned button)
{
    if (tool->pending_action_count == 8) {
        memmove(tool->pending_actions, tool->pending_actions + 1,
                7 * sizeof(tool->pending_actions[0]));
        memmove(tool->pending_buttons, tool->pending_buttons + 1,
                7 * sizeof(tool->pending_buttons[0]));
        memmove(tool->pending_contacts, tool->pending_contacts + 1,
                7 * sizeof(tool->pending_contacts[0]));
        tool->pending_action_count--;
    }
    unsigned i = tool->pending_action_count++;
    tool->pending_actions[i] = action;
    tool->pending_buttons[i] = button;
    tool->pending_contacts[i] = tool->in_contact;
}

static void tablet_tool_proximity_in(void *data,
        struct zwp_tablet_tool_v2 *tool, uint32_t serial,
        struct zwp_tablet_v2 *tablet, struct wl_surface *surface)
{
    (void)tool;
    struct tablet_tool *t = data;
    struct seat *s = t->seat;
    struct wayland_surface *w = surface ?
        (struct wayland_surface *)wl_surface_get_user_data(surface) : NULL;
    s->tablet_focus = w;
    s->tool = t;
    t->device_id = 0;
    for (struct tablet_device *device = s->tablet_devices; device; device = device->next)
        if (device->proxy == tablet) { t->device_id = device->id; break; }
    t->pen_in_proximity = true;
    t->in_contact = false;
    t->pending_action_count = 0;
    pen_action_push(t, YASL_PRIVATE_PEN_ENTER, 0);
    t->axes_changed = 0;
    t->pressure = t->tilt_x = t->tilt_y = t->rotation = t->distance = 0;
    t->tangential = t->wheel_degrees = 0;
    s->tool_buttons = 0;
    s->pen = (struct yasl_private_pointer_pen){0};
    t->proximity_serial = serial;
    if (!w) return;
    t->x = s->tool_pos.x;
    t->y = s->tool_pos.y;
    s->tool_precise = (struct yasl_private_vector2d){ s->tool_pos.x, s->tool_pos.y };
    if (s->conn->cursor_shape_mgr && !t->shape_dev)
        t->shape_dev = wp_cursor_shape_manager_v1_get_tablet_tool_v2(
                s->conn->cursor_shape_mgr, t->proxy);
    tool_set_cursor(t, w->cursor);
    s->tool_motion = true;
}

static void tablet_tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool)
{
    (void)tool;
    struct tablet_tool *t = data;
    struct seat *s = t->seat;
    struct wayland_surface *w = s->tablet_focus;
    if (w) {
        t->in_contact = false;
        t->pressure = 0;
        push_pen_sample(t, w, YASL_PRIVATE_PEN_LEAVE, 0);
        press_queue_flush(s, &s->tool_presses, NULL, &s->tool_buttons, tool_event(s));
        s->pen.pressure = 0;
        struct yasl_private_pointer_event ev = tool_event(s);
        ev.pos = s->tool_pos;
        push_pointer(s, w, YASL_PRIVATE_EVENT_POINTER_LEAVE, ev);
    }
    s->tool_presses.n = 0;
    s->tablet_focus = NULL;
    s->tool = NULL;
    s->tool_motion = false;
    s->tool_wheel = 0;
    s->tool_buttons = 0;
    t->proximity_serial = 0;
    t->pen_in_proximity = false;
    t->in_contact = false;
}

static void tablet_tool_down(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial)
{
    (void)tool;
    struct tablet_tool *t = data;
    struct seat *s = t->seat;
    struct wayland_surface *w = s->tablet_focus;
    if (!w) return;
    seat_note_input(s, serial);
    s->press_serial = serial;
    s->press_surface = w;
    press_queue_push(&s->tool_presses, w, s->tool_pos, s->tool_precise, 0, false);
    t->in_contact = true;
    pen_action_push(t, YASL_PRIVATE_PEN_DOWN, 0);
}

static void tablet_tool_up(void *data, struct zwp_tablet_tool_v2 *tool)
{
    (void)tool;
    struct seat *s = ((struct tablet_tool *)data)->seat;
    struct tablet_tool *t = data;
    struct wayland_surface *w = s->tablet_focus;
    if (!w) return;
    s->pen.pressure = 0;
    press_queue_push(&s->tool_presses, w, s->tool_pos, s->tool_precise, 0, true);
    t->pressure = 0;
    t->in_contact = false;
    pen_action_push(t, YASL_PRIVATE_PEN_UP, 0);
    t->axes_changed |= YASL_PRIVATE_PEN_PRESSURE;
}

static void tablet_tool_motion(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x, wl_fixed_t y)
{
    (void)tool;
    struct seat *s = ((struct tablet_tool *)data)->seat;
    struct wayland_surface *w = s->tablet_focus;
    if (!w) return;
    s->tool_precise = device_precise(w, x, y);
    s->tool_pos = device_round(s->tool_precise);
    struct tablet_tool *owned = data;
    owned->x = s->tool_precise.x;
    owned->y = s->tool_precise.y;
    s->tool_motion = true;
}

static void tablet_tool_button(void *data, struct zwp_tablet_tool_v2 *tool,
        uint32_t serial, uint32_t button, uint32_t state)
{
    (void)tool;
    struct tablet_tool *t = data;
    struct seat *s = t->seat;
    struct wayland_surface *w = s->tablet_focus;
    if (!w) return;
    seat_note_input(s, serial);
    int b;
    switch (button) {
    case BTN_STYLUS:  b = 2; break;
    case BTN_STYLUS2: b = 1; break;
    case BTN_STYLUS3: b = 3; break;
    default: return;
    }
    bool release = state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED;
    press_queue_push(&s->tool_presses, w, s->tool_pos, s->tool_precise, b, release);
    pen_action_push(t, YASL_PRIVATE_PEN_BUTTON, (unsigned)b);
}

static void tool_pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t v)
{
    (void)t;
    struct seat *s = ((struct tablet_tool *)d)->seat;
    struct tablet_tool *owned = d;
    s->pen.pressure = v > 65535 ? 65535 : (uint16_t)v;
    owned->pressure = (double)(v > 65535 ? 65535 : v) / 65535.0;
    owned->axes_valid |= YASL_PRIVATE_PEN_PRESSURE;
    owned->axes_changed |= YASL_PRIVATE_PEN_PRESSURE;
    s->tool_motion = true;
}

static int8_t tilt_degrees(wl_fixed_t v)
{
    double d = wl_fixed_to_double(v);
    return (int8_t)(d < -90 ? -90 : d > 90 ? 90 : lround(d));
}

static void tool_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y)
{
    (void)t;
    struct seat *s = ((struct tablet_tool *)d)->seat;
    struct tablet_tool *owned = d;
    s->pen.tilt_x = tilt_degrees(x);
    s->pen.tilt_y = tilt_degrees(y);
    owned->tilt_x = wl_fixed_to_double(x);
    owned->tilt_y = wl_fixed_to_double(y);
    owned->axes_valid |= YASL_PRIVATE_PEN_TILT;
    owned->axes_changed |= YASL_PRIVATE_PEN_TILT;
    s->tool_motion = true;
}

static void tool_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t deg, int32_t clicks)
{
    (void)t;
    struct tablet_tool *owned = d;
    owned->seat->tool_wheel += clicks;
    owned->wheel_degrees += wl_fixed_to_double(deg);
    owned->axes_valid |= YASL_PRIVATE_PEN_WHEEL;
    owned->axes_changed |= YASL_PRIVATE_PEN_WHEEL;
}

static void tool_frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t time)
{
    (void)t; (void)time;
    struct seat *s = ((struct tablet_tool *)d)->seat;
    struct tablet_tool *owned = d;
    struct wayland_surface *w = s->tablet_focus;
    if (!w) {
        s->tool_presses.n = 0;
        s->tool_wheel = 0;
        return;
    }
    struct yasl_private_pointer_event like = tool_event(s);
    if (owned->pending_action_count) {
        unsigned actions = owned->pending_action_count;
        bool contact = owned->in_contact;
        for (unsigned i = 0; i < actions; i++) {
            owned->changed_button = owned->pending_buttons[i];
            owned->in_contact = owned->pending_contacts[i];
            push_pen_sample(owned, w, owned->pending_actions[i],
                    i + 1 == actions ? owned->axes_changed : 0);
        }
        owned->in_contact = contact;
        owned->pending_action_count = 0;
        owned->axes_changed = 0;
        owned->changed_button = 0;
    } else if (s->tool_motion || owned->axes_changed) {
        push_pen_sample(owned, w, YASL_PRIVATE_PEN_MOVE, owned->axes_changed);
        owned->axes_changed = 0;
        owned->changed_button = 0;
    }
    if (s->tool_motion) {
        struct yasl_private_pointer_event ev = like;
        ev.pos = s->tool_pos;
        ev.precise_x = s->tool_precise.x;
        ev.precise_y = s->tool_precise.y;
        ev.precise_position = true;
        push_pointer(s, w, YASL_PRIVATE_EVENT_POINTER_MOTION, ev);
        s->tool_motion = false;
    }
    press_queue_flush(s, &s->tool_presses, NULL, &s->tool_buttons, like);
    if (s->tool_wheel) {
        struct yasl_private_pointer_event ev = like;
        ev.pos = s->tool_pos;
        ev.scroll.y = s->tool_wheel;
        push_pointer(s, w, YASL_PRIVATE_EVENT_POINTER_SCROLL, ev);
        s->tool_wheel = 0;
    }
}

static void tablet_tool_removed(void *data, struct zwp_tablet_tool_v2 *tool)
{
    struct tablet_tool *t = data;
    struct seat *s = t->seat;
    if (s->tool == t) {
        tablet_tool_proximity_out(t, tool);
    }
    struct tablet_tool **link = &s->tablet_tools;
    while (*link && *link != t)
        link = &(*link)->next;
    if (*link)
        *link = t->next;
    if (t->shape_dev)
        wp_cursor_shape_device_v1_destroy(t->shape_dev);
    zwp_tablet_tool_v2_destroy(tool);
    free(t);
}

static void tool_type(void *d, struct zwp_tablet_tool_v2 *t, uint32_t type)
{
    (void)t;
    struct tablet_tool *tool = d;
    tool->eraser = type == ZWP_TABLET_TOOL_V2_TYPE_ERASER;
    switch (type) {
    case ZWP_TABLET_TOOL_V2_TYPE_PEN: tool->kind = 1; break;
    case ZWP_TABLET_TOOL_V2_TYPE_ERASER: tool->kind = 2; break;
    case ZWP_TABLET_TOOL_V2_TYPE_BRUSH: tool->kind = 3; break;
    case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH: tool->kind = 4; break;
    case ZWP_TABLET_TOOL_V2_TYPE_PENCIL: tool->kind = 5; break;
    default: tool->kind = 0; break;
    }
}

static void tool_hardware_serial(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo)
{ (void)t; struct tablet_tool *tool = d; uint64_t id = ((uint64_t)hi << 32) | lo; if (id) tool->tool_id = id; }
static void tool_hardware_id_wacom(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo) { (void)d; (void)t; (void)hi; (void)lo; }
static void tool_capability(void *d, struct zwp_tablet_tool_v2 *t, uint32_t cap)
{
    (void)t;
    struct tablet_tool *tool = d;
    tool->capability_mask |= 1u << cap;
    switch (cap) {
    case ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE: tool->axes_valid |= YASL_PRIVATE_PEN_PRESSURE; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_TILT: tool->axes_valid |= YASL_PRIVATE_PEN_TILT; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION: tool->axes_valid |= YASL_PRIVATE_PEN_ROTATION; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE: tool->axes_valid |= YASL_PRIVATE_PEN_DISTANCE; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER: tool->axes_valid |= YASL_PRIVATE_PEN_TANGENTIAL; break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL: tool->axes_valid |= YASL_PRIVATE_PEN_WHEEL; break;
    default: break;
    }
}
static void tool_done(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; }
static void tool_distance(void *d, struct zwp_tablet_tool_v2 *t, uint32_t v)
{ (void)t; struct tablet_tool *tool = d; tool->distance = (double)(v > 65535 ? 65535 : v) / 65535.0; tool->axes_valid |= YASL_PRIVATE_PEN_DISTANCE; tool->axes_changed |= YASL_PRIVATE_PEN_DISTANCE; }
static void tool_rotation(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t deg)
{ (void)t; struct tablet_tool *tool = d; tool->rotation = wl_fixed_to_double(deg); tool->axes_valid |= YASL_PRIVATE_PEN_ROTATION; tool->axes_changed |= YASL_PRIVATE_PEN_ROTATION; }
static void tool_slider(void *d, struct zwp_tablet_tool_v2 *t, int32_t pos)
{ (void)t; struct tablet_tool *tool = d; tool->tangential = pos / 65535.0; if (tool->tangential < -1) tool->tangential = -1; if (tool->tangential > 1) tool->tangential = 1; tool->axes_valid |= YASL_PRIVATE_PEN_TANGENTIAL; tool->axes_changed |= YASL_PRIVATE_PEN_TANGENTIAL; }

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
    .type = tool_type, .hardware_serial = tool_hardware_serial,
    .hardware_id_wacom = tool_hardware_id_wacom, .capability = tool_capability,
    .done = tool_done, .removed = tablet_tool_removed,
    .proximity_in = tablet_tool_proximity_in, .proximity_out = tablet_tool_proximity_out,
    .down = tablet_tool_down, .up = tablet_tool_up, .motion = tablet_tool_motion,
    .pressure = tool_pressure, .distance = tool_distance, .tilt = tool_tilt,
    .rotation = tool_rotation, .slider = tool_slider, .wheel = tool_wheel,
    .button = tablet_tool_button, .frame = tool_frame,
};

static void tablet_name(void *data, struct zwp_tablet_v2 *tablet, const char *name) { (void)data; (void)tablet; (void)name; }
static void tablet_id(void *data, struct zwp_tablet_v2 *tablet, uint32_t vid, uint32_t pid) { (void)data; (void)tablet; (void)vid; (void)pid; }
static void tablet_path(void *data, struct zwp_tablet_v2 *tablet, const char *path) { (void)data; (void)tablet; (void)path; }
static void tablet_done(void *data, struct zwp_tablet_v2 *tablet) { (void)data; (void)tablet; }
static void tablet_bustype(void *data, struct zwp_tablet_v2 *tablet, uint32_t bustype) { (void)data; (void)tablet; (void)bustype; }

static void tablet_removed(void *data, struct zwp_tablet_v2 *tablet)
{
    struct seat *s = data;
    struct tablet_device **link = &s->tablet_devices;
    while (*link && (*link)->proxy != tablet)
        link = &(*link)->next;
    if (*link) {
        struct tablet_device *removed = *link;
        *link = removed->next;
        free(removed);
    }
    zwp_tablet_v2_destroy(tablet);
}

static const struct zwp_tablet_v2_listener tablet_listener = {
    .name = tablet_name, .id = tablet_id, .path = tablet_path,
    .done = tablet_done, .removed = tablet_removed, .bustype = tablet_bustype,
};

/* yasl does not use pad groups. The group proxy belongs to the client, so
 * destroy it here rather than leak it. */
static void tablet_pad_group(void *data, struct zwp_tablet_pad_v2 *pad, struct zwp_tablet_pad_group_v2 *group) { (void)data; (void)pad; zwp_tablet_pad_group_v2_destroy(group); }
static void tablet_pad_path(void *data, struct zwp_tablet_pad_v2 *pad, const char *path) { (void)data; (void)pad; (void)path; }
static void tablet_pad_buttons(void *data, struct zwp_tablet_pad_v2 *pad, uint32_t buttons) { (void)data; (void)pad; (void)buttons; }
static void tablet_pad_done(void *data, struct zwp_tablet_pad_v2 *pad) { (void)data; (void)pad; }
static void tablet_pad_button(void *data, struct zwp_tablet_pad_v2 *pad, uint32_t time, uint32_t button, uint32_t state) { (void)data; (void)pad; (void)time; (void)button; (void)state; }
static void tablet_pad_enter(void *data, struct zwp_tablet_pad_v2 *pad, uint32_t serial, struct zwp_tablet_v2 *tablet, struct wl_surface *surface) { (void)data; (void)pad; (void)serial; (void)tablet; (void)surface; }
static void tablet_pad_leave(void *data, struct zwp_tablet_pad_v2 *pad, uint32_t serial, struct wl_surface *surface) { (void)data; (void)pad; (void)serial; (void)surface; }

static void tablet_pad_removed(void *data, struct zwp_tablet_pad_v2 *pad)
{
    struct seat *s = data;
    struct tablet_pad **link = &s->tablet_pads;
    while (*link && (*link)->proxy != pad)
        link = &(*link)->next;
    if (*link) {
        struct tablet_pad *removed = *link;
        *link = removed->next;
        free(removed);
    }
    zwp_tablet_pad_v2_destroy(pad);
}

static const struct zwp_tablet_pad_v2_listener tablet_pad_listener = {
    .group = tablet_pad_group, .path = tablet_pad_path, .buttons = tablet_pad_buttons,
    .done = tablet_pad_done, .button = tablet_pad_button, .enter = tablet_pad_enter,
    .leave = tablet_pad_leave, .removed = tablet_pad_removed,
};

void seat_destroy_tablet(struct seat *s)
{
    while (s->tablet_tools)
        tablet_tool_removed(s->tablet_tools, s->tablet_tools->proxy);
    while (s->tablet_pads)
        tablet_pad_removed(s, s->tablet_pads->proxy);
    while (s->tablet_devices)
        tablet_removed(s, s->tablet_devices->proxy);
    if (s->tablet_seat) {
        zwp_tablet_seat_v2_destroy(s->tablet_seat);
        s->tablet_seat = NULL;
    }
}

static void tablet_seat_tablet_added(void *data, struct zwp_tablet_seat_v2 *tablet_seat, struct zwp_tablet_v2 *tablet)
{
    (void)tablet_seat;
    struct seat *s = data;
    struct tablet_device *owned = calloc(1, sizeof(*owned));
    if (!owned) { zwp_tablet_v2_destroy(tablet); return; }
    owned->proxy = tablet;
    owned->id = ++s->conn->next_tool_id;
    owned->next = s->tablet_devices;
    s->tablet_devices = owned;
    zwp_tablet_v2_add_listener(tablet, &tablet_listener, s);
}

static void tablet_seat_tool_added(void *data, struct zwp_tablet_seat_v2 *tablet_seat, struct zwp_tablet_tool_v2 *tool)
{
    (void)tablet_seat;
    struct seat *s = data;
    struct tablet_tool *owned = calloc(1, sizeof(*owned));
    if (!owned) { zwp_tablet_tool_v2_destroy(tool); return; }
    owned->proxy = tool;
    owned->seat = s;
    owned->tool_id = ++s->conn->next_tool_id;
    owned->next = s->tablet_tools;
    s->tablet_tools = owned;
    zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, owned);
}

static void tablet_seat_pad_added(void *data, struct zwp_tablet_seat_v2 *tablet_seat, struct zwp_tablet_pad_v2 *pad)
{
    (void)tablet_seat;
    struct seat *s = data;
    struct tablet_pad *owned = calloc(1, sizeof(*owned));
    if (!owned) { zwp_tablet_pad_v2_destroy(pad); return; }
    owned->proxy = pad;
    owned->next = s->tablet_pads;
    s->tablet_pads = owned;
    zwp_tablet_pad_v2_add_listener(pad, &tablet_pad_listener, s);
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    .tablet_added = tablet_seat_tablet_added,
    .tool_added = tablet_seat_tool_added,
    .pad_added = tablet_seat_pad_added,
};

static void clipboard_received(const char *text, size_t len, bool uri_list,
                             struct wl_surface *on, uint64_t request_id,
                             enum yasl_private_transfer_status status, void *ud)
{
    (void)ud;
    struct wayland_surface *w = on ?
        (struct wayland_surface *)wl_surface_get_user_data(on) : NULL;
    if (uri_list) w = surface_root(w);
    if (!w || w->closed || w->destroyed) return;
    if (!uri_list) {
        yasl_private_surface_complete_paste(&w->base, request_id, status, text, len);
        return;
    }
    if (status != YASL_PRIVATE_TRANSFER_OK) return;
    struct yasl_private_event ev = {
        .surface = &w->base,
        .type = uri_list ? YASL_PRIVATE_EVENT_DROP : YASL_PRIVATE_EVENT_PASTE,
        .text = {
            .data = text,
            .len = len,
        },
    };
    input_queue_push(&w->input, &ev);
}

static void clipboard_drag(enum yasl_private_drag_event ev, struct wl_surface *on,
                           double x, double y, void *ud)
{
    struct seat *s = ud;
    conn_drag_target(s->conn, ev, on, x, y);
}

static void clipboard_drop_hover(enum yasl_private_drop_event ev, struct wl_surface *on,
                                 double x, double y, void *ud)
{
    struct seat *s = ud;
    conn_drop_target(s->conn, ev, on, x, y);
}

void seat_clipboard_ensure(struct seat *s)
{
    struct wayland_display *c = s->conn;
    if (!c->data_manager || !s->proxy)
        return;
    if (!s->clipboard) {
        s->clipboard = clipboard_create(c->wl, c->data_manager, s->proxy,
                                        c->loop, clipboard_received, s);
        if (!s->clipboard)
            return;
        clipboard_set_drag_handler(s->clipboard, clipboard_drag, s);
        clipboard_set_drop_handler(s->clipboard, clipboard_drop_hover, s);
    }
    if (c->primary_selection_mgr)
        clipboard_set_primary_manager(s->clipboard, c->primary_selection_mgr);
}

static void seat_drop_pointer(struct seat *s)
{
    struct wayland_display *c = s->conn;
    bool unlocked = false;
    for (struct wayland_surface *w = c->surfaces; w; w = w->conn_next) {
        if (w->lock_seat == s) {
            surface_unlock_pointer(w);
            unlocked = true;
        }
    }
    if (s->relative_pointer) {
        zwp_relative_pointer_v1_destroy(s->relative_pointer);
        s->relative_pointer = NULL;
    }
    if (s->cursor_shape_dev) {
        wp_cursor_shape_device_v1_destroy(s->cursor_shape_dev);
        s->cursor_shape_dev = NULL;
    }
    if (s->pointer) {
        if (wl_pointer_get_version(s->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
            wl_pointer_release(s->pointer);
        else
            wl_pointer_destroy(s->pointer);
        s->pointer = NULL;
    }
    seat_pointer_reset(s);
    s->pointer_focus = s->ptr_left = NULL;
    s->ptr_presses.n = 0;
    for (struct seat *o = c->seats; unlocked && o; o = o->next)
        if (o != s && o->pointer)
            seat_lock_surfaces(o);
}

static void seat_drop_keyboard(struct seat *s)
{
    keyboard_repeat_stop(s);
    keyboard_text_reset(s);
    if (s->keyboard) {
        if (wl_keyboard_get_version(s->keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
            wl_keyboard_release(s->keyboard);
        else
            wl_keyboard_destroy(s->keyboard);
        s->keyboard = NULL;
    }
    s->repeat_rate = 0;
    struct wayland_surface *w = s->keyboard_focus;
    s->keyboard_focus = NULL;
    if (w && !s->focus_leaving)
        s->focus_leaving = w;
}

static void seat_drop_touch(struct seat *s)
{
    if (s->touch) {
        touch_cancel(s, s->touch);
        if (wl_touch_get_version(s->touch) >= WL_TOUCH_RELEASE_SINCE_VERSION)
            wl_touch_release(s->touch);
        else
            wl_touch_destroy(s->touch);
        s->touch = NULL;
    }
}

void seat_ensure_text_input(struct seat *s)
{
    if (!s || !s->conn->text_input_mgr || s->text_input || !s->proxy)
        return;
    s->text_input = zwp_text_input_manager_v3_get_text_input(
            s->conn->text_input_mgr, s->proxy);
    if (s->text_input)
        zwp_text_input_v3_add_listener(s->text_input, &text_input_listener, s);
}

static void seat_capabilities(void *data, struct wl_seat *proxy, uint32_t caps)
{
    struct seat *s = data;
    struct wayland_display *c = s->conn;
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!s->keyboard) {
            s->keyboard = wl_seat_get_keyboard(proxy);
            if (s->keyboard)
                wl_keyboard_add_listener(s->keyboard, &keyboard_listener, s);
            if (s->keyboard && wl_keyboard_get_version(s->keyboard) <
                    WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
                s->repeat_rate = 25;
                s->repeat_delay = 600;
            }
        }
    } else if (s->keyboard) {
        seat_drop_keyboard(s);
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (!s->pointer) {
            s->pointer = wl_seat_get_pointer(proxy);
            if (s->pointer)
                wl_pointer_add_listener(s->pointer, &pointer_listener, s);
            if (s->pointer && c->cursor_shape_mgr)
                s->cursor_shape_dev = wp_cursor_shape_manager_v1_get_pointer(
                        c->cursor_shape_mgr, s->pointer);
            if (s->pointer && c->relative_pointer_mgr) {
                s->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
                        c->relative_pointer_mgr, s->pointer);
                if (s->relative_pointer)
                    zwp_relative_pointer_v1_add_listener(s->relative_pointer,
                            &relative_pointer_listener, s);
            }
            if (s->pointer)
                seat_lock_surfaces(s);
        }
    } else if (s->pointer) {
        seat_drop_pointer(s);
    }

    if (caps & WL_SEAT_CAPABILITY_TOUCH) {
        if (!s->touch) {
            s->touch = wl_seat_get_touch(proxy);
            if (s->touch)
                wl_touch_add_listener(s->touch, &touch_listener, s);
        }
    } else if (s->touch) {
        seat_drop_touch(s);
    }

    if (c->tablet_mgr && !s->tablet_seat) {
        s->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(c->tablet_mgr, proxy);
        if (s->tablet_seat)
            zwp_tablet_seat_v2_add_listener(s->tablet_seat, &tablet_seat_listener, s);
    }

    seat_ensure_text_input(s);

    seat_clipboard_ensure(s);
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }

const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

void seat_bind(struct wayland_display *c, struct wl_registry *registry,
               uint32_t name, uint32_t version)
{
    struct seat *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->conn = c;
    s->name = name;
    s->repeat_fd = -1;
    s->cursor_shape = -1;
    s->proxy = wl_registry_bind(registry, name, &wl_seat_interface,
                                version < 8 ? version : 8);
    if (!s->proxy) {
        free(s);
        return;
    }
    s->next = c->seats;
    c->seats = s;
    wl_seat_add_listener(s->proxy, &seat_listener, s);
    keyboard_repeat_init(s);
}

void seat_forget_surface(struct seat *s, struct wayland_surface *w)
{
    clipboard_forget_surface(s->clipboard, w->surface);
    struct wayland_surface **refs[] = {
        &s->pointer_focus, &s->keyboard_focus,
        &s->tablet_focus, &s->ime_focus, &s->focus_leaving, &s->ptr_left,
        &s->press_surface,
    };
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++)
        if (*refs[i] == w)
            *refs[i] = NULL;
    press_queue_forget(&s->ptr_presses, w);
    press_queue_forget(&s->tool_presses, w);
    for (int i = 0; i < SEAT_TOUCH_POINTS; i++) {
        if (s->touch_points[i].active && s->touch_points[i].surface == w) {
            if (s->touch_primary == &s->touch_points[i]) {
                s->touch_primary = NULL;
                s->touch_pressed = false;
            }
            s->touch_points[i] = (struct touch_point){0};
        }
    }
}

void seat_destroy(struct seat *s)
{
    struct wayland_display *c = s->conn;
    for (struct seat **link = &c->seats; *link; link = &(*link)->next) {
        if (*link == s) {
            *link = s->next;
            break;
        }
    }
    if (c->active_seat == s)
        c->active_seat = NULL;
    for (struct wayland_surface *w = c->surfaces; w; w = w->conn_next) {
        if (w->ime_seat == s) {
            w->ime_seat = NULL;
            ime_reset(w);
        }
    }
    if (c->drag.seat == s)
        conn_drag_end(c);
    if (s->text_input)
        zwp_text_input_v3_destroy(s->text_input);
    seat_destroy_tablet(s);
    seat_drop_pointer(s);
    seat_drop_keyboard(s);
    struct wayland_surface *left = s->focus_leaving;
    if (left && !conn_keyboard_on(c, left, NULL)) {
        struct yasl_private_event ev = {
            .type = YASL_PRIVATE_EVENT_FOCUS,
            .surface = &left->base,
            .focus = { .focused = false },
        };
        input_queue_push(&left->input, &ev);
    }
    seat_drop_touch(s);
    clipboard_destroy(s->clipboard);
    if (s->repeat_watch > 0) yasl_loop_del_fd(c->loop, s->repeat_watch);
    if (s->repeat_fd >= 0) close(s->repeat_fd);
    if (s->cursor_viewport) wp_viewport_destroy(s->cursor_viewport);
    if (s->cursor_surface) wl_surface_destroy(s->cursor_surface);
    if (s->xkb_state) xkb_state_unref(s->xkb_state);
    if (s->compose) xkb_compose_state_unref(s->compose);
    if (s->xkb_keymap) xkb_keymap_unref(s->xkb_keymap);
    if (s->proxy) {
        if (wl_seat_get_version(s->proxy) >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(s->proxy);
        else
            wl_seat_destroy(s->proxy);
    }
    free(s);
}

bool seat_remove(struct wayland_display *c, uint32_t name)
{
    for (struct seat *s = c->seats; s; s = s->next) {
        if (s->name == name) {
            seat_destroy(s);
            return true;
        }
    }
    return false;
}

int display_input_init(struct wayland_display *c)
{
    for (struct seat *s = c->seats; s; s = s->next) {
        keyboard_repeat_init(s);
        seat_clipboard_ensure(s);
    }
    return 0;
}

void surface_init_cursor(struct wayland_display *c)
{
    if (c->cursor_shape_mgr)
        return;
    const char *env_size = getenv("XCURSOR_SIZE");
    int size = env_size ? atoi(env_size) : 0;
    c->cursor_theme_name = getenv("XCURSOR_THEME");
    c->cursor_size = size > 0 && size <= 512 ? size : 24;
    cursor_theme_for(c, 1);
}
