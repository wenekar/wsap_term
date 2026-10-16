#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "common/backend.h"
#include "common/input.h"
#include "common/keyboard.h"
#include "keymap.h"
#include "common/utf8.h"
#include "common/timing.h"
#include "platform/freedesktop/dbus.h"

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XShm.h>
#if YASL_HAS_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif
#include <sys/ipc.h>
#include <sys/shm.h>
#if YASL_HAS_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#include <xkbcommon/xkbcommon.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

#define TEXT_LIMIT (16 * 1024 * 1024)
#define PASTE_TIMEOUT_NS YASL_PRIVATE_MS_TO_NS(30000)

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

struct x11_surface {
    struct yasl_surface base;
    struct x11_surface *next;
    struct x11_surface *dead_next;
    Window window;
    GC gc;
    Colormap colormap;
    Visual *visual;
    int visual_depth;
    XIC ic;
#if YASL_HAS_EGL
    EGLContext gl_context;
    EGLSurface gl_surface;
    EGLConfig gl_config;
    int swap_interval; /* Applied whenever this surface becomes current. */
#endif
    XImage *image;
    uint32_t *pixels;
    bool shm_active;
    XShmSegmentInfo shm_info;
    struct yasl_private_vector2i size;
    const struct yasl_private_window_cbs *cbs;
    void *ud;
    struct input_queue input;
    struct yasl_private_frame frame;
    struct yasl_private_damage_list damage;
    struct yasl_private_frame_clock clock;
    bool want_draw, drawing, presented, destroyed;
    bool focused;
    struct x11_surface *parent;
    bool is_popup;
    struct yasl_private_vector2i content_offset;
    char *ime_preedit;
    int ime_cursor_begin;
    int ime_cursor_end;
    struct yasl_private_rect2i ime_caret;
};

struct paste_request {
    struct paste_request *next;
    struct x11_display *display;
    struct x11_surface *target;
    Window window;
    Atom selection;
    uint64_t id;
    yasl_private_timer_id timeout;
    /* INCR receive: chunks accumulate in buf until a zero-length chunk arrives. */
    bool incr;
    Atom incr_type;
    char *buf;
    size_t buf_len;
};

/* An INCR transfer this backend sends to a requestor. data is a private copy, so
 * clipboard changes do not affect a transfer in progress. */
struct incr_send {
    struct incr_send *next;
    struct x11_display *display;
    Window requestor;
    Atom property, target;
    char *data;
    size_t length, offset;
    bool finishing;
    yasl_private_timer_id timeout;
};

struct tablet_axis {
    int number;
    double min, max, value;
    bool valid;
};

struct x11_scroll_axis {
    int number;
    double increment, value;
    bool valid, initialized;
    int type;
};

struct x11_pointer {
    struct x11_pointer *next;
    int id;
    enum yasl_private_pointer_device device;
    unsigned tool;
    struct tablet_axis pressure, tilt_x, tilt_y, rotation, distance, tangential, wheel;
    struct x11_scroll_axis scroll_x, scroll_y;
    uint32_t buttons;
    bool finger_scroll;
};

struct x11_display {
    struct yasl_private_display base;
    struct x11_display *next_open;
    Display *native;
    int screen;
    struct yasl_loop *loop;
    int watch;
    struct x11_surface *surfaces, *dead;
    int depth;
    /* 32-bit TrueColor visual for CPU windows, when the server has one. */
    Visual *argb_visual;
    int argb_depth;
    /* Device scale numerator from Xft.dpi, with 120 representing 1x. */
    uint32_t scale_120;
    XIM im;
    Window selection_window;
    /* Server time of the latest key or button event, and the time each selection was taken. */
    Time last_time, selection_time[2];
    struct incr_send *incr_sends;
    Atom protocols, delete_window, utf8, clipboard, targets, paste_property, incr;
    Atom timestamp, multiple, pair;
    Atom net_name, net_state, fullscreen, maximized_h, maximized_v, motif_hints;
    Atom xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_leave, xdnd_drop, xdnd_finished;
    Atom xdnd_selection, xdnd_action_copy, xdnd_type_list, uri_list;
    char *selection[2];
    struct paste_request *pastes;
    uint32_t mods;
    char *app_id;
    bool owns_loop;
    int xi_opcode;
    bool xi2;
    bool has_shm;
    bool has_xkb;
    bool lost;
    bool popup_grab; /* The pointer is grabbed for the topmost popup. */
    int popup_device; /* XI2 device holding that grab. */
    int xkb_event;
    enum yasl_key keys[256];
    Cursor cursors[16];
    Atom abs_pressure, abs_tilt_x, abs_tilt_y, abs_rotation, abs_distance, abs_tangential, abs_wheel;
    Atom tool_type, eraser;
    Atom brush, airbrush, pencil;
    struct x11_pointer *pointers;
#if YASL_HAS_EGL
    EGLDisplay egl;
    bool egl_ready;
#endif
    struct {
        bool active;
        struct x11_surface *origin;
        struct x11_surface *carried;
        struct yasl_private_vector2i carry_offset;
        struct yasl_private_drag_listener listener;
        void *ud;
        struct x11_surface *hover;
        struct yasl_private_vector2i hover_pos;
        Window icon_window;
        Pixmap icon_pixmap;
        struct yasl_private_vector2i hotspot;
    } drag;
    struct {
        Window source;
        int version;
        bool accepts;
        bool has_uri_list;
        struct x11_surface *surface;
        struct yasl_private_vector2i pos;
        Time time;
    } xdnd;
    struct {
        bool active;
        Window source;
        struct x11_surface *surface;
        Time time;
    } pending_drop;
};

static bool place(struct yasl_surface *base, struct yasl_private_vector2i at);
static void drag_end(struct x11_display *d, enum yasl_private_drag_end how);
static void drag_cancel(struct yasl_private_display *base);
static void schedule_redraw(struct yasl_surface *base);
static uint64_t paste_timeout(void *ud, yasl_private_timer_id id, uint64_t interval_ns);

static void leave(struct x11_display *d)
{
    if (--d->depth) return;
    while (d->dead) {
        struct x11_surface *s = d->dead;
        d->dead = s->dead_next;
        free(s);
    }
}

static void push(struct x11_surface *s, struct yasl_private_event event,
                 const char *text, size_t len)
{
    if (s->destroyed) return;
    if (text) {
        event.text.data = text;
        event.text.len = len;
    }
    event.surface = &s->base;
    input_queue_push(&s->input, &event);
}

static void note_time(struct x11_display *d, Time time)
{
    if (time != CurrentTime) d->last_time = time;
}

/* Xft.dpi of 96 is 1x. The scale is clamped to 1x through 4x; unset is 1x. */
static uint32_t scale_from_xft_dpi(Display *native)
{
    const char *value = XGetDefault(native, "Xft", "dpi");
    char *end = NULL;
    double dpi = value ? strtod(value, &end) : 0;
    if (!value || end == value || !isfinite(dpi) || dpi <= 0) return YASL_PRIVATE_SCALE_UNIT;
    double scale = dpi / 96.0 * YASL_PRIVATE_SCALE_UNIT;
    if (scale < YASL_PRIVATE_SCALE_UNIT) scale = YASL_PRIVATE_SCALE_UNIT;
    if (scale > 4 * YASL_PRIVATE_SCALE_UNIT) scale = 4 * YASL_PRIVATE_SCALE_UNIT;
    return (uint32_t)lround(scale);
}

static unsigned long component(uint32_t color, unsigned long mask)
{
    if (unlikely(!mask)) return 0;
    unsigned shift = (unsigned)__builtin_ctzl(mask);
    unsigned long m = mask >> shift;
    return (((unsigned long)color * m + 127) / 255) << shift;
}

static void drag_set_hover(struct x11_display *d, struct x11_surface *s, struct yasl_private_vector2i pos)
{
    if (!d->drag.active) return;
    if (d->drag.hover && d->drag.hover != s) {
        struct x11_surface *old = d->drag.hover;
        struct yasl_private_vector2i old_pos = d->drag.hover_pos;
        d->drag.hover = NULL;
        if (!old->destroyed)
            yasl_private_surface_notify_drag(&old->base, YASL_PRIVATE_DRAG_LEAVE, old_pos);
        if (!d->drag.active || (s && s->destroyed)) return;
    }
    if (s && s->destroyed) return;
    d->drag.hover = s;
    d->drag.hover_pos = pos;
    if (s && !s->destroyed)
        yasl_private_surface_notify_drag(&s->base, YASL_PRIVATE_DRAG_OVER, pos);
}

static void drag_leave_hover(struct x11_display *d, struct x11_surface *s, struct yasl_private_vector2i pos)
{
    if (!d->drag.active || d->drag.hover != s) return;
    d->drag.hover = NULL;
    if (s && !s->destroyed)
        yasl_private_surface_notify_drag(&s->base, YASL_PRIVATE_DRAG_LEAVE, pos);
}

static void drag_drop_hover(struct x11_display *d)
{
    if (!d->drag.active) return;
    drag_end(d, YASL_PRIVATE_DRAG_DROPPED);
}

/* Xlib's error and IO handlers are process-wide. Open YASL displays are linked
 * here so the handlers can recognize their own errors and pass others on. */
static struct x11_display *open_displays;
static XErrorHandler previous_error_handler;
static XIOErrorHandler previous_io_handler;
static Display *shm_probe; /* Set while an MIT-SHM attach is awaiting its result. */
static bool shm_probe_failed;
static int yasl_private_x11_error_handler(Display *native, XErrorEvent *event);
static int yasl_private_x11_io_error_handler(Display *native);

static struct x11_display *display_for_native(Display *native)
{
    for (struct x11_display *d = open_displays; d; d = d->next_open)
        if (d->native == native) return d;
    return NULL;
}

static void display_register(struct x11_display *d)
{
    if (!open_displays) {
        previous_error_handler = XSetErrorHandler(yasl_private_x11_error_handler);
        previous_io_handler = XSetIOErrorHandler(yasl_private_x11_io_error_handler);
    }
    d->next_open = open_displays;
    open_displays = d;
}

static void display_unregister(struct x11_display *d)
{
    for (struct x11_display **p = &open_displays; *p; p = &(*p)->next_open) {
        if (*p != d) continue;
        *p = d->next_open;
        break;
    }
    d->next_open = NULL;
    if (open_displays) return;
    XSetErrorHandler(previous_error_handler);
    XSetIOErrorHandler(previous_io_handler);
    previous_error_handler = NULL;
    previous_io_handler = NULL;
}

/* Asynchronous errors (BadWindow, BadDrawable, BadMatch) on YASL displays are
 * suppressed during close, destruction races and speculative queries. */
static int yasl_private_x11_error_handler(Display *native, XErrorEvent *event)
{
    if (!display_for_native(event->display))
        return previous_error_handler ? previous_error_handler(native, event) : 0;
    if (event->display == shm_probe) shm_probe_failed = true;
    return 0;
}

/* Marks the display lost once and queues DISPLAY_LOST. The poll watch is removed
 * by prepare() or check(), on the thread that owns the loop, because this can run
 * on a GL thread. */
static void display_lost(struct x11_display *d)
{
    if (d->lost) return;
    d->lost = true;
    atomic_store(&d->base.offline, true);
    yasl_private_display_notify_event(&d->base, YASL_PRIVATE_EVENT_DISPLAY_LOST);
}

/* Xlib's default IO handler prints and exits before consulting the exit handler.
 * Returning here makes Xlib call the exit handler below, which returns to the caller. */
static int yasl_private_x11_io_error_handler(Display *native)
{
    if (display_for_native(native)) return 0;
    return previous_io_handler ? previous_io_handler(native) : 0;
}

static void yasl_private_x11_io_exit_handler(Display *native, void *user_data)
{
    (void)native;
    display_lost(user_data);
}

static struct x11_surface *surface_at_root_pos(struct x11_display *d, int rx, int ry, struct yasl_private_vector2i *out_pos)
{
    if (!d->native) return NULL;
    Window ignore_icon = d->drag.icon_window;
    Window ignore_carried = d->drag.carried ? d->drag.carried->window : None;

    for (struct x11_surface *s = d->surfaces; s; s = s->next) {
        if (s->destroyed || s->window == ignore_icon || s->window == ignore_carried) continue;
        int tx = 0, ty = 0;
        Window sub = None;
        if (XTranslateCoordinates(d->native, RootWindow(d->native, d->screen),
                                  s->window, rx, ry, &tx, &ty, &sub)) {
            if (tx >= 0 && ty >= 0 && tx < s->size.x && ty < s->size.y) {
                if (out_pos) *out_pos = (struct yasl_private_vector2i){tx, ty};
                return s;
            }
        }
    }
    return NULL;
}

/* Popups are prepended to the list, so the first live one is the topmost. */
static struct x11_surface *topmost_popup(struct x11_display *d)
{
    for (struct x11_surface *p = d->surfaces; p; p = p->next)
        if (p->is_popup && !p->destroyed) return p;
    return NULL;
}

#define POPUP_GRAB_MASK (ButtonPressMask | ButtonReleaseMask | PointerMotionMask | \
                         EnterWindowMask | LeaveWindowMask)

/* XI2 clients get their presses through an XI2 device grab; a core grab does not
 * deliver XI2 events to the grabbing client. */
static bool popup_grab_begin(struct x11_display *d, Window window)
{
    if (!d->xi2)
        return XGrabPointer(d->native, window, True, POPUP_GRAB_MASK, GrabModeAsync, GrabModeAsync,
                None, None, CurrentTime) == GrabSuccess;
    int device = 0;
    if (!XIGetClientPointer(d->native, window, &device)) return false;
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(bits, XI_ButtonPress);
    XISetMask(bits, XI_ButtonRelease);
    XISetMask(bits, XI_Motion);
    XISetMask(bits, XI_Enter);
    XISetMask(bits, XI_Leave);
    XIEventMask mask = {device, sizeof(bits), bits};
    if (XIGrabDevice(d->native, device, window, CurrentTime, None, GrabModeAsync, GrabModeAsync,
            True, &mask) != GrabSuccess)
        return false;
    d->popup_device = device;
    return true;
}

static void popup_grab_end(struct x11_display *d)
{
    if (d->xi2) XIUngrabDevice(d->native, d->popup_device, CurrentTime);
    else XUngrabPointer(d->native, CurrentTime);
}

/* Grabs the pointer for the topmost popup, so presses outside YASL windows reach it. */
static void popup_grab_update(struct x11_display *d)
{
    struct x11_surface *top = topmost_popup(d);
    if (!d->native || d->lost) return;
    if (d->popup_grab) popup_grab_end(d);
    d->popup_grab = top && popup_grab_begin(d, top->window);
    XFlush(d->native);
}

static void popup_dismiss_outside(struct x11_display *d, int root_x, int root_y)
{
    if (!d->popup_grab || surface_at_root_pos(d, root_x, root_y, NULL)) return;
    struct x11_surface *top = topmost_popup(d);
    if (top) yasl_private_surface_notify_close(&top->base);
}

/* The lowest pointer button held now, 1 to 5, or 0. Also returns the root position. */
static unsigned held_button(struct x11_display *d, Window window, int *root_x, int *root_y)
{
    Window root, child;
    int rx, ry, x, y;
    unsigned mask = 0;
    if (!d->native || !XQueryPointer(d->native, window, &root, &child, &rx, &ry, &x, &y, &mask))
        return 0;
    if (root_x) *root_x = rx;
    if (root_y) *root_y = ry;
    for (unsigned b = 1; b <= 5; b++)
        if (mask & (Button1Mask << (b - 1))) return b;
    return 0;
}

/* Whether the window manager lists an EWMH atom in _NET_SUPPORTED on the root. */
static bool wm_supports(struct x11_display *d, const char *name)
{
    Atom want = XInternAtom(d->native, name, False);
    Atom list = XInternAtom(d->native, "_NET_SUPPORTED", True);
    if (!list) return false;
    Window root = RootWindow(d->native, d->screen);
    for (long offset = 0;;) {
        Atom actual;
        int format;
        unsigned long count, left;
        unsigned char *data = NULL;
        if (XGetWindowProperty(d->native, root, list, offset, 1024, False, XA_ATOM,
                &actual, &format, &count, &left, &data) != Success || actual != XA_ATOM || format != 32) {
            if (data) XFree(data);
            return false;
        }
        Atom *atoms = (Atom *)data;
        bool found = false;
        for (unsigned long i = 0; data && i < count && !found; i++)
            found = atoms[i] == want;
        if (data) XFree(data);
        if (found) return true;
        if (!left) return false;
        offset += (long)count;
    }
}

static uint32_t modifiers(unsigned state)
{
    return ((state & ShiftMask) ? YASL_PRIVATE_MOD_SHIFT : 0) |
           ((state & ControlMask) ? YASL_PRIVATE_MOD_CTRL : 0) |
           ((state & Mod1Mask) ? YASL_PRIVATE_MOD_ALT : 0) |
           ((state & Mod4Mask) ? YASL_PRIVATE_MOD_SUPER : 0) |
           ((state & LockMask) ? YASL_PRIVATE_MOD_CAPS : 0);
}

static void pointer_info(struct x11_display *d, struct x11_pointer *p,
                         const XIDeviceInfo *info)
{
    p->id = info->deviceid;
    p->device = YASL_PRIVATE_PTR_DEV_MOUSE;
    p->tool = 0;
    p->buttons = 0;
    p->pressure.number = p->tilt_x.number = p->tilt_y.number = -1;
    p->rotation.number = p->distance.number = p->tangential.number = p->wheel.number = -1;
    p->scroll_x.number = p->scroll_y.number = -1;
    if (info->name) {
        for (const char *s = info->name; *s; s++) {
            if (!strncasecmp(s, "touchpad", 8) || !strncasecmp(s, "trackpad", 8)) {
                p->finger_scroll = true;
                break;
            }
        }
    }
    bool touch = false;
    for (int i = 0; i < info->num_classes; i++) {
        XIAnyClassInfo *c = info->classes[i];
        if (c->type == XITouchClass) touch = true;
        if (c->type != XIValuatorClass) continue;
        XIValuatorClassInfo *v = (XIValuatorClassInfo *)c;
        if (v->mode != XIModeAbsolute) continue;
        struct tablet_axis *axis = v->label == d->abs_pressure ? &p->pressure :
            v->label == d->abs_tilt_x ? &p->tilt_x :
            v->label == d->abs_tilt_y ? &p->tilt_y :
            v->label == d->abs_rotation ? &p->rotation :
            v->label == d->abs_distance ? &p->distance :
            v->label == d->abs_tangential ? &p->tangential :
            v->label == d->abs_wheel ? &p->wheel : NULL;
        if (axis) *axis = (struct tablet_axis){v->number, v->min, v->max, v->value, true};
    }
    for (int i = 0; i < info->num_classes; i++) {
        XIAnyClassInfo *c = info->classes[i];
        if (c->type != XIScrollClass) continue;
        XIScrollClassInfo *scroll = (XIScrollClassInfo *)c;
        struct x11_scroll_axis *axis = scroll->scroll_type == XIScrollTypeVertical ?
            &p->scroll_y : scroll->scroll_type == XIScrollTypeHorizontal ? &p->scroll_x : NULL;
        if (!axis) continue;
        *axis = (struct x11_scroll_axis){.number = scroll->number,
            .increment = scroll->increment, .type = scroll->scroll_type};
        for (int j = 0; j < info->num_classes; j++) {
            XIAnyClassInfo *vclass = info->classes[j];
            if (vclass->type != XIValuatorClass) continue;
            XIValuatorClassInfo *v = (XIValuatorClassInfo *)vclass;
            if (v->number == scroll->number) {
                axis->value = v->value;
                axis->initialized = axis->valid = true;
                break;
            }
        }
    }
    if (!touch && p->pressure.number >= 0) {
        p->device = YASL_PRIVATE_PTR_DEV_PEN;
        p->tool = 1;
        /* Xorg's Wacom and libinput drivers expose separate eraser devices.
         * Use the tool property when available, with the device name as a
         * fallback for servers (including Xwayland) without that property. */
        if (info->name) {
            for (const char *s = info->name; *s; s++)
                if (!strncasecmp(s, "eraser", 6)) { p->device = YASL_PRIVATE_PTR_DEV_ERASER; break; }
        }
    }
}

static struct x11_pointer *pointer_for(struct x11_display *d, int id)
{
    for (struct x11_pointer *p = d->pointers; p; p = p->next)
        if (p->id == id) return p;
    if (!d->native) return NULL;
    int count = 0;
    XIDeviceInfo *info = XIQueryDevice(d->native, id, &count);
    if (!info) return NULL;
    struct x11_pointer *p = count ? calloc(1, sizeof(*p)) : NULL;
    if (p) {
        pointer_info(d, p, info);
        if (p->device == YASL_PRIVATE_PTR_DEV_PEN || p->device == YASL_PRIVATE_PTR_DEV_ERASER) {
            Atom type;
            int format;
            unsigned long n, after;
            unsigned char *value = NULL;
            if (XIGetProperty(d->native, id, d->tool_type, 0, 1, False,
                    XA_ATOM, &type, &format, &n, &after, &value) == Success &&
                    type == XA_ATOM && format == 32 && n == 1 && value) {
                /* XIGetProperty packs format-32 values as uint32_t, unlike
                 * XGetWindowProperty's array of native-sized longs. */
                if (*(uint32_t *)value == d->eraser) p->device = YASL_PRIVATE_PTR_DEV_ERASER;
                else if (d->brush && *(uint32_t *)value == d->brush) p->tool = 3;
                else if (d->airbrush && *(uint32_t *)value == d->airbrush) p->tool = 4;
                else if (d->pencil && *(uint32_t *)value == d->pencil) p->tool = 5;
            }
            if (value) XFree(value);
        }
        p->next = d->pointers;
        d->pointers = p;
    }
    XIFreeDeviceInfo(info);
    return p;
}

static void clear_pointers(struct x11_display *d)
{
    while (d->pointers) {
        struct x11_pointer *p = d->pointers;
        d->pointers = p->next;
        free(p);
    }
}

static uint32_t update_axes(struct x11_pointer *p, const XIValuatorState *state)
{
    uint32_t changed = 0;
    const double *value = state->values;
    for (int i = 0; i < state->mask_len * 8; i++) {
        if (!XIMaskIsSet(state->mask, i)) continue;
        struct tablet_axis *axes[] = {&p->pressure, &p->tilt_x, &p->tilt_y, &p->rotation,
            &p->distance, &p->tangential, &p->wheel};
        const uint32_t bits[] = {YASL_PRIVATE_PEN_PRESSURE, YASL_PRIVATE_PEN_TILT,
            YASL_PRIVATE_PEN_TILT, YASL_PRIVATE_PEN_ROTATION, YASL_PRIVATE_PEN_DISTANCE,
            YASL_PRIVATE_PEN_TANGENTIAL, YASL_PRIVATE_PEN_WHEEL};
        for (unsigned a = 0; a < sizeof(axes) / sizeof(axes[0]); a++)
            if (axes[a]->number == i && isfinite(*value)) {
                axes[a]->value = *value;
                axes[a]->valid = true;
                changed |= bits[a];
            }
        value++;
    }
    return changed;
}

static void update_scroll(struct x11_pointer *p, const XIValuatorState *state,
                          struct yasl_private_pointer_event *out)
{
    const double *value = state->values;
    for (int i = 0; i < state->mask_len * 8; i++) {
        if (!XIMaskIsSet(state->mask, i)) continue;
        struct x11_scroll_axis *axes[] = {&p->scroll_x, &p->scroll_y};
        for (unsigned a = 0; a < 2; a++) {
            struct x11_scroll_axis *axis = axes[a];
            if (axis->number != i || !isfinite(*value)) continue;
            if (axis->initialized) {
                double delta = *value - axis->value;
                if (axis->increment != 0 && isfinite(delta)) {
                    double lines = delta / axis->increment;
                    if (a == 0) out->scroll.x += lines;
                    else out->scroll.y += lines;
                    if (lines != 0) {
                        out->scroll_valid |= 1u;
                        out->scroll_source = p->finger_scroll ? 2u : 1u;
                        out->scroll_phase = 2u;
                    }
                }
            }
            axis->value = *value;
            axis->initialized = true;
        }
        value++;
    }
}

static uint16_t pressure_value(const struct tablet_axis *a)
{
    if (a->number < 0 || !(a->max > a->min) || !a->valid || !isfinite(a->value)) return 0;
    double v = (a->value - a->min) / (a->max - a->min);
    return (uint16_t)lround(fmax(0, fmin(1, v)) * 65535);
}

static int8_t tilt_value(const struct tablet_axis *a)
{
    /* Xorg tablet tilt valuators are already in degrees. */
    return a->number < 0 || !a->valid || !isfinite(a->value) ? 0 :
        (int8_t)lround(fmax(-90, fmin(90, a->value)));
}

static enum yasl_private_pointer_button pointer_button(unsigned button)
{
    return button == 1 ? YASL_PRIVATE_BUTTON_PRIMARY :
           button == 2 ? YASL_PRIVATE_BUTTON_MIDDLE :
           button == 3 ? YASL_PRIVATE_BUTTON_SECONDARY :
           button >= 8 && button <= INT16_MAX ? (enum yasl_private_pointer_button)(button - 5) :
           YASL_PRIVATE_BUTTON_BACK;
}

static uint32_t pen_axes_valid(const struct x11_pointer *p)
{
    return (p->pressure.valid ? YASL_PRIVATE_PEN_PRESSURE : 0) |
           (p->tilt_x.valid || p->tilt_y.valid ? YASL_PRIVATE_PEN_TILT : 0) |
           (p->rotation.valid ? YASL_PRIVATE_PEN_ROTATION : 0) |
           (p->distance.valid ? YASL_PRIVATE_PEN_DISTANCE : 0) |
           (p->tangential.valid ? YASL_PRIVATE_PEN_TANGENTIAL : 0) |
           (p->wheel.valid ? YASL_PRIVATE_PEN_WHEEL : 0);
}

static double axis_normalized(const struct tablet_axis *axis)
{
    return axis->valid && axis->max > axis->min && isfinite(axis->value) ?
        fmax(0, fmin(1, (axis->value - axis->min) / (axis->max - axis->min))) : 0;
}

static void push_pen_sample(struct x11_surface *s, struct x11_pointer *p,
        unsigned action, double x, double y, uint32_t changed, unsigned changed_button)
{
    if (!p || (p->device != YASL_PRIVATE_PTR_DEV_PEN && p->device != YASL_PRIVATE_PTR_DEV_ERASER)) return;
    push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_PEN,
        .pen = {
            .device_id = (uint64_t)(unsigned)p->id,
            .tool_id = 0, /* XI2 exposes source IDs, but not a stable per-tool serial. */
            .x = x, .y = y,
            .pressure = p->pressure.valid && p->pressure.max > p->pressure.min ?
                fmax(0, fmin(1, (p->pressure.value - p->pressure.min) /
                                     (p->pressure.max - p->pressure.min))) : 0,
            .tilt_x = p->tilt_x.valid ? fmax(-90, fmin(90, p->tilt_x.value)) : 0,
            .tilt_y = p->tilt_y.valid ? fmax(-90, fmin(90, p->tilt_y.value)) : 0,
            .rotation = p->rotation.valid ? p->rotation.value : 0,
            .distance = axis_normalized(&p->distance),
            .tangential = p->tangential.valid && p->tangential.max > p->tangential.min ?
                fmax(-1, fmin(1, 2 * axis_normalized(&p->tangential) - 1)) : 0,
            .wheel_degrees = p->wheel.valid ? p->wheel.value : 0,
            .axes_valid = pen_axes_valid(p), .axes_changed = changed,
            .buttons = p->buttons, .changed_button = changed_button,
            .in_contact = (p->buttons & YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY)) != 0,
            .tool = p->device == YASL_PRIVATE_PTR_DEV_ERASER ? 2u : p->tool,
            .action = action,
        }}, NULL, 0);
}

static void xi_pointer_event(struct x11_surface *s, const XIDeviceEvent *ev)
{
    struct x11_display *d = (struct x11_display *)s->base.display;
    struct x11_pointer *p = pointer_for(d, ev->sourceid);
    struct yasl_private_event event = {
        .pointer = {
            .pos = {yasl_private_scale_to_device(ev->event_x, YASL_PRIVATE_SCALE_UNIT),
                    yasl_private_scale_to_device(ev->event_y, YASL_PRIVATE_SCALE_UNIT)},
            .device = p ? p->device : YASL_PRIVATE_PTR_DEV_MOUSE,
            .mods = modifiers(ev->mods.effective),
        },
    };
    uint32_t pen_changed = 0;
    if (p && (p->device == YASL_PRIVATE_PTR_DEV_PEN || p->device == YASL_PRIVATE_PTR_DEV_ERASER)) {
        pen_changed = update_axes(p, &ev->valuators);
        if (ev->evtype == XI_ButtonRelease && ev->detail == 1) {
            p->pressure.value = p->pressure.min;
            pen_changed |= p->pressure.valid ? YASL_PRIVATE_PEN_PRESSURE : 0;
        }
        if (ev->evtype == XI_ButtonPress || ev->evtype == XI_ButtonRelease) {
            enum yasl_private_pointer_button button = pointer_button(ev->detail);
            uint32_t mask = YASL_PRIVATE_BUTTON_BIT(button);
            if (ev->evtype == XI_ButtonPress) p->buttons |= mask;
            else p->buttons &= ~mask;
        }
        event.pointer.pen = (struct yasl_private_pointer_pen){
            pressure_value(&p->pressure), tilt_value(&p->tilt_x), tilt_value(&p->tilt_y)};
    }
    if (p) update_scroll(p, &ev->valuators, &event.pointer);
    if (ev->evtype == XI_Motion) event.type = YASL_PRIVATE_EVENT_POINTER_MOTION;
    else if (ev->detail >= 4 && ev->detail <= 7) {
        if (ev->evtype == XI_ButtonRelease) return;
        event.type = YASL_PRIVATE_EVENT_POINTER_SCROLL;
        if (ev->detail <= 5) event.pointer.scroll.y = ev->detail == 4 ? -1 : 1;
        else event.pointer.scroll.x = ev->detail == 6 ? -1 : 1;
        event.pointer.scroll_valid = 1u;
        event.pointer.scroll_source = p && p->finger_scroll ? 2u : 1u;
        event.pointer.scroll_phase = 2u;
    } else {
        event.type = ev->evtype == XI_ButtonPress ? YASL_PRIVATE_EVENT_POINTER_DOWN : YASL_PRIVATE_EVENT_POINTER_UP;
        event.pointer.button = pointer_button(ev->detail);
    }
    d->mods = event.pointer.mods;
    if (p && (p->device == YASL_PRIVATE_PTR_DEV_PEN || p->device == YASL_PRIVATE_PTR_DEV_ERASER) &&
        ev->detail != 4 && ev->detail != 5 && ev->detail != 6 && ev->detail != 7) {
        unsigned action = ev->evtype == XI_Motion ? YASL_PRIVATE_PEN_MOVE :
            ev->evtype == XI_ButtonPress ? (ev->detail == 1 ? YASL_PRIVATE_PEN_DOWN : YASL_PRIVATE_PEN_BUTTON) :
            (ev->detail == 1 ? YASL_PRIVATE_PEN_UP : YASL_PRIVATE_PEN_BUTTON);
        push_pen_sample(s, p, action, ev->event_x, ev->event_y, pen_changed,
                        ev->evtype == XI_Motion ? 0u : (unsigned)ev->detail);
    }
    push(s, event, NULL, 0);
    if (event.type == YASL_PRIVATE_EVENT_POINTER_MOTION && event.pointer.scroll_valid) {
        event.type = YASL_PRIVATE_EVENT_POINTER_SCROLL;
        event.pointer.pos = (struct yasl_private_vector2i){
            yasl_private_scale_to_device(ev->event_x, YASL_PRIVATE_SCALE_UNIT),
            yasl_private_scale_to_device(ev->event_y, YASL_PRIVATE_SCALE_UNIT)};
        push(s, event, NULL, 0);
    }
}

static void xi_event(struct x11_display *d, XGenericEventCookie *cookie)
{
    if (cookie->evtype == XI_ButtonPress || cookie->evtype == XI_ButtonRelease)
        note_time(d, ((const XIDeviceEvent *)cookie->data)->time);
    if (cookie->evtype == XI_HierarchyChanged || cookie->evtype == XI_DeviceChanged) {
        if (cookie->evtype == XI_DeviceChanged &&
            ((XIDeviceChangedEvent *)cookie->data)->reason == XISlaveSwitch) return;
        for (struct x11_pointer *p = d->pointers; p; p = p->next) {
            if (!p->buttons) continue;
            for (struct x11_surface *s = d->surfaces; s; s = s->next)
                push_pen_sample(s, p, YASL_PRIVATE_PEN_CANCEL, 0, 0, 0, 0);
        }
        clear_pointers(d);
        if (cookie->evtype == XI_HierarchyChanged &&
            (((XIHierarchyEvent *)cookie->data)->flags &
             (XISlaveRemoved | XIDeviceDisabled | XISlaveDetached | XIMasterRemoved))) {
            /* Removal/disable may interrupt a drag before a release arrives. */
            if (d->drag.active)
                drag_end(d, YASL_PRIVATE_DRAG_CANCELLED);
            for (struct x11_surface *s = d->surfaces; s; s = s->next)
                push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_INPUT_RESET}, NULL, 0);
        }
        return;
    }
    bool crossing = cookie->evtype == XI_Enter || cookie->evtype == XI_Leave;
    if (!crossing && cookie->evtype != XI_Motion && cookie->evtype != XI_ButtonPress &&
        cookie->evtype != XI_ButtonRelease) return;
    Window window = crossing ? ((XIEnterEvent *)cookie->data)->event :
                                ((XIDeviceEvent *)cookie->data)->event;
    struct x11_surface *s = d->surfaces;
    while (s && s->window != window) s = s->next;
    if (cookie->evtype == XI_ButtonPress && d->popup_grab) {
        const XIDeviceEvent *press = cookie->data;
        if (press->detail < 4 || press->detail > 7)  /* Wheel clicks are not presses. */
            popup_dismiss_outside(d, (int)round(press->root_x), (int)round(press->root_y));
    }
    if (cookie->evtype == XI_Motion && d->drag.active) {
        const XIDeviceEvent *mev = cookie->data;
        if (d->drag.carried)
            place(&d->drag.carried->base, d->drag.carry_offset);
        else if (d->drag.icon_window && d->native)
            XMoveWindow(d->native, d->drag.icon_window,
                        (int)round(mev->root_x) - d->drag.hotspot.x,
                        (int)round(mev->root_y) - d->drag.hotspot.y);
        struct yasl_private_vector2i hpos = {yasl_private_scale_to_device(mev->event_x, YASL_PRIVATE_SCALE_UNIT),
                                     yasl_private_scale_to_device(mev->event_y, YASL_PRIVATE_SCALE_UNIT)};
        struct x11_surface *hsurf = d->native ?
            surface_at_root_pos(d, (int)round(mev->root_x), (int)round(mev->root_y), &hpos) : s;
        drag_set_hover(d, hsurf, hpos);
    } else if (cookie->evtype == XI_Enter && d->drag.active) {
        const XIEnterEvent *eev = cookie->data;
        struct yasl_private_vector2i hpos = {yasl_private_scale_to_device(eev->event_x, YASL_PRIVATE_SCALE_UNIT),
                                     yasl_private_scale_to_device(eev->event_y, YASL_PRIVATE_SCALE_UNIT)};
        struct x11_surface *hsurf = d->native ?
            surface_at_root_pos(d, (int)round(eev->root_x), (int)round(eev->root_y), &hpos) : s;
        drag_set_hover(d, hsurf, hpos);
    } else if (cookie->evtype == XI_Leave && d->drag.active) {
        const XIEnterEvent *lev = cookie->data;
        struct yasl_private_vector2i lpos = {yasl_private_scale_to_device(lev->event_x, YASL_PRIVATE_SCALE_UNIT),
                                     yasl_private_scale_to_device(lev->event_y, YASL_PRIVATE_SCALE_UNIT)};
        drag_leave_hover(d, s, lpos);
    } else if (cookie->evtype == XI_ButtonRelease && d->drag.active) {
        drag_drop_hover(d);
    }
    if (!s || s->destroyed) return;
    if (!crossing) xi_pointer_event(s, cookie->data);
    else {
        const XIEnterEvent *ev = cookie->data;
        struct x11_pointer *p = pointer_for(d, ev->sourceid);
        if (p && cookie->evtype == XI_Leave) {
            p->pressure.value = p->pressure.min;
            if (p->pressure.valid) push_pen_sample(s, p, YASL_PRIVATE_PEN_LEAVE,
                    ev->event_x, ev->event_y, YASL_PRIVATE_PEN_PRESSURE, 0);
        } else if (p && (p->device == YASL_PRIVATE_PTR_DEV_PEN || p->device == YASL_PRIVATE_PTR_DEV_ERASER)) {
            push_pen_sample(s, p, YASL_PRIVATE_PEN_ENTER, ev->event_x, ev->event_y, 0, 0);
        }
        push(s, (struct yasl_private_event){
            .type = cookie->evtype == XI_Leave ? YASL_PRIVATE_EVENT_POINTER_LEAVE : YASL_PRIVATE_EVENT_POINTER_MOTION,
            .pointer = {
                .pos = {yasl_private_scale_to_device(ev->event_x, YASL_PRIVATE_SCALE_UNIT),
                        yasl_private_scale_to_device(ev->event_y, YASL_PRIVATE_SCALE_UNIT)},
                .device = p ? p->device : YASL_PRIVATE_PTR_DEV_MOUSE,
                .mods = modifiers(ev->mods.effective),
            },
        }, NULL, 0);
    }
}

/* A shared image is written in place, so only 32-bit TrueColor 0xAARRGGBB layouts
 * qualify; other visuals take the converting XPutImage path. */
static bool shm_format_direct(const XImage *image, const Visual *visual)
{
    return visual->class == TrueColor && image->bits_per_pixel == 32 &&
           image->red_mask == 0xff0000 && image->green_mask == 0xff00 && image->blue_mask == 0xff;
}

/* XShmAttach only queues the request, so the server's refusal arrives later as an
 * error. Sync around the attach to catch it here; on failure, detach and stop using SHM.
 * The sync after the detach consumes its error while the probe is disarmed. */
static bool shm_attach(struct x11_display *d, XShmSegmentInfo *info)
{
    XSync(d->native, False);
    shm_probe = d->native;
    shm_probe_failed = false;
    bool queued = XShmAttach(d->native, info);
    XSync(d->native, False);
    shm_probe = NULL;
    if (queued && !shm_probe_failed) return true;
    XShmDetach(d->native, info);
    XSync(d->native, False);
    d->has_shm = false;
    return false;
}

static bool resize(struct x11_surface *s, struct yasl_private_vector2i size)
{
    if (size.x <= 0 || size.y <= 0 || size.x > 32767 || size.y > 32767 ||
        (size_t)size.x > SIZE_MAX / sizeof(uint32_t) / (size_t)size.y)
        return false;
    if (s->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
        s->size = size;
        s->base.size = size;
        return true;
    }
    if (s->image && size.x == s->size.x && size.y == s->size.y) return true;
    struct x11_display *d = (struct x11_display *)s->base.display;
    if (!d->native) {
        s->size = size;
        s->want_draw = true;
        return true;
    }
    uint64_t start = yasl_private_loop_now_ns();
    /* The presented frame goes with the old pixels. */
    s->presented = false;

    if (s->shm_active) {
        XShmDetach(d->native, &s->shm_info);
        if (s->image) XDestroyImage(s->image);
        if (s->shm_info.shmaddr) shmdt(s->shm_info.shmaddr);
        s->image = NULL;
        s->pixels = NULL;
        s->shm_active = false;
    } else {
        if (s->image) XDestroyImage(s->image);
        free(s->pixels);
        s->image = NULL;
        s->pixels = NULL;
    }

    XImage *image = NULL;
    uint32_t *pixels = NULL;
    bool shm_ok = false;
    if (d->has_shm) {
        size_t bytes = (size_t)size.x * (size_t)size.y * sizeof(uint32_t);
        s->shm_info.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (s->shm_info.shmid >= 0) {
            s->shm_info.shmaddr = (char *)shmat(s->shm_info.shmid, NULL, 0);
            if (s->shm_info.shmaddr != (char *)-1) {
                s->shm_info.readOnly = False;
                image = XShmCreateImage(d->native, s->visual,
                        (unsigned)s->visual_depth, ZPixmap,
                        s->shm_info.shmaddr, &s->shm_info,
                        (unsigned)size.x, (unsigned)size.y);
                if (image) {
                    shmctl(s->shm_info.shmid, IPC_RMID, NULL);
                    if (shm_format_direct(image, s->visual) &&
                            shm_attach(d, &s->shm_info)) {
                        pixels = (uint32_t *)s->shm_info.shmaddr;
                        memset(pixels, 0, bytes);
                        shm_ok = true;
                    } else {
                        XDestroyImage(image);
                        image = NULL;
                        shmdt(s->shm_info.shmaddr);
                        s->shm_info.shmaddr = NULL;
                    }
                } else {
                    shmdt(s->shm_info.shmaddr);
                    s->shm_info.shmaddr = NULL;
                    shmctl(s->shm_info.shmid, IPC_RMID, NULL);
                }
            } else {
                s->shm_info.shmaddr = NULL;
                shmctl(s->shm_info.shmid, IPC_RMID, NULL);
            }
        }
    }
    if (!shm_ok) {
        image = XCreateImage(d->native, s->visual,
                (unsigned)s->visual_depth, ZPixmap, 0, NULL,
                (unsigned)size.x, (unsigned)size.y, 32, 0);
        pixels = calloc((size_t)size.x * (size_t)size.y, sizeof(*pixels));
        if (!image || !pixels) {
            if (image) XDestroyImage(image);
            free(pixels);
            return false;
        }
        image->data = calloc((size_t)image->bytes_per_line, (size_t)size.y);
        if (!image->data) {
            XDestroyImage(image);
            free(pixels);
            return false;
        }
    }
    s->shm_active = shm_ok;
    s->image = image;
    s->pixels = pixels;
    s->size = size;
    s->base.size = size;
    s->base.scale_120 = d->scale_120;
    s->want_draw = true;
    yasl_private_clock_note_resize(&s->clock, yasl_private_loop_now_ns() - start);
    return true;
}

static void cancel_pastes(struct x11_display *d, struct x11_surface *target)
{
    for (struct paste_request **p = &d->pastes; *p;) {
        struct paste_request *r = *p;
        if (target && r->target == target && !r->incr) {
            /* Keep the requestor window alive until the selection owner replies.
             * Some INCR owners write to it after the original surface is gone. */
            r->target = NULL;
            r->id = 0;
            p = &r->next;
        } else if (!target || r->target == target) {
            /* An INCR transfer in progress is abandoned; its owner times out. */
            *p = r->next;
            if (r->timeout) yasl_loop_del_timer(d->loop, r->timeout);
            if (!d->lost) XDestroyWindow(d->native, r->window);
            free(r->buf);
            free(r);
        } else {
            p = &r->next;
        }
    }
}

static uint64_t paste_timeout(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    struct paste_request *r = ud;
    struct x11_surface *s = r->target;
    struct x11_display *d = r->display;
    struct paste_request **p = &d->pastes;
    while (*p && *p != r) p = &(*p)->next;
    if (*p) *p = r->next;
    r->timeout = 0; /* The loop retires this one-shot timer after the callback. */
    XDestroyWindow(d->native, r->window);
    if (s && !s->destroyed)
        yasl_private_surface_complete_paste(&s->base, r->id, YASL_PRIVATE_TRANSFER_FAILED, NULL, 0);
    free(r->buf);
    free(r);
    return 0;
}

/* Restart the stall timeout; an INCR transfer gets a fresh allowance per chunk. */
static void paste_rearm(struct paste_request *r)
{
    struct x11_display *d = r->display;
    if (r->timeout) yasl_loop_del_timer(d->loop, r->timeout);
    r->timeout = yasl_loop_add_timer(d->loop, PASTE_TIMEOUT_NS, paste_timeout, r);
}

/* Unlink a paste request at *link and report its outcome to the surface, if alive. */
static void paste_complete(struct x11_display *d, struct paste_request **link,
        enum yasl_private_transfer_status status, const char *text, size_t len)
{
    struct paste_request *r = *link;
    *link = r->next;
    if (r->timeout) yasl_loop_del_timer(d->loop, r->timeout);
    XDestroyWindow(d->native, r->window);
    if (r->target && !r->target->destroyed)
        yasl_private_surface_complete_paste(&r->target->base, r->id, status, text, len);
    free(r->buf);
    free(r);
}

static void surface_destroy(struct yasl_surface *base)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (s->destroyed) return;
    struct x11_display *d = (struct x11_display *)base->display;
    if (d->drag.active) {
        if (d->drag.origin == s || d->drag.carried == s)
            drag_end(d, YASL_PRIVATE_DRAG_CANCELLED);
        else if (d->drag.hover == s)
            d->drag.hover = NULL;
    }
    if (d->xdnd.surface == s)
        d->xdnd.surface = NULL;
    if (d->pending_drop.surface == s) d->pending_drop.surface = NULL;
    for (struct x11_surface *p = d->surfaces; p; p = p->next) {
        if (p->parent == s) p->parent = NULL;
    }
    s->destroyed = true;
    for (struct x11_surface **p = &d->surfaces; *p; p = &(*p)->next) {
        if (*p == s) { *p = s->next; break; }
    }
    if (s->is_popup) popup_grab_update(d);
    cancel_pastes(d, s);
    free(s->ime_preedit);
    s->ime_preedit = NULL;
    if (s->ic) XDestroyIC(s->ic);
#if YASL_HAS_EGL
    if (s->gl_context && d->egl_ready) eglDestroyContext(d->egl, s->gl_context);
    if (s->gl_surface && d->egl_ready) eglDestroySurface(d->egl, s->gl_surface);
#endif
    if (s->shm_active) {
        if (!d->lost) XShmDetach(d->native, &s->shm_info);
        if (s->image) XDestroyImage(s->image);
        if (s->shm_info.shmaddr) shmdt(s->shm_info.shmaddr);
        s->image = NULL;
        s->pixels = NULL;
        s->shm_active = false;
    } else {
        if (s->image) XDestroyImage(s->image);
        free(s->pixels);
        s->image = NULL;
        s->pixels = NULL;
    }
    if (d->native && !d->lost) {
        if (s->gc) XFreeGC(d->native, s->gc);
        if (s->window) XDestroyWindow(d->native, s->window);
        if (s->colormap) XFreeColormap(d->native, s->colormap);
    }
    input_queue_clear(&s->input);
    if (d->depth) {
        s->dead_next = d->dead;
        d->dead = s;
    } else free(s);
}

static void set_callbacks(struct yasl_surface *base,
        const struct yasl_private_window_cbs *cbs, void *ud)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (s->destroyed) return;
    s->base.callbacks = cbs ? *cbs : (struct yasl_private_window_cbs){0};
    s->cbs = &s->base.callbacks;
    s->base.ud = ud;
    s->ud = ud;
}

static void set_title(struct yasl_surface *base, const char *title)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (s->destroyed || !title) return;
    struct x11_display *d = (struct x11_display *)base->display;
    XStoreName(d->native, s->window, title);
    XChangeProperty(d->native, s->window, d->net_name, d->utf8, 8,
                    PropModeReplace, (const unsigned char *)title, (int)strlen(title));
}

static bool net_message(struct x11_surface *s, Atom type,
                        long a, long b, long c, long e, long f)
{
    struct x11_display *d = (struct x11_display *)s->base.display;
    XEvent event = {0};
    event.xclient = (XClientMessageEvent){
        .type = ClientMessage, .display = d->native, .window = s->window,
        .message_type = type, .format = 32, .data.l = {a, b, c, e, f},
    };
    return XSendEvent(d->native, RootWindow(d->native, d->screen), False,
                      SubstructureRedirectMask | SubstructureNotifyMask, &event) != 0;
}

/* A fixed-size window tells the window manager that its size is exact. */
static void set_fixed_size_hints(struct x11_display *d, Window window, struct yasl_private_vector2i size)
{
    XSizeHints size_hints = {.flags = PMinSize | PMaxSize,
        .min_width = size.x, .max_width = size.x,
        .min_height = size.y, .max_height = size.y};
    XSetWMNormalHints(d->native, window, &size_hints);
}

static bool surface_set_size(struct yasl_surface *base, struct yasl_private_vector2i size)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    /* The request is logical; the window is sized in device pixels. */
    struct yasl_private_vector2i px = {yasl_private_scale_to_device(size.x, s->base.scale_120),
                                       yasl_private_scale_to_device(size.y, s->base.scale_120)};
    if (s->destroyed || s->is_popup || px.x <= 0 || px.y <= 0 ||
        px.x > 32767 || px.y > 32767) {
        errno = EINVAL;
        return false;
    }
    if (!d->native || !s->window) { errno = ENODEV; return false; }
    XResizeWindow(d->native, s->window, (unsigned)px.x, (unsigned)px.y);
    if (s->base.creation_flags & YASL_PRIVATE_SURFACE_FIXED_SIZE)
        set_fixed_size_hints(d, s->window, px);
    XFlush(d->native);
    return true;
}

static bool surface_set_fullscreen(struct yasl_surface *base, bool enabled)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || s->is_popup) { errno = EINVAL; return false; }
    if (!d->native || !s->window) { errno = ENODEV; return false; }
    if (!net_message(s, d->net_state, enabled ? 1 : 0, d->fullscreen, 0, 1, 0)) {
        errno = ENOTSUP;
        return false;
    }
    XFlush(d->native);
    return true;
}

static bool surface_set_maximized(struct yasl_surface *base, bool enabled)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || s->is_popup) { errno = EINVAL; return false; }
    if (!d->native || !s->window) { errno = ENODEV; return false; }
    long action = enabled ? 1 : 0;
    if (!net_message(s, d->net_state, action, d->maximized_v, d->maximized_h, 1, 0)) {
        errno = ENOTSUP;
        return false;
    }
    XFlush(d->native);
    return true;
}

static bool surface_minimize(struct yasl_surface *base)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || s->is_popup) { errno = EINVAL; return false; }
    if (!d->native || !s->window) { errno = ENODEV; return false; }
    if (!XIconifyWindow(d->native, s->window, d->screen)) { errno = ENOTSUP; return false; }
    XFlush(d->native);
    return true;
}

static size_t utf8_offset_for_chars(const char *text, size_t len, int chars)
{
    size_t at = 0;
    while (at < len && chars-- > 0) {
        unsigned char c = (unsigned char)text[at];
        size_t step = c < 0x80 ? 1 : (c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4));
        if (step > len - at) step = 1;
        at += step;
    }
    return at;
}

static size_t utf8_encode(char *out, uint32_t cp)
{
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static char *xim_text_dup(const XIMText *text, size_t *len)
{
    *len = 0;
    if (!text) return NULL;
    const void *source = text->encoding_is_wchar
            ? (const void *)text->string.wide_char
            : (const void *)text->string.multi_byte;
    if (!source) return NULL;
    /* XIMText.length is an unsigned short, so four bytes per scalar cannot
     * overflow size_t on the supported X11 targets. */
    size_t cap = (size_t)text->length * 4 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t used = 0;
    if (text->encoding_is_wchar) {
        const wchar_t *src = text->string.wide_char;
        for (unsigned i = 0; i < text->length; i++) {
            uint32_t cp = (uint32_t)src[i];
            used += utf8_encode(out + used, cp);
        }
    } else {
        const char *src = text->string.multi_byte;
        mbstate_t state = {0};
        size_t consumed_bytes = 0;
        size_t max_bytes = (size_t)text->length * MB_LEN_MAX;
        unsigned converted = 0;
        while (converted < text->length && consumed_bytes < max_bytes) {
            wchar_t wc = 0;
            size_t result = mbrtowc(&wc, src + consumed_bytes, 1, &state);
            if (result == (size_t)-1) { free(out); return NULL; }
            consumed_bytes++;
            if (result == (size_t)-2) continue;
            if (result == 0) {
                /* The public UTF-8 event payload cannot contain an embedded NUL. */
                wc = (wchar_t)0xfffd;
                state = (mbstate_t){0};
            }
            used += utf8_encode(out + used, (uint32_t)wc);
            converted++;
        }
        if (converted != text->length) { free(out); return NULL; }
    }
    out[used] = '\0';
    if (!yasl_private_utf8_validate(out, used)) { free(out); return NULL; }
    *len = used;
    return out;
}

static void xim_preedit_done(XIM im, XPointer client_data, XPointer call_data)
{
    (void)im;
    (void)call_data;
    struct x11_surface *s = (struct x11_surface *)client_data;
    if (s->destroyed) return;
    free(s->ime_preedit);
    s->ime_preedit = NULL;
    s->ime_cursor_begin = s->ime_cursor_end = -1;
    yasl_private_surface_notify_composition(&s->base, NULL, -1, -1);
}

static int xim_preedit_start(XIM im, XPointer client_data, XPointer call_data)
{
    (void)im;
    (void)client_data;
    (void)call_data;
    return -1; /* Accept preedit strings of any length. */
}

static void xim_preedit_draw(XIM im, XPointer client_data, XPointer call_data)
{
    (void)im;
    struct x11_surface *s = (struct x11_surface *)client_data;
    XIMPreeditDrawCallbackStruct *draw = (XIMPreeditDrawCallbackStruct *)call_data;
    if (s->destroyed || !draw || draw->chg_first < 0 || draw->chg_length < 0) return;
    size_t old_len = s->ime_preedit ? strlen(s->ime_preedit) : 0;
    size_t new_len = old_len;
    bool has_string = draw->text && (draw->text->encoding_is_wchar
            ? draw->text->string.wide_char != NULL
            : draw->text->string.multi_byte != NULL);
    if (has_string) {
        const char *old_text = s->ime_preedit ? s->ime_preedit : "";
        size_t first = utf8_offset_for_chars(old_text, old_len, draw->chg_first);
        size_t after = first + utf8_offset_for_chars(old_text + first,
                old_len - first, draw->chg_length);
        size_t insert_len = 0;
        char *insert = xim_text_dup(draw->text, &insert_len);
        if (!insert) return;
        if (insert_len > SIZE_MAX - (old_len - (after - first)) - 1) {
            free(insert);
            return;
        }
        new_len = old_len - (after - first) + insert_len;
        char *updated = malloc(new_len + 1);
        if (!updated) { free(insert); return; }
        if (first) memcpy(updated, old_text, first);
        if (insert_len) memcpy(updated + first, insert, insert_len);
        if (old_len > after) memcpy(updated + first + insert_len, old_text + after,
                                     old_len - after);
        updated[new_len] = '\0';
        free(insert);
        free(s->ime_preedit);
        s->ime_preedit = new_len ? updated : (free(updated), NULL);
    }
    int caret = draw->caret < 0 ? -1 : (int)utf8_offset_for_chars(
            s->ime_preedit ? s->ime_preedit : "", new_len, draw->caret);
    s->ime_cursor_begin = s->ime_cursor_end = caret;
    yasl_private_surface_notify_composition(&s->base, s->ime_preedit, caret, caret);
}

static void xim_preedit_caret(XIM im, XPointer client_data, XPointer call_data)
{
    (void)im;
    struct x11_surface *s = (struct x11_surface *)client_data;
    XIMPreeditCaretCallbackStruct *caret = (XIMPreeditCaretCallbackStruct *)call_data;
    if (s->destroyed || !caret || !s->ime_preedit) return;
    int offset = caret->position < 0 ? -1 : (int)utf8_offset_for_chars(
            s->ime_preedit, strlen(s->ime_preedit), caret->position);
    s->ime_cursor_begin = s->ime_cursor_end = offset;
    yasl_private_surface_notify_composition(&s->base, s->ime_preedit, offset, offset);
}

static XIC create_ic(struct x11_display *d, struct x11_surface *s)
{
    if (!d->im) return NULL;
    union {
        int (*start)(XIM, XPointer, XPointer);
        XIMProc proc;
    } start_proc = {.start = xim_preedit_start};
    XIMCallback start = {.client_data = (XPointer)s,
        .callback = start_proc.proc};
    XIMCallback done = {.client_data = (XPointer)s, .callback = xim_preedit_done};
    XIMCallback draw = {.client_data = (XPointer)s, .callback = xim_preedit_draw};
    XIMCallback caret = {.client_data = (XPointer)s, .callback = xim_preedit_caret};
    XVaNestedList preedit = XVaCreateNestedList(0,
            XNPreeditStartCallback, &start,
            XNPreeditDoneCallback, &done,
            XNPreeditDrawCallback, &draw,
            XNPreeditCaretCallback, &caret, NULL);
    XIC ic = preedit ? XCreateIC(d->im,
            XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
            XNPreeditAttributes, preedit,
            XNClientWindow, s->window, XNFocusWindow, s->window, NULL) : NULL;
    if (preedit) XFree(preedit);
    if (!ic)
        ic = XCreateIC(d->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                XNClientWindow, s->window, XNFocusWindow, s->window, NULL);
    return ic;
}

static struct yasl_surface *create_toplevel_visual(struct yasl_private_display *base,
        const struct yasl_private_window_spec *spec, Visual *visual, int depth,
        enum yasl_private_presenter presenter)
{
    if (!spec) return NULL;
    struct x11_display *d = (struct x11_display *)base;
    struct x11_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->base.display = base;
    s->base.creation_flags = spec->flags | (spec->external_gpu ? 0u : YASL_PRIVATE_SURFACE_CPU);
    s->base.presenter = presenter;
    s->clock.refresh_ns = YASL_PRIVATE_REFRESH_DEFAULT_NS;
    /* Logical creation sizes become device pixels. */
    struct yasl_private_vector2i px = {yasl_private_scale_to_device(spec->size.x, d->scale_120),
                                       yasl_private_scale_to_device(spec->size.y, d->scale_120)};
    if (!visual) visual = DefaultVisual(d->native, d->screen);
    if (!depth) depth = DefaultDepth(d->native, d->screen);
    s->visual = visual;
    s->visual_depth = depth;
    if (!resize(s, px)) { free(s); return NULL; }
    s->colormap = XCreateColormap(d->native, RootWindow(d->native, d->screen), visual, AllocNone);
    XSetWindowAttributes attrs = {.colormap = s->colormap, .border_pixel = 0, .background_pixel = 0};
    s->window = XCreateWindow(d->native, RootWindow(d->native, d->screen), 0, 0,
            (unsigned)px.x, (unsigned)px.y, 0, depth, InputOutput,
            visual, CWBorderPixel | CWBackPixel | CWColormap, &attrs);
    if (!s->window) {
        XFreeColormap(d->native, s->colormap);
        free(s);
        errno = ENODEV;
        return NULL;
    }
    s->base.native_handle = (void *)(uintptr_t)s->window;
    s->base.size = s->size;
    s->base.scale_120 = d->scale_120;
    s->gc = XCreateGC(d->native, s->window, 0, NULL);
    XSelectInput(d->native, s->window, ExposureMask | StructureNotifyMask |
            FocusChangeMask | KeyPressMask | KeyReleaseMask | PropertyChangeMask |
            (d->xi2 ? 0 : ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | LeaveWindowMask | EnterWindowMask));
    if (d->xi2) {
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
        XISetMask(bits, XI_Motion);
        XISetMask(bits, XI_ButtonPress);
        XISetMask(bits, XI_ButtonRelease);
        XISetMask(bits, XI_Enter);
        XISetMask(bits, XI_Leave);
        XIEventMask mask = {XIAllMasterDevices, sizeof(bits), bits};
        XISelectEvents(d->native, s->window, &mask, 1);
    }
    XSetWMProtocols(d->native, s->window, &d->delete_window, 1);
    if (spec->flags & YASL_PRIVATE_SURFACE_UNDECORATED) {
        /* MWM_HINTS_DECORATIONS: only the decorations field is set, so the
         * functions stay enabled. */
        struct { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } hints = {
            .flags = 2, .decorations = 0,
        };
        XChangeProperty(d->native, s->window, d->motif_hints, d->motif_hints, 32,
                        PropModeReplace, (unsigned char *)&hints, 5);
    }
    if (spec->flags & YASL_PRIVATE_SURFACE_FIXED_SIZE) set_fixed_size_hints(d, s->window, px);
    XClassHint hint = { .res_name = d->app_id, .res_class = d->app_id };
    XSetClassHint(d->native, s->window, &hint);
    Atom xdnd_version = 5;
    XChangeProperty(d->native, s->window, d->xdnd_aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&xdnd_version, 1);
    s->ic = create_ic(d, s);
    s->next = d->surfaces;
    d->surfaces = s;
    set_title(&s->base, spec->title ? spec->title : "YASL");
    if (spec->fullscreen) {
        XChangeProperty(d->native, s->window, d->net_state, XA_ATOM, 32,
                PropModeReplace, (unsigned char *)&d->fullscreen, 1);
        s->base.state |= YASL_PRIVATE_STATE_FULLSCREEN;
    }
    if (!(spec->flags & YASL_PRIVATE_SURFACE_HIDDEN)) XMapWindow(d->native, s->window);
    XFlush(d->native);
    return &s->base;
}

static struct yasl_surface *create_toplevel(struct yasl_private_display *base,
        const struct yasl_private_window_spec *spec, void *graphics_device)
{
    if (!spec) return NULL;
    if (graphics_device) { errno = ENOTSUP; return NULL; }
    struct x11_display *d = (struct x11_display *)base;
    /* CPU windows keep alpha when the server has a 32-bit visual. */
    Visual *visual = !spec->external_gpu && d->argb_visual ? d->argb_visual : NULL;
    int depth = visual ? d->argb_depth : 0;
    return create_toplevel_visual(base, spec, visual, depth,
        spec->external_gpu ? YASL_PRIVATE_PRESENTER_NONE : YASL_PRIVATE_PRESENTER_CPU);
}

#if YASL_HAS_EGL
static bool egl_open(struct x11_display *d)
{
    if (d->egl_ready) return true;
    d->egl = eglGetDisplay((EGLNativeDisplayType)d->native);
    EGLint major = 0, minor = 0;
    if (d->egl == EGL_NO_DISPLAY || !eglInitialize(d->egl, &major, &minor)) {
        errno = ENOTSUP;
        return false;
    }
    d->egl_ready = true;
    return true;
}

static struct yasl_surface *create_gl_surface(struct yasl_private_display *base,
        const struct yasl_private_window_spec *spec, const int *attrs,
        struct yasl_surface *share)
{
    if (!spec) { errno = EINVAL; return NULL; }
    struct x11_display *d = (struct x11_display *)base;
    struct yasl_private_gl_options options;
    if (!yasl_private_gl_parse_options(attrs, &options)) { errno = EINVAL; return NULL; }
    if (!egl_open(d)) return NULL;
    if (options.srgb) {
        const char *extensions = eglQueryString(d->egl, EGL_EXTENSIONS);
        if (!extensions || !strstr(extensions, "EGL_KHR_gl_colorspace")) {
            errno = ENOTSUP;
            return NULL;
        }
    }

    EGLint renderable = options.api == YASL_PRIVATE_GL_OPENGL_ES ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT;
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, renderable,
        EGL_RED_SIZE, options.red_bits,
        EGL_GREEN_SIZE, options.green_bits,
        EGL_BLUE_SIZE, options.blue_bits,
        EGL_ALPHA_SIZE, options.alpha_bits,
        EGL_DEPTH_SIZE, options.depth_bits,
        EGL_STENCIL_SIZE, options.stencil_bits,
        EGL_SAMPLE_BUFFERS, options.samples > 0 ? 1 : 0,
        EGL_SAMPLES, options.samples,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint count = 0;
    if (!eglChooseConfig(d->egl, config_attrs, &config, 1, &count) || !count) {
        errno = ENOTSUP;
        return NULL;
    }
    EGLint visual_id = 0, depth = 0;
    if (!eglGetConfigAttrib(d->egl, config, EGL_NATIVE_VISUAL_ID, &visual_id) ||
        !eglGetConfigAttrib(d->egl, config, EGL_BUFFER_SIZE, &depth)) {
        errno = ENODEV;
        return NULL;
    }
    XVisualInfo visual_query = {.visualid = (VisualID)visual_id};
    int visual_count = 0;
    XVisualInfo *visual_info = XGetVisualInfo(d->native, VisualIDMask, &visual_query, &visual_count);
    if (!visual_info || !visual_count) {
        if (visual_info) XFree(visual_info);
        errno = ENODEV;
        return NULL;
    }
    Visual *visual = visual_info[0].visual;
    depth = visual_info[0].depth;
    struct yasl_private_window_spec gl_spec = *spec;
    gl_spec.external_gpu = true;
    gl_spec.flags &= ~YASL_PRIVATE_SURFACE_CPU;
    struct yasl_surface *base_surface = create_toplevel_visual(base, &gl_spec, visual, depth,
                                                YASL_PRIVATE_PRESENTER_GL);
    XFree(visual_info);
    if (!base_surface) return NULL;
    struct x11_surface *s = (struct x11_surface *)base_surface;
    s->gl_config = config;
    s->swap_interval = 1; /* EGL's default for window surfaces. */
    EGLint surface_attrs[] = {EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR, EGL_NONE};
    s->gl_surface = eglCreateWindowSurface(d->egl, config,
                    (EGLNativeWindowType)s->window, options.srgb ? surface_attrs : NULL);
    if (s->gl_surface == EGL_NO_SURFACE) {
        surface_destroy(base_surface);
        errno = ENODEV;
        return NULL;
    }
    if (options.api == YASL_PRIVATE_GL_OPENGL) {
        if (!eglBindAPI(EGL_OPENGL_API)) { surface_destroy(base_surface); errno = ENOTSUP; return NULL; }
    } else if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        surface_destroy(base_surface); errno = ENOTSUP; return NULL;
    }
    EGLContext share_context = EGL_NO_CONTEXT;
    if (share) {
        struct x11_surface *shared = (struct x11_surface *)share;
        if (share->display != base || shared->base.presenter != YASL_PRIVATE_PRESENTER_GL ||
            shared->gl_context == EGL_NO_CONTEXT) {
            surface_destroy(base_surface); errno = EINVAL; return NULL;
        }
        share_context = shared->gl_context;
    }
    EGLint context_attrs[12];
    EGLint n = 0;
    context_attrs[n++] = EGL_CONTEXT_MAJOR_VERSION;
    context_attrs[n++] = options.major;
    context_attrs[n++] = EGL_CONTEXT_MINOR_VERSION;
    context_attrs[n++] = options.minor;
    if (options.api == YASL_PRIVATE_GL_OPENGL && options.profile != YASL_PRIVATE_GL_ANY) {
        context_attrs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
        context_attrs[n++] = options.profile == YASL_PRIVATE_GL_CORE ?
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR;
    }
    EGLint context_flags = (options.debug ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0) |
        (options.forward_compat ? EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR : 0);
    if (context_flags) {
        context_attrs[n++] = EGL_CONTEXT_FLAGS_KHR;
        context_attrs[n++] = context_flags;
    }
    context_attrs[n] = EGL_NONE;
    s->gl_context = eglCreateContext(d->egl, config, share_context, context_attrs);
    if (s->gl_context == EGL_NO_CONTEXT) {
        surface_destroy(base_surface);
        errno = ENOTSUP;
        return NULL;
    }
    return base_surface;
}

static bool gl_make_current(struct yasl_private_display *base, struct yasl_surface *surface)
{
    struct x11_display *d = (struct x11_display *)base;
    if (!surface) /* Without EGL there is no context to detach. */
        return !d->egl_ready || eglMakeCurrent(d->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
    if (!d->egl_ready) { errno = ENOTSUP; return false; }
    struct x11_surface *s = (struct x11_surface *)surface;
    if (surface->display != base || s->base.presenter != YASL_PRIVATE_PRESENTER_GL || !s->gl_context) {
        errno = EINVAL;
        return false;
    }
    if (eglMakeCurrent(d->egl, s->gl_surface, s->gl_surface, s->gl_context) != EGL_TRUE) return false;
    eglSwapInterval(d->egl, s->swap_interval);
    return true;
}

static yasl_private_gl_proc gl_get_proc_address(struct yasl_private_display *base, const char *name)
{
    (void)base;
    return name ? (yasl_private_gl_proc)eglGetProcAddress(name) : NULL;
}

static bool gl_swap_interval(struct yasl_private_display *base, struct yasl_surface *surface, int interval)
{
    struct x11_display *d = (struct x11_display *)base;
    if (!surface || surface->display != base || ((struct x11_surface *)surface)->base.presenter != YASL_PRIVATE_PRESENTER_GL) {
        errno = EINVAL;
        return false;
    }
    struct x11_surface *s = (struct x11_surface *)surface;
    s->swap_interval = interval;
    /* EGL applies the interval to the calling thread's draw surface, so apply it now
     * only when this surface is current here; otherwise gl_make_current applies it. */
    if (eglGetCurrentSurface(EGL_DRAW) != s->gl_surface) return true;
    return eglSwapInterval(d->egl, interval) == EGL_TRUE;
}

static bool gl_swap_buffers(struct yasl_private_display *base, struct yasl_surface *surface)
{
    struct x11_display *d = (struct x11_display *)base;
    struct x11_surface *s = (struct x11_surface *)surface;
    if (!surface || surface->display != base || s->base.presenter != YASL_PRIVATE_PRESENTER_GL || !d->egl_ready) {
        errno = EINVAL;
        return false;
    }
    if (!eglSwapBuffers(d->egl, s->gl_surface)) return false;
    s->presented = true;
    yasl_private_clock_presented(&s->clock, yasl_private_loop_now_ns(), false);
    return true;
}
#endif

static bool popup_spec_valid(const struct x11_surface *parent,
                             const struct yasl_private_popup_spec *spec)
{
    if (!parent || !spec) return false;
    if (spec->anchor_edges > 8 || spec->gravity > 8 ||
            (spec->constraints & ~63u)) return false;
    int parent_cx = parent->is_popup ? parent->content_offset.x : 0;
    int parent_cy = parent->is_popup ? parent->content_offset.y : 0;
    int64_t x = (int64_t)spec->anchor.x - parent_cx;
    int64_t y = (int64_t)spec->anchor.y - parent_cy;
    return x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767 &&
           spec->anchor.width >= 0 && spec->anchor.height >= 0 &&
           spec->size.x > 0 && spec->size.y > 0 &&
           spec->size.x <= 32767 && spec->size.y <= 32767 &&
           spec->content_offset.x >= 0 && spec->content_offset.y >= 0 &&
           spec->content_size.x >= 0 && spec->content_size.y >= 0 &&
           (int64_t)spec->content_offset.x + spec->content_size.x <= spec->size.x &&
           (int64_t)spec->content_offset.y + spec->content_size.y <= spec->size.y;
}

enum { X11_ANCHOR_NONE, X11_ANCHOR_TOP, X11_ANCHOR_BOTTOM, X11_ANCHOR_LEFT,
       X11_ANCHOR_RIGHT, X11_ANCHOR_TOP_LEFT, X11_ANCHOR_BOTTOM_LEFT,
       X11_ANCHOR_TOP_RIGHT, X11_ANCHOR_BOTTOM_RIGHT };
enum { X11_CONSTRAINT_SLIDE_X = 1u, X11_CONSTRAINT_SLIDE_Y = 2u,
       X11_CONSTRAINT_FLIP_X = 4u, X11_CONSTRAINT_FLIP_Y = 8u,
       X11_CONSTRAINT_RESIZE_X = 16u, X11_CONSTRAINT_RESIZE_Y = 32u };

static void popup_anchor_point(const struct yasl_private_popup_spec *spec, int *x, int *y)
{
    switch (spec->anchor_edges) {
    case X11_ANCHOR_TOP: *x = spec->anchor.x + spec->anchor.width / 2; *y = spec->anchor.y; break;
    case X11_ANCHOR_BOTTOM: *x = spec->anchor.x + spec->anchor.width / 2; *y = spec->anchor.y + spec->anchor.height; break;
    case X11_ANCHOR_LEFT: *x = spec->anchor.x; *y = spec->anchor.y + spec->anchor.height / 2; break;
    case X11_ANCHOR_RIGHT: *x = spec->anchor.x + spec->anchor.width; *y = spec->anchor.y + spec->anchor.height / 2; break;
    case X11_ANCHOR_TOP_LEFT: *x = spec->anchor.x; *y = spec->anchor.y; break;
    case X11_ANCHOR_BOTTOM_LEFT: *x = spec->anchor.x; *y = spec->anchor.y + spec->anchor.height; break;
    case X11_ANCHOR_TOP_RIGHT: *x = spec->anchor.x + spec->anchor.width; *y = spec->anchor.y; break;
    case X11_ANCHOR_BOTTOM_RIGHT: *x = spec->anchor.x + spec->anchor.width; *y = spec->anchor.y + spec->anchor.height; break;
    default: *x = spec->anchor.x + spec->anchor.width / 2; *y = spec->anchor.y + spec->anchor.height / 2; break;
    }
}

static unsigned popup_flip_x(unsigned gravity)
{
    switch (gravity) {
    case X11_ANCHOR_LEFT: return X11_ANCHOR_RIGHT;
    case X11_ANCHOR_RIGHT: return X11_ANCHOR_LEFT;
    case X11_ANCHOR_TOP_LEFT: return X11_ANCHOR_TOP_RIGHT;
    case X11_ANCHOR_BOTTOM_LEFT: return X11_ANCHOR_BOTTOM_RIGHT;
    case X11_ANCHOR_TOP_RIGHT: return X11_ANCHOR_TOP_LEFT;
    case X11_ANCHOR_BOTTOM_RIGHT: return X11_ANCHOR_BOTTOM_LEFT;
    default: return gravity;
    }
}

static unsigned popup_flip_y(unsigned gravity)
{
    switch (gravity) {
    case X11_ANCHOR_TOP: return X11_ANCHOR_BOTTOM;
    case X11_ANCHOR_BOTTOM: return X11_ANCHOR_TOP;
    case X11_ANCHOR_TOP_LEFT: return X11_ANCHOR_BOTTOM_LEFT;
    case X11_ANCHOR_TOP_RIGHT: return X11_ANCHOR_BOTTOM_RIGHT;
    case X11_ANCHOR_BOTTOM_LEFT: return X11_ANCHOR_TOP_LEFT;
    case X11_ANCHOR_BOTTOM_RIGHT: return X11_ANCHOR_TOP_RIGHT;
    default: return gravity;
    }
}

static void popup_position_at(const struct yasl_private_popup_spec *spec, int ax, int ay,
        unsigned gravity, struct yasl_private_vector2i size, int *x, int *y)
{
    int content_w = spec->content_size.x ? spec->content_size.x : size.x - spec->content_offset.x;
    int content_h = spec->content_size.y ? spec->content_size.y : size.y - spec->content_offset.y;
    switch (gravity) {
    case X11_ANCHOR_LEFT: case X11_ANCHOR_TOP_LEFT: case X11_ANCHOR_BOTTOM_LEFT:
        *x = ax - content_w - spec->content_offset.x; break;
    case X11_ANCHOR_RIGHT: case X11_ANCHOR_TOP_RIGHT: case X11_ANCHOR_BOTTOM_RIGHT:
        *x = ax - spec->content_offset.x; break;
    default: *x = ax - content_w / 2 - spec->content_offset.x; break;
    }
    switch (gravity) {
    case X11_ANCHOR_TOP: case X11_ANCHOR_TOP_LEFT: case X11_ANCHOR_TOP_RIGHT:
        *y = ay - content_h - spec->content_offset.y; break;
    case X11_ANCHOR_BOTTOM: case X11_ANCHOR_BOTTOM_LEFT: case X11_ANCHOR_BOTTOM_RIGHT:
        *y = ay - spec->content_offset.y; break;
    default: *y = ay - content_h / 2 - spec->content_offset.y; break;
    }
}

static int popup_overflow(int pos, int extent, int low, int high)
{
    int64_t end = (int64_t)pos + extent;
    int64_t overflow = (pos < low ? (int64_t)low - pos : 0) + (end > high ? end - high : 0);
    return overflow > INT_MAX ? INT_MAX : (int)overflow;
}

static void popup_calculate_position(struct x11_display *d,
                                     struct x11_surface *parent,
                                     const struct yasl_private_popup_spec *spec,
                                     int *out_x, int *out_y,
                                     struct yasl_private_vector2i *out_size)
{
    int parent_rx = 0, parent_ry = 0;
    if (d->native) {
        Window child;
        XTranslateCoordinates(d->native, parent->window,
                             RootWindow(d->native, d->screen),
                             0, 0, &parent_rx, &parent_ry, &child);
    }
    int parent_cx = parent->is_popup ? parent->content_offset.x : 0;
    int parent_cy = parent->is_popup ? parent->content_offset.y : 0;

    struct yasl_private_vector2i size = spec->size;
    int anchor_x, anchor_y, root_x, root_y;
    popup_anchor_point(spec, &anchor_x, &anchor_y);
    anchor_x += parent_rx - parent_cx;
    anchor_y += parent_ry - parent_cy;
    popup_position_at(spec, anchor_x, anchor_y, spec->gravity, size, &root_x, &root_y);

    int mon_x = 0, mon_y = 0;
    int mon_w = d->native ? DisplayWidth(d->native, d->screen) : 1920;
    int mon_h = d->native ? DisplayHeight(d->native, d->screen) : 1080;

#if YASL_HAS_XRANDR
    if (d->native) {
        int nmonitors = 0;
        XRRMonitorInfo *monitors = XRRGetMonitors(d->native, RootWindow(d->native, d->screen), True, &nmonitors);
        if (monitors && nmonitors > 0) {
            for (int i = 0; i < nmonitors; i++) {
                if (anchor_x >= monitors[i].x && anchor_x < monitors[i].x + monitors[i].width &&
                    anchor_y >= monitors[i].y && anchor_y < monitors[i].y + monitors[i].height) {
                    mon_x = monitors[i].x;
                    mon_y = monitors[i].y;
                    mon_w = monitors[i].width;
                    mon_h = monitors[i].height;
                    break;
                }
            }
            XRRFreeMonitors(monitors);
        }
    }
#endif

    if (spec->constraints & X11_CONSTRAINT_FLIP_X) {
        int flip_x, flip_y;
        /* Reflect the selected anchor edge across the anchor rectangle center. */
        int anchor_origin_x = parent_rx - parent_cx + spec->anchor.x;
        int flipped_anchor_x = 2 * anchor_origin_x + spec->anchor.width - anchor_x;
        popup_position_at(spec, flipped_anchor_x, anchor_y, popup_flip_x(spec->gravity), size, &flip_x, &flip_y);
        if (popup_overflow(flip_x, size.x, mon_x, mon_x + mon_w) <
            popup_overflow(root_x, size.x, mon_x, mon_x + mon_w)) root_x = flip_x;
    }
    if (spec->constraints & X11_CONSTRAINT_FLIP_Y) {
        int flip_x, flip_y;
        int anchor_origin_y = parent_ry - parent_cy + spec->anchor.y;
        int flipped_anchor_y = 2 * anchor_origin_y + spec->anchor.height - anchor_y;
        popup_position_at(spec, anchor_x, flipped_anchor_y, popup_flip_y(spec->gravity), size, &flip_x, &flip_y);
        if (popup_overflow(flip_y, size.y, mon_y, mon_y + mon_h) <
            popup_overflow(root_y, size.y, mon_y, mon_y + mon_h)) root_y = flip_y;
    }
    if (spec->constraints & X11_CONSTRAINT_SLIDE_X) {
        if (root_x < mon_x) root_x = mon_x;
        if (root_x + size.x > mon_x + mon_w) root_x = mon_x + mon_w - size.x;
    }
    if (spec->constraints & X11_CONSTRAINT_SLIDE_Y) {
        if (root_y < mon_y) root_y = mon_y;
        if (root_y + size.y > mon_y + mon_h) root_y = mon_y + mon_h - size.y;
    }
    if ((spec->constraints & X11_CONSTRAINT_RESIZE_X) &&
        popup_overflow(root_x, size.x, mon_x, mon_x + mon_w)) {
        if (root_x >= mon_x + mon_w) root_x = mon_x + mon_w - 1;
        if (root_x < mon_x) { size.x -= mon_x - root_x; root_x = mon_x; }
        if (root_x + size.x > mon_x + mon_w) size.x = mon_x + mon_w - root_x;
        if (size.x < 1) size.x = 1;
    }
    if ((spec->constraints & X11_CONSTRAINT_RESIZE_Y) &&
        popup_overflow(root_y, size.y, mon_y, mon_y + mon_h)) {
        if (root_y >= mon_y + mon_h) root_y = mon_y + mon_h - 1;
        if (root_y < mon_y) { size.y -= mon_y - root_y; root_y = mon_y; }
        if (root_y + size.y > mon_y + mon_h) size.y = mon_y + mon_h - root_y;
        if (size.y < 1) size.y = 1;
    }

    *out_x = root_x;
    *out_y = root_y;
    if (out_size) *out_size = size;
}

static struct yasl_surface *surface_create_popup(struct yasl_surface *parent_base,
                                                 const struct yasl_private_popup_spec *spec)
{
    struct x11_surface *parent = (struct x11_surface *)parent_base;
    if (!parent || parent->destroyed || !popup_spec_valid(parent, spec))
        return NULL;
    struct x11_display *d = (struct x11_display *)parent_base->display;
    struct x11_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->base.display = parent_base->display;
    s->base.creation_flags = parent_base->creation_flags;
    s->base.presenter = YASL_PRIVATE_PRESENTER_CPU;
    s->base.scale_120 = d->scale_120;
    s->parent = parent;
    s->is_popup = true;
    s->content_offset = spec->content_offset;
    s->clock.refresh_ns = parent->clock.refresh_ns;
    bool argb = s->base.presenter == YASL_PRIVATE_PRESENTER_CPU && d->argb_visual;
    if (argb) {
        s->visual = d->argb_visual;
        s->visual_depth = d->argb_depth;
    } else if (d->native) {
        s->visual = DefaultVisual(d->native, d->screen);
        s->visual_depth = DefaultDepth(d->native, d->screen);
    }
    int root_x = 0, root_y = 0;
    struct yasl_private_vector2i actual_size;
    popup_calculate_position(d, parent, spec, &root_x, &root_y, &actual_size);
    if (!resize(s, actual_size)) { free(s); return NULL; }

    if (d->native) {
        XSetWindowAttributes attrs = {
            .override_redirect = True,
            .save_under = True,
        };
        unsigned long mask = CWOverrideRedirect | CWSaveUnder;
        Visual *visual = CopyFromParent;
        int depth = CopyFromParent;
        if (argb) {
            s->colormap = XCreateColormap(d->native, RootWindow(d->native, d->screen),
                                          s->visual, AllocNone);
            attrs.colormap = s->colormap;
            attrs.border_pixel = 0;
            attrs.background_pixel = 0;
            mask |= CWColormap | CWBorderPixel | CWBackPixel;
            visual = s->visual;
            depth = s->visual_depth;
        }
        s->window = XCreateWindow(d->native, RootWindow(d->native, d->screen),
                                  root_x, root_y, (unsigned)actual_size.x, (unsigned)actual_size.y,
                                  0, depth, InputOutput, visual, mask, &attrs);
        if (!s->window) {
            surface_destroy(&s->base);
            return NULL;
        }
        s->base.native_handle = (void *)(uintptr_t)s->window;
        s->base.size = s->size;
        s->gc = XCreateGC(d->native, s->window, 0, NULL);
        XSelectInput(d->native, s->window, ExposureMask | StructureNotifyMask |
                FocusChangeMask | KeyPressMask | KeyReleaseMask | PropertyChangeMask |
                (d->xi2 ? 0 : ButtonPressMask | ButtonReleaseMask |
                               PointerMotionMask | LeaveWindowMask | EnterWindowMask));
        if (d->xi2) {
            unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
            XISetMask(bits, XI_Motion);
            XISetMask(bits, XI_ButtonPress);
            XISetMask(bits, XI_ButtonRelease);
            XISetMask(bits, XI_Enter);
            XISetMask(bits, XI_Leave);
            XIEventMask mask = {XIAllMasterDevices, sizeof(bits), bits};
            XISelectEvents(d->native, s->window, &mask, 1);
        }
        Atom xdnd_version = 5;
        XChangeProperty(d->native, s->window, d->xdnd_aware, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&xdnd_version, 1);
        s->ic = create_ic(d, s);
        XMapRaised(d->native, s->window);
        XFlush(d->native);
    }
    s->next = d->surfaces;
    d->surfaces = s;
    if (d->native) popup_grab_update(d);
    return &s->base;
}

static bool surface_popup_reposition(struct yasl_surface *popup_base,
                                     const struct yasl_private_popup_spec *spec)
{
    struct x11_surface *s = (struct x11_surface *)popup_base;
    if (!s || s->destroyed || !s->is_popup || !s->parent || s->parent->destroyed ||
        !popup_spec_valid(s->parent, spec))
        return false;
    struct x11_display *d = (struct x11_display *)popup_base->display;
    s->content_offset = spec->content_offset;
    int root_x = 0, root_y = 0;
    struct yasl_private_vector2i actual_size;
    popup_calculate_position(d, s->parent, spec, &root_x, &root_y, &actual_size);
    bool changed_size = actual_size.x != s->size.x || actual_size.y != s->size.y;
    if (changed_size && !resize(s, actual_size)) return false;
    if (d->native && s->window) {
        XMoveResizeWindow(d->native, s->window, root_x, root_y,
                          (unsigned)actual_size.x, (unsigned)actual_size.y);
        XFlush(d->native);
    }
    if (changed_size)
        yasl_private_surface_notify_configure(&s->base,
                (struct yasl_private_geometry){actual_size, s->base.scale_120});
    schedule_redraw(&s->base);
    return true;
}

static void update_state(struct x11_surface *s)
{
    struct x11_display *d = (struct x11_display *)s->base.display;
    Atom actual;
    int format;
    unsigned long count, left;
    unsigned char *data = NULL;
    uint32_t state = s->base.state & ~(YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_MAXIMIZED);
    if (XGetWindowProperty(d->native, s->window, d->net_state, 0, 64, False,
            XA_ATOM, &actual, &format, &count, &left, &data) == Success && format == 32) {
        Atom *atoms = (Atom *)data;
        bool h = false, v = false;
        for (unsigned long i = 0; i < count; i++) {
            if (atoms[i] == d->fullscreen) state |= YASL_PRIVATE_STATE_FULLSCREEN;
            if (atoms[i] == d->maximized_h) h = true;
            if (atoms[i] == d->maximized_v) v = true;
        }
        if (h && v) state |= YASL_PRIVATE_STATE_MAXIMIZED;
    }
    if (data) XFree(data);
    if (state == s->base.state) return;
    s->base.state = state;
    yasl_private_surface_notify_state(&s->base, state);
}

/* Largest payload written in one property; bigger ones go out as INCR chunks. */
static size_t incr_chunk_limit(const struct x11_display *d)
{
    return (size_t)XMaxRequestSize(d->native) * 4 - 128;
}

enum convert_status { CONVERT_REFUSED, CONVERT_WRITTEN, CONVERT_INCR };

/* Unlink and free an INCR transfer. Cancel its timer unless the timer itself is calling. */
static void incr_send_retire(struct x11_display *d, struct incr_send *st, bool cancel_timer)
{
    for (struct incr_send **p = &d->incr_sends; *p; p = &(*p)->next) {
        if (*p != st) continue;
        *p = st->next;
        break;
    }
    if (cancel_timer && st->timeout) yasl_loop_del_timer(d->loop, st->timeout);
    free(st->data);
    free(st);
}

static uint64_t incr_send_timeout(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    struct incr_send *st = ud;
    incr_send_retire(st->display, st, false);
    return 0;
}

/* Restart the stall timeout; each chunk gets a fresh allowance. */
static void incr_send_arm(struct incr_send *st)
{
    struct x11_display *d = st->display;
    if (st->timeout) yasl_loop_del_timer(d->loop, st->timeout);
    st->timeout = yasl_loop_add_timer(d->loop, PASTE_TIMEOUT_NS, incr_send_timeout, st);
}

/* Start an INCR send: publish the total size, then wait for the requestor to
 * delete the property before each chunk. On failure data stays with the caller. */
static bool incr_send_start(struct x11_display *d, Window requestor, Atom property,
        Atom target, char *data, size_t length)
{
    struct incr_send *st = calloc(1, sizeof(*st));
    if (!st) return false;
    st->display = d;
    st->requestor = requestor;
    st->property = property;
    st->target = target;
    st->data = data;
    st->length = length;
    st->timeout = yasl_loop_add_timer(d->loop, PASTE_TIMEOUT_NS, incr_send_timeout, st);
    if (!st->timeout) {
        free(st);
        return false;
    }
    /* The requestor's property deletions arrive as PropertyNotify events. */
    XSelectInput(d->native, requestor, PropertyChangeMask);
    unsigned long total = length;
    XChangeProperty(d->native, requestor, property, d->incr, 32, PropModeReplace,
                    (const unsigned char *)&total, 1);
    st->next = d->incr_sends;
    d->incr_sends = st;
    return true;
}

/* Each requestor deletion asks for the next chunk. After the last data chunk, a
 * zero-length chunk ends the transfer, and its deletion finishes it. */
static void incr_send_step(struct incr_send *st)
{
    struct x11_display *d = st->display;
    if (st->finishing) {
        incr_send_retire(d, st, true);
        return;
    }
    if (st->offset >= st->length) {
        XChangeProperty(d->native, st->requestor, st->property, st->target, 8,
                        PropModeReplace, (const unsigned char *)"", 0);
        st->finishing = true;
    } else {
        size_t n = st->length - st->offset;
        if (n > incr_chunk_limit(d)) n = incr_chunk_limit(d);
        XChangeProperty(d->native, st->requestor, st->property, st->target, 8,
                        PropModeReplace, (const unsigned char *)st->data + st->offset, (int)n);
        st->offset += n;
    }
    incr_send_arm(st);
    XFlush(d->native);
}

/* Returns true when a deletion belongs to an INCR transfer. */
static bool incr_send_property_deleted(struct x11_display *d, const XPropertyEvent *ev)
{
    for (struct incr_send *st = d->incr_sends; st; st = st->next) {
        if (st->requestor != ev->window || st->property != ev->atom) continue;
        incr_send_step(st);
        return true;
    }
    return false;
}

static void incr_cancel_all(struct x11_display *d)
{
    while (d->incr_sends) incr_send_retire(d, d->incr_sends, true);
}

/* Write the selection's value for target to property on requestor. allow_incr is
 * false inside MULTIPLE, where a payload that needs INCR is refused. */
static enum convert_status convert_selection(struct x11_display *d, int slot, Atom target,
        Window requestor, Atom property, bool allow_incr)
{
    const char *text = d->selection[slot];
    if (!text) return CONVERT_REFUSED;
    if (target == d->targets) {
        size_t latin1_len = 0;
        char *latin1 = yasl_private_utf8_to_latin1(text, &latin1_len);
        Atom list[5];
        int count = 0;
        list[count++] = d->targets;
        list[count++] = d->utf8;
        if (latin1) list[count++] = XA_STRING;
        list[count++] = d->timestamp;
        list[count++] = d->multiple;
        free(latin1);
        XChangeProperty(d->native, requestor, property, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)list, count);
        return CONVERT_WRITTEN;
    }
    if (target == d->timestamp) {
        Time when = d->selection_time[slot];
        XChangeProperty(d->native, requestor, property, XA_INTEGER, 32, PropModeReplace,
                        (const unsigned char *)&when, 1);
        return CONVERT_WRITTEN;
    }
    if (target == d->multiple) {
        Atom actual = None;
        int format = 0;
        unsigned long count = 0, left = 0;
        unsigned char *pairs = NULL;
        if (XGetWindowProperty(d->native, requestor, property, 0, 0x10000, False, d->pair,
                &actual, &format, &count, &left, &pairs) != Success || !pairs ||
                actual != d->pair || format != 32 || count < 2 || (count & 1) || left) {
            if (pairs) XFree(pairs);
            return CONVERT_REFUSED;
        }
        Atom *entries = (Atom *)pairs;
        for (unsigned long i = 0; i + 1 < count; i += 2) {
            Atom sub_target = entries[i];
            Atom sub_property = entries[i + 1];
            bool ok = sub_target != d->multiple && sub_property != None && sub_property != property &&
                convert_selection(d, slot, sub_target, requestor, sub_property, false) == CONVERT_WRITTEN;
            if (!ok) entries[i + 1] = None;
        }
        XChangeProperty(d->native, requestor, property, d->pair, 32, PropModeReplace,
                        pairs, (int)count);
        XFree(pairs);
        return CONVERT_WRITTEN;
    }
    if (target != d->utf8 && target != XA_STRING) return CONVERT_REFUSED;
    const char *payload = text;
    size_t len = strlen(text);
    char *data = NULL;
    if (target == XA_STRING) {
        data = yasl_private_utf8_to_latin1(text, &len);
        if (!data) return CONVERT_REFUSED;
        payload = data;
    }
    if (len <= incr_chunk_limit(d)) {
        XChangeProperty(d->native, requestor, property, target, 8, PropModeReplace,
                        (const unsigned char *)payload, (int)len);
        free(data);
        return CONVERT_WRITTEN;
    }
    if (!allow_incr) {
        free(data);
        return CONVERT_REFUSED;
    }
    if (!data) {
        data = malloc(len);
        if (!data) return CONVERT_REFUSED;
        memcpy(data, text, len);
    }
    if (incr_send_start(d, requestor, property, target, data, len)) return CONVERT_INCR;
    free(data);
    return CONVERT_REFUSED;
}

static void selection_request(struct x11_display *d, XSelectionRequestEvent *request)
{
    int slot = request->selection == XA_PRIMARY ? YASL_PRIVATE_PRIMARY :
               request->selection == d->clipboard ? YASL_PRIVATE_CLIPBOARD : -1;
    /* Obsolete requestors pass property None and expect the target name. */
    Atom property = request->property ? request->property : request->target;
    enum convert_status status = slot < 0 ? CONVERT_REFUSED :
        convert_selection(d, slot, request->target, request->requestor, property, true);
    XEvent reply = {0};
    reply.xselection = (XSelectionEvent){
        .type = SelectionNotify, .display = d->native,
        .requestor = request->requestor, .selection = request->selection,
        .target = request->target, .time = request->time,
        .property = status == CONVERT_REFUSED ? None : property,
    };
    XSendEvent(d->native, request->requestor, False, 0, &reply);
    XFlush(d->native);
}

/* Another client took a selection this display owned. */
static void selection_clear(struct x11_display *d, const XSelectionClearEvent *event)
{
    if (event->window != d->selection_window) return;
    int slot = event->selection == XA_PRIMARY ? YASL_PRIVATE_PRIMARY :
               event->selection == d->clipboard ? YASL_PRIVATE_CLIPBOARD : -1;
    if (slot < 0) return;
    free(d->selection[slot]);
    d->selection[slot] = NULL;
}

static char *parse_file_uris(const char *data, size_t len, size_t *out_len)
{
    if (!data || !len) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    char *out = malloc(len * 2 + 2);
    if (!out) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    size_t out_pos = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (data[i] == '\r' || data[i] == '\n')) i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && data[i] != '\r' && data[i] != '\n') i++;
        size_t end = i;
        while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t' || data[end - 1] == '\r'))
            end--;
        size_t line_len = end - start;
        if (line_len >= 7 && !strncmp(data + start, "file://", 7)) {
            memcpy(out + out_pos, data + start, line_len);
            out_pos += line_len;
            out[out_pos++] = '\r';
            out[out_pos++] = '\n';
        }
    }
    if (!out_pos) {
        free(out);
        if (out_len) *out_len = 0;
        return NULL;
    }
    out[out_pos] = '\0';
    if (out_len) *out_len = out_pos;
    return out;
}

static void xdnd_selection_notify(struct x11_display *d, XSelectionEvent *event)
{
    if (!d->pending_drop.active) return;
    Window source = d->pending_drop.source;
    struct x11_surface *s = d->pending_drop.surface;
    d->pending_drop.active = false;
    d->pending_drop.source = None;
    d->pending_drop.surface = NULL;

    bool success = false;
    if (s && !s->destroyed && event->property != None) {
        Atom actual;
        int format;
        unsigned long count, left;
        unsigned char *data = NULL;
        if (XGetWindowProperty(d->native, s->window, event->property,
                0, TEXT_LIMIT / 4, True, AnyPropertyType,
                &actual, &format, &count, &left, &data) == Success && data && count > 0) {
            size_t out_len = 0;
            char *parsed = parse_file_uris((const char *)data, count, &out_len);
            if (parsed && out_len > 0) {
                push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_DROP}, parsed, out_len);
                free(parsed);
                success = true;
            }
        }
        if (data) XFree(data);
    }
    if (source) {
        XEvent finished = {0};
        finished.xclient = (XClientMessageEvent){
            .type = ClientMessage,
            .display = d->native,
            .window = source,
            .message_type = d->xdnd_finished,
            .format = 32,
            .data.l = {
                s ? s->window : None,
                success ? 1 : 0,
                success ? d->xdnd_action_copy : None,
            },
        };
        XSendEvent(d->native, source, False, NoEventMask, &finished);
        XFlush(d->native);
    }
}

static void selection_notify(struct x11_display *d, XSelectionEvent *event)
{
    struct paste_request **p = &d->pastes;
    while (*p && (*p)->window != event->requestor) p = &(*p)->next;
    if (!*p) return;
    struct paste_request *r = *p;
    struct x11_surface *s = r->target;
    if (!s) {
        /* Surface destruction detached this accepted request. The reply means
         * the owner has finished writing the initial property; retire safely. */
        *p = r->next;
        if (r->timeout) yasl_loop_del_timer(d->loop, r->timeout);
        XDestroyWindow(d->native, r->window);
        free(r);
        return;
    }
    if (event->property == None) {
        *p = r->next;
        if (r->timeout) yasl_loop_del_timer(d->loop, r->timeout);
        enum yasl_private_transfer_status status =
            XGetSelectionOwner(d->native, r->selection) == None
                ? YASL_PRIVATE_TRANSFER_UNAVAILABLE : YASL_PRIVATE_TRANSFER_FAILED;
        XDestroyWindow(d->native, r->window);
        if (s && !s->destroyed)
            yasl_private_surface_complete_paste(&s->base, r->id, status, NULL, 0);
        free(r);
        return;
    }
    /* Probe without deleting first; the type tells an INCR marker apart. */
    Atom probe_type = None;
    int probe_format = 0;
    unsigned long probe_count = 0, probe_left = 0;
    unsigned char *probe_data = NULL;
    int probe_result = XGetWindowProperty(d->native, r->window, event->property,
            0, 0, False, AnyPropertyType, &probe_type, &probe_format,
            &probe_count, &probe_left, &probe_data);
    if (probe_data) XFree(probe_data);
    if (probe_result == Success && probe_type == d->incr) {
        /* Deleting the marker tells the owner to send the first chunk. The
         * request stays queued until a zero-length chunk ends the transfer. */
        XDeleteProperty(d->native, r->window, event->property);
        r->incr = true;
        paste_rearm(r);
        XFlush(d->native);
        return;
    }
    *p = r->next;
    if (r->timeout) yasl_loop_del_timer(d->loop, r->timeout);
    Atom actual = None;
    int format = 0;
    unsigned long count = 0, left = 0;
    unsigned char *data = NULL;
    char *buf = NULL;
    size_t total = 0;
    unsigned long offset = 0;
    bool complete = false;

    /* Read up to TEXT_LIMIT bytes in 64KB chunks to avoid huge single-request limits */
    while (total < TEXT_LIMIT) {
        unsigned long chunk = 16384; /* 64 KB (in 32-bit multiples) */
        if (XGetWindowProperty(d->native, r->window, event->property,
                (long)offset, (long)chunk, True, AnyPropertyType,
                &actual, &format, &count, &left, &data) != Success || !actual) {
            if (data) { XFree(data); data = NULL; }
            break;
        }
        if ((actual != d->utf8 && actual != XA_STRING) || format != 8) {
            if (data) { XFree(data); data = NULL; }
            break;
        }
        if (count > TEXT_LIMIT - total) {
            XFree(data);
            data = NULL;
            break;
        }
        char *nb = realloc(buf, total + (size_t)count + 1);
        if (!nb) {
            XFree(data);
            break;
        }
        buf = nb;
        if (count) memcpy(buf + total, data, count);
        total += count;
        buf[total] = '\0';
        offset += (count + 3) / 4;
        XFree(data);
        data = NULL;
        if (left == 0) { complete = true; break; }
    }
    if (complete && actual == XA_STRING) {
        size_t utf8_len;
        char *utf8 = yasl_private_utf8_from_latin1(buf, total, &utf8_len);
        free(buf);
        buf = utf8;
        if (buf) total = utf8_len;
    }
    enum yasl_private_transfer_status status = YASL_PRIVATE_TRANSFER_FAILED;
    if (complete && buf && yasl_private_utf8_validate(buf, total))
        status = YASL_PRIVATE_TRANSFER_OK;
    yasl_private_surface_complete_paste(&s->base, r->id, status,
            status == YASL_PRIVATE_TRANSFER_OK ? buf : NULL,
            status == YASL_PRIVATE_TRANSFER_OK ? total : 0);
    free(buf);
    XDestroyWindow(d->native, r->window);
    free(r);
}

/* Consume a PropertyNotify on an INCR paste window. Returns false for other windows. */
static bool paste_incr_property(struct x11_display *d, const XPropertyEvent *ev)
{
    struct paste_request **link = &d->pastes;
    while (*link && !((*link)->incr && (*link)->window == ev->window)) link = &(*link)->next;
    if (!*link) return false;
    if (ev->state != PropertyNewValue || ev->atom != d->paste_property) return true;
    struct paste_request *r = *link;
    Atom actual = None;
    int format = 0;
    unsigned long count = 0, left = 0;
    unsigned char *data = NULL;
    /* Reading and deleting the chunk lets the owner write the next one. */
    int result = XGetWindowProperty(d->native, r->window, ev->atom, 0, TEXT_LIMIT / 4, True,
            AnyPropertyType, &actual, &format, &count, &left, &data);
    bool valid = result == Success && actual != None && format == 8 && left == 0 &&
            (actual == d->utf8 || actual == XA_STRING) && count <= TEXT_LIMIT - r->buf_len;
    if (!valid) {
        if (data) XFree(data);
        paste_complete(d, link, YASL_PRIVATE_TRANSFER_FAILED, NULL, 0);
        return true;
    }
    if (count == 0) {
        /* A zero-length chunk ends the transfer. */
        if (data) XFree(data);
        if (r->buf && r->incr_type == XA_STRING) {
            size_t utf8_len = 0;
            char *utf8 = yasl_private_utf8_from_latin1(r->buf, r->buf_len, &utf8_len);
            free(r->buf);
            r->buf = utf8;
            r->buf_len = utf8_len;
            if (!utf8) {
                paste_complete(d, link, YASL_PRIVATE_TRANSFER_FAILED, NULL, 0);
                return true;
            }
        }
        bool text_ok = !r->buf || yasl_private_utf8_validate(r->buf, r->buf_len);
        if (text_ok) paste_complete(d, link, YASL_PRIVATE_TRANSFER_OK, r->buf ? r->buf : "", r->buf_len);
        else paste_complete(d, link, YASL_PRIVATE_TRANSFER_FAILED, NULL, 0);
        return true;
    }
    if (!r->incr_type) r->incr_type = actual;
    char *grown = realloc(r->buf, r->buf_len + count + 1);
    if (!grown) {
        XFree(data);
        paste_complete(d, link, YASL_PRIVATE_TRANSFER_FAILED, NULL, 0);
        return true;
    }
    r->buf = grown;
    memcpy(r->buf + r->buf_len, data, count);
    r->buf_len += count;
    r->buf[r->buf_len] = '\0';
    XFree(data);
    paste_rearm(r);
    return true;
}

static void refresh_keyboard(struct x11_display *d)
{
    memset(d->keys, 0, sizeof(d->keys));
    if (!d->has_xkb) return;
    XkbDescPtr keyboard = XkbGetKeyboard(d->native, XkbAllComponentsMask, XkbUseCoreKbd);
    if (!keyboard) return;
    if (keyboard->names && keyboard->names->keys) {
        for (unsigned code = keyboard->min_key_code; code <= keyboard->max_key_code; code++)
            d->keys[code] = yasl_private_key_from_xkb_name(keyboard->names->keys[code].name);
    }
    XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
}

static void key_event(struct x11_surface *s, XKeyEvent *key, bool repeat)
{
    struct x11_display *d = (struct x11_display *)s->base.display;
    d->mods = modifiers(key->state);
    unsigned clean_state = key->state & ~(Mod2Mask | LockMask);
    KeySym sym = NoSymbol, base = NoSymbol, shifted = NoSymbol;

    int group = XkbGroupForCoreState(key->state);
    if (d->has_xkb) {
        sym = XkbKeycodeToKeysym(d->native, key->keycode, group, (clean_state & ShiftMask) ? 1 : 0);
        base = XkbKeycodeToKeysym(d->native, key->keycode, group, 0);
        shifted = XkbKeycodeToKeysym(d->native, key->keycode, group, 1);
    }
    if (sym == NoSymbol) {
        XKeyEvent clean_key = *key;
        clean_key.state = clean_state;
        sym = XLookupKeysym(&clean_key, (clean_state & ShiftMask) ? 1 : 0);
        base = XLookupKeysym(&clean_key, 0);
        shifted = XLookupKeysym(&clean_key, 1);
    }
    char local[128];
    char *text = local;
    int len = 0;
    if (key->type == KeyPress) {
        if (s->ic) {
            Status status;
            len = Xutf8LookupString(s->ic, key, local, sizeof(local), &sym, &status);
            if (status == XBufferOverflow && len > 0 && len < TEXT_LIMIT) {
                text = malloc((size_t)len + 1);
                if (text) len = Xutf8LookupString(s->ic, key, text, len + 1, &sym, &status);
                else len = 0;
            }
            if (status != XLookupChars && status != XLookupBoth) len = 0;
        } else {
            /* XLookupString's bytes are Latin-1. Use its modifier-aware keysym
             * and encode UTF-8 even when no input method is available. */
            XLookupString(key, local, sizeof(local), &sym, NULL);
            len = xkb_keysym_to_utf8((xkb_keysym_t)sym, local, sizeof(local));
            len = len > 0 ? len - 1 : 0; /* xkb includes the terminating NUL. */
        }
    }
    uint32_t cp = xkb_keysym_to_utf32((xkb_keysym_t)base);
    push(s, (struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_KEY,
        .key = {.key = key->keycode < 256 ? d->keys[key->keycode] : YASL_KEY_UNKNOWN,
                .codepoint = yasl_private_key_label(cp), .native_code = key->keycode,
                .shifted = yasl_private_key_label(xkb_keysym_to_utf32((xkb_keysym_t)shifted)),
                .mods = d->mods,
                .action = key->type == KeyRelease ? YASL_KEY_RELEASE :
                          repeat ? YASL_KEY_REPEAT : YASL_KEY_PRESS},
    }, NULL, 0);
    if (len > 0 && text && (unsigned char)text[0] >= 0x20 && text[0] != 0x7f &&
        !(d->mods & (YASL_PRIVATE_MOD_CTRL | YASL_PRIVATE_MOD_SUPER | YASL_PRIVATE_MOD_ALT)))
        push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_TEXT}, text, (size_t)len);
    if (text != local) free(text);
}

static void handle_client_message(struct x11_display *d, struct x11_surface *s,
                                  const XClientMessageEvent *ev)
{
    if (ev->message_type == d->protocols &&
        (Atom)ev->data.l[0] == d->delete_window) {
        yasl_private_surface_notify_close(&s->base);
    } else if (ev->message_type == d->xdnd_enter) {
        Window source = (Window)ev->data.l[0];
        int version = (int)((unsigned long)ev->data.l[1] >> 24);
        bool more_types = (ev->data.l[1] & 1) != 0;
        bool has_uri = false;
        if (more_types && d->native) {
            Atom actual;
            int format;
            unsigned long count, left;
            unsigned char *prop_data = NULL;
            if (XGetWindowProperty(d->native, source, d->xdnd_type_list,
                    0, 64, False, XA_ATOM, &actual, &format, &count, &left, &prop_data) == Success &&
                    actual == XA_ATOM && format == 32 && prop_data) {
                Atom *types = (Atom *)prop_data;
                for (unsigned long i = 0; i < count; i++) {
                    if (types[i] == d->uri_list) { has_uri = true; break; }
                }
            }
            if (prop_data) XFree(prop_data);
        } else {
            for (int i = 2; i <= 4; i++) {
                if ((Atom)ev->data.l[i] == d->uri_list) { has_uri = true; break; }
            }
        }
        d->xdnd.source = source;
        d->xdnd.version = version;
        d->xdnd.has_uri_list = has_uri;
        d->xdnd.accepts = has_uri;
    } else if (ev->message_type == d->xdnd_position) {
        Window source = (Window)ev->data.l[0];
        Window target = s->window;
        int rx = (int)(int16_t)((ev->data.l[2] >> 16) & 0xFFFF);
        int ry = (int)(int16_t)(ev->data.l[2] & 0xFFFF);
        Time t = (Time)ev->data.l[3];
        int wx = rx, wy = ry;
        if (d->native) {
            Window child;
            XTranslateCoordinates(d->native, RootWindow(d->native, d->screen), target,
                                 rx, ry, &wx, &wy, &child);
        }
        struct yasl_private_vector2i pos = {wx, wy};
        if (d->xdnd.surface && d->xdnd.surface != s) {
            struct x11_surface *old = d->xdnd.surface;
            struct yasl_private_vector2i old_pos = d->xdnd.pos;
            d->xdnd.surface = NULL;
            if (d->xdnd.accepts && !old->destroyed)
                yasl_private_surface_notify_drop(&old->base, YASL_PRIVATE_DROP_LEAVE, old_pos);
        }
        d->xdnd.surface = s;
        d->xdnd.pos = pos;
        d->xdnd.time = t;
        if (d->xdnd.accepts && !s->destroyed)
            yasl_private_surface_notify_drop(&s->base, YASL_PRIVATE_DROP_OVER, pos);

        if (d->native) {
            XEvent reply = {0};
            reply.xclient = (XClientMessageEvent){
                .type = ClientMessage,
                .display = d->native,
                .window = source,
                .message_type = d->xdnd_status,
                .format = 32,
                .data.l = {
                    target,
                    d->xdnd.accepts && !s->destroyed ? 1 : 0,
                    0,
                    0,
                    d->xdnd.accepts && !s->destroyed ? d->xdnd_action_copy : None,
                },
            };
            XSendEvent(d->native, source, False, NoEventMask, &reply);
        }
    } else if (ev->message_type == d->xdnd_leave) {
        if (d->xdnd.surface == s) {
            if (d->xdnd.accepts && !s->destroyed)
                yasl_private_surface_notify_drop(&s->base, YASL_PRIVATE_DROP_LEAVE, d->xdnd.pos);
        }
        d->xdnd = (typeof(d->xdnd)){0};
    } else if (ev->message_type == d->xdnd_drop) {
        Window source = (Window)ev->data.l[0];
        Window target = s->window;
        Time drop_time = (Time)ev->data.l[2];
        bool was_accepting = d->xdnd.accepts && d->xdnd.has_uri_list && (d->xdnd.source == source);
        if (d->xdnd.surface == s) {
            if (was_accepting && !s->destroyed)
                yasl_private_surface_notify_drop(&s->base, YASL_PRIVATE_DROP_LEAVE, d->xdnd.pos);
        }
        if (was_accepting && !s->destroyed) {
            d->pending_drop.active = true;
            d->pending_drop.source = source;
            d->pending_drop.surface = s;
            d->pending_drop.time = drop_time;
            if (d->native) {
                XConvertSelection(d->native, d->xdnd_selection, d->uri_list,
                                  d->paste_property, target, drop_time);
            }
        } else if (d->native) {
            XEvent reply = {0};
            reply.xclient = (XClientMessageEvent){
                .type = ClientMessage,
                .display = d->native,
                .window = source,
                .message_type = d->xdnd_finished,
                .format = 32,
                .data.l = { target, 0, None },
            };
            XSendEvent(d->native, source, False, NoEventMask, &reply);
        }
        d->xdnd = (typeof(d->xdnd)){0};
    }
}

static void dispatch(struct yasl_private_display *base)
{
    struct x11_display *d = (struct x11_display *)base;
    if (d->lost) return;
    d->depth++;
    while (!d->lost && XPending(d->native)) {
        XEvent event;
        XNextEvent(d->native, &event);
        if (event.type == GenericEvent && d->xi2 &&
            event.xcookie.extension == d->xi_opcode && XGetEventData(d->native, &event.xcookie)) {
            xi_event(d, &event.xcookie);
            XFreeEventData(d->native, &event.xcookie);
            continue;
        }
        if (event.type == MappingNotify) {
            XRefreshKeyboardMapping(&event.xmapping);
            refresh_keyboard(d);
            continue;
        }
        if (d->has_xkb && event.type == d->xkb_event) {
            XkbEvent *xkb = (XkbEvent *)&event;
            if (xkb->any.xkb_type == XkbMapNotify) XkbRefreshKeyboardMapping(&xkb->map);
            refresh_keyboard(d);
            continue;
        }
        switch (event.type) {
        case KeyPress: case KeyRelease: note_time(d, event.xkey.time); break;
        case ButtonPress: case ButtonRelease: note_time(d, event.xbutton.time); break;
        }
        if (XFilterEvent(&event, None)) continue;
        if (event.type == SelectionRequest) {
            selection_request(d, &event.xselectionrequest);
            continue;
        }
        if (event.type == SelectionClear) {
            selection_clear(d, &event.xselectionclear);
            continue;
        }
        if (event.type == PropertyNotify) {
            if (event.xproperty.state == PropertyDelete && incr_send_property_deleted(d, &event.xproperty))
                continue;
            if (paste_incr_property(d, &event.xproperty)) continue;
        }
        if (event.type == SelectionNotify) {
            if (event.xselection.selection == d->xdnd_selection) {
                xdnd_selection_notify(d, &event.xselection);
                continue;
            }
            selection_notify(d, &event.xselection);
            continue;
        }
        struct x11_surface *s = d->surfaces;
        while (s && s->window != event.xany.window) s = s->next;
        if (!s) continue;
        switch (event.type) {
        case Expose: s->want_draw = true; break;
        case ConfigureNotify: {
            struct yasl_private_vector2i size = {event.xconfigure.width, event.xconfigure.height};
            if (size.x == s->size.x && size.y == s->size.y) break;
            if (resize(s, size))
                yasl_private_surface_notify_configure(&s->base,
                        (struct yasl_private_geometry){size, s->base.scale_120});
            break;
        }
        case ClientMessage:
            handle_client_message(d, s, &event.xclient);
            break;
        case FocusIn: case FocusOut: {
            bool focused = event.type == FocusIn;
            s->focused = focused;
            if (s->ic) {
                if (focused && s->base.text_input_active) XSetICFocus(s->ic);
                else XUnsetICFocus(s->ic);
            }
            if (focused) s->base.state |= YASL_PRIVATE_STATE_ACTIVATED;
            else s->base.state &= ~YASL_PRIVATE_STATE_ACTIVATED;
            push(s, (struct yasl_private_event){
                .type = YASL_PRIVATE_EVENT_FOCUS, .focus.focused = focused}, NULL, 0);
            yasl_private_surface_notify_state(&s->base, s->base.state);
            break;
        }
        case PropertyNotify:
            if (event.xproperty.atom == d->net_state) update_state(s);
            break;
        case KeyPress: case KeyRelease: {
            bool repeat = false;
            if (event.type == KeyRelease && XPending(d->native)) {
                XEvent next;
                XPeekEvent(d->native, &next);
                if (next.type == KeyPress && next.xkey.window == event.xkey.window &&
                    next.xkey.keycode == event.xkey.keycode && next.xkey.time == event.xkey.time) {
                    XNextEvent(d->native, &event);
                    repeat = true;
                }
            }
            key_event(s, &event.xkey, repeat);
            break;
        }
        case MotionNotify: case LeaveNotify: case EnterNotify: case ButtonPress: case ButtonRelease: {
            if (event.type == ButtonPress && !d->xi2 && (event.xbutton.button < 4 || event.xbutton.button > 7))
                popup_dismiss_outside(d, event.xbutton.x_root, event.xbutton.y_root);
            struct yasl_private_event ev = {
                .pointer = {.device = YASL_PRIVATE_PTR_DEV_MOUSE},
            };
            unsigned state;
            if (event.type == MotionNotify) {
                ev.pointer.pos = (struct yasl_private_vector2i){event.xmotion.x, event.xmotion.y};
                ev.type = YASL_PRIVATE_EVENT_POINTER_MOTION;
                state = event.xmotion.state;
                if (d->drag.active) {
                    if (d->drag.carried)
                        place(&d->drag.carried->base, d->drag.carry_offset);
                    else if (d->drag.icon_window && d->native)
                        XMoveWindow(d->native, d->drag.icon_window,
                                    event.xmotion.x_root - d->drag.hotspot.x,
                                    event.xmotion.y_root - d->drag.hotspot.y);
                    struct yasl_private_vector2i hpos = ev.pointer.pos;
                    struct x11_surface *hsurf = d->native ?
                        surface_at_root_pos(d, event.xmotion.x_root, event.xmotion.y_root, &hpos) : s;
                    drag_set_hover(d, hsurf, hpos);
                }
            } else if (event.type == EnterNotify) {
                ev.pointer.pos = (struct yasl_private_vector2i){event.xcrossing.x, event.xcrossing.y};
                ev.type = YASL_PRIVATE_EVENT_POINTER_MOTION;
                state = event.xcrossing.state;
                if (d->drag.active) {
                    struct yasl_private_vector2i hpos = ev.pointer.pos;
                    struct x11_surface *hsurf = d->native ?
                        surface_at_root_pos(d, event.xcrossing.x_root, event.xcrossing.y_root, &hpos) : s;
                    drag_set_hover(d, hsurf, hpos);
                }
            } else if (event.type == LeaveNotify) {
                ev.pointer.pos = (struct yasl_private_vector2i){event.xcrossing.x, event.xcrossing.y};
                ev.type = YASL_PRIVATE_EVENT_POINTER_LEAVE;
                state = event.xcrossing.state;
                if (d->drag.active)
                    drag_leave_hover(d, s, ev.pointer.pos);
            } else {
                ev.pointer.pos = (struct yasl_private_vector2i){event.xbutton.x, event.xbutton.y};
                state = event.xbutton.state;
                unsigned button = event.xbutton.button;
                if (button >= 4 && button <= 7) {
                    if (event.type == ButtonRelease) break;
                    ev.type = YASL_PRIVATE_EVENT_POINTER_SCROLL;
                    if (button <= 5) ev.pointer.scroll.y = button == 4 ? -1 : 1;
                    else ev.pointer.scroll.x = button == 6 ? -1 : 1;
                    ev.pointer.scroll_valid = 1u;
                    ev.pointer.scroll_source = 1u;
                    ev.pointer.scroll_phase = 2u;
                } else {
                    ev.type = event.type == ButtonPress ? YASL_PRIVATE_EVENT_POINTER_DOWN : YASL_PRIVATE_EVENT_POINTER_UP;
                    ev.pointer.button = pointer_button(button);
                    if (event.type == ButtonRelease && d->drag.active)
                        drag_drop_hover(d);
                }
            }
            d->mods = ev.pointer.mods = modifiers(state);
            push(s, ev, NULL, 0);
            break;
        }
        }
    }
    leave(d);
}

static bool present(struct yasl_surface *base, const struct yasl_private_framebuffer *fb,
                    const struct yasl_private_damage_list *damage)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (s->destroyed || !s->drawing || s->presented || !fb || !damage ||
        fb->pixels != s->pixels || fb->size.x != s->size.x || fb->size.y != s->size.y ||
        damage->n < -1 || damage->n > YASL_PRIVATE_DAMAGE_MAX) return false;
    struct x11_display *d = (struct x11_display *)base->display;
    if (d->lost) { errno = ENOTCONN; return false; }
    XImage *im = s->image;
    if (s->shm_active) {
        if (damage->n == -1)
            XShmPutImage(d->native, s->window, s->gc, im, 0, 0, 0, 0,
                         (unsigned)s->size.x, (unsigned)s->size.y, False);
        else for (int i = 0; i < damage->n; i++) {
            struct yasl_private_rect2i r = damage->rects[i];
            int64_t right = (int64_t)r.x + r.width, bottom = (int64_t)r.y + r.height;
            int x = r.x > 0 ? r.x : 0, y = r.y > 0 ? r.y : 0;
            int w = (int)(right < s->size.x ? right : s->size.x) - x;
            int h = (int)(bottom < s->size.y ? bottom : s->size.y) - y;
            if (w > 0 && h > 0)
                XShmPutImage(d->native, s->window, s->gc, im, x, y, x, y, (unsigned)w, (unsigned)h, False);
        }
        s->presented = true;
        yasl_private_clock_presented(&s->clock, yasl_private_loop_now_ns(), false);
        return true;
    }
    bool direct = im->bits_per_pixel == 32 && im->red_mask == 0xff0000 &&
                  im->green_mask == 0xff00 && im->blue_mask == 0xff;
    int y_start = 0, y_end = s->size.y;
    int x_start = 0, x_end = s->size.x;
    if (damage->n >= 0) {
        y_start = s->size.y; y_end = 0;
        x_start = s->size.x; x_end = 0;
        for (int i = 0; i < damage->n; i++) {
            struct yasl_private_rect2i r = damage->rects[i];
            int y0 = r.y > 0 ? r.y : 0;
            int y1 = (r.y + r.height < s->size.y) ? r.y + r.height : s->size.y;
            int x0 = r.x > 0 ? r.x : 0;
            int x1 = (r.x + r.width < s->size.x) ? r.x + r.width : s->size.x;
            if (y0 < y_start) y_start = y0;
            if (y1 > y_end) y_end = y1;
            if (x0 < x_start) x_start = x0;
            if (x1 > x_end) x_end = x1;
        }
        if (y_start >= y_end || x_start >= x_end) {
            y_start = y_end = x_start = x_end = 0;
        }
    }
    size_t copy_bytes = (size_t)(x_end - x_start) * 4;
    for (int y = y_start; y < y_end; y++) {
        const uint32_t *row = s->pixels + (size_t)y * s->size.x;
        if (direct) memcpy(im->data + (size_t)y * im->bytes_per_line + (size_t)x_start * 4,
                           row + x_start, copy_bytes);
        else for (int x = x_start; x < x_end; x++) {
            uint32_t p = row[x];
            XPutPixel(im, x, y, component((p >> 16) & 255, im->red_mask) |
                    component((p >> 8) & 255, im->green_mask) | component(p & 255, im->blue_mask));
        }
    }
    if (damage->n == -1)
        XPutImage(d->native, s->window, s->gc, im, 0, 0, 0, 0,
                  (unsigned)s->size.x, (unsigned)s->size.y);
    else for (int i = 0; i < damage->n; i++) {
        struct yasl_private_rect2i r = damage->rects[i];
        int64_t right = (int64_t)r.x + r.width, bottom = (int64_t)r.y + r.height;
        int x = r.x > 0 ? r.x : 0, y = r.y > 0 ? r.y : 0;
        int w = (int)(right < s->size.x ? right : s->size.x) - x;
        int h = (int)(bottom < s->size.y ? bottom : s->size.y) - y;
        if (w > 0 && h > 0)
            XPutImage(d->native, s->window, s->gc, im, x, y, x, y, (unsigned)w, (unsigned)h);
    }
    s->presented = true;
    yasl_private_clock_presented(&s->clock, yasl_private_loop_now_ns(), false);
    return true;
}

static void flush(struct yasl_private_display *base)
{
    struct x11_display *d = (struct x11_display *)base;
    if (d->lost) return;
    d->depth++;
    for (struct x11_surface *s = d->surfaces, *next; s; s = next) {
        next = s->next;
        if (s->destroyed || !s->want_draw || s->drawing) continue;
        if (s->base.presenter != YASL_PRIVATE_PRESENTER_CPU) {
            s->want_draw = false;
            yasl_private_surface_notify_refresh(&s->base);
            continue;
        }
        if (!s->cbs || !s->cbs->draw) continue;
        s->want_draw = false;
        s->drawing = true;
        s->presented = false;
        s->damage = (struct yasl_private_damage_list){0};
        s->frame = (struct yasl_private_frame){
            .surface = &s->base,
            .target = {.generation = yasl_private_frame_begin(&s->base),
                       .pixels = s->pixels, .size = s->size},
        };
        yasl_private_surface_draw(&s->base, &s->frame);
        if (!s->destroyed) s->drawing = false;
    }
    if (!d->lost) XFlush(d->native);
    leave(d);
}

/* The connection is read in check(), before any callback. */
static void ready(void *ud, int fd, short events)
{
    (void)ud; (void)fd; (void)events;
}

/* A hung-up socket stays readable; stop watching it so the loop does not spin. */
static void drop_watch(struct x11_display *d)
{
    if (d->watch > 0) yasl_loop_del_fd(d->loop, d->watch);
    d->watch = 0;
}

/* Before the wait: dispatch what Xlib already holds, draw due surfaces, and send
 * requests. Loss is final, so a lost display only drops its watch. */
static bool prepare(struct yasl_private_display *base)
{
    struct x11_display *d = (struct x11_display *)base;
    if (!d->lost) {
        dispatch(base);
        flush(base);
    }
    if (d->lost) {
        drop_watch(d);
        return false;
    }
    /* Replies awaited while drawing can read events into Xlib's queue. */
    return XEventsQueued(d->native, QueuedAlready) > 0;
}

static void check(struct yasl_private_display *base)
{
    struct x11_display *d = (struct x11_display *)base;
    short revents = d->watch > 0 ? yasl_private_loop_revents(d->loop, d->watch) : 0;
    if (revents & (POLLERR | POLLHUP)) display_lost(d);
    if (d->lost) {
        drop_watch(d);
        return;
    }
    if (revents & POLLIN) dispatch(base);
}

static struct yasl_private_display *display_open(struct yasl_loop *loop,
        const struct yasl_private_display_spec *spec)
{
    struct x11_display *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->base.ops = &yasl_private_x11_backend;
    /* GL rendering threads call Xlib, so it must be thread-safe before the first connection. */
    static bool threads_ready;
    if (!threads_ready) {
        if (!XInitThreads()) { free(d); errno = ENODEV; return NULL; }
        threads_ready = true;
    }
    d->native = XOpenDisplay(NULL);
    if (!d->native) { free(d); errno = ENODEV; return NULL; }
    XSetIOErrorExitHandler(d->native, yasl_private_x11_io_exit_handler, d);
    display_register(d);
    d->base.native_handle = d->native;
    d->screen = DefaultScreen(d->native);
    d->scale_120 = scale_from_xft_dpi(d->native);
    XVisualInfo argb;
    if (XMatchVisualInfo(d->native, d->screen, 32, TrueColor, &argb)) {
        d->argb_visual = argb.visual;
        d->argb_depth = argb.depth;
    }
    d->has_shm = XShmQueryExtension(d->native);
    int xkb_opcode = 0, xkb_event = 0, xkb_error = 0;
    int xkb_major = XkbMajorVersion, xkb_minor = XkbMinorVersion;
    d->has_xkb = XkbQueryExtension(d->native, &xkb_opcode, &xkb_event, &xkb_error, &xkb_major, &xkb_minor);
    d->xkb_event = xkb_event;
    if (d->has_xkb) {
        unsigned events = XkbNewKeyboardNotifyMask | XkbMapNotifyMask | XkbNamesNotifyMask;
        XkbSelectEvents(d->native, XkbUseCoreKbd, events, events);
        refresh_keyboard(d);
    }
    int xi_event_base, xi_error_base;
    int major = 2, minor = 0;
    d->xi2 = XQueryExtension(d->native, "XInputExtension", &d->xi_opcode,
                            &xi_event_base, &xi_error_base) &&
             XIQueryVersion(d->native, &major, &minor) == Success && major >= 2;
    if (d->xi2) {
        d->abs_pressure = XInternAtom(d->native, "Abs Pressure", False);
        d->abs_tilt_x = XInternAtom(d->native, "Abs Tilt X", False);
        d->abs_tilt_y = XInternAtom(d->native, "Abs Tilt Y", False);
        d->abs_rotation = XInternAtom(d->native, "Abs Rotation", False);
        d->abs_distance = XInternAtom(d->native, "Abs Distance", False);
        d->abs_tangential = XInternAtom(d->native, "Abs TPressure", False);
        d->abs_wheel = XInternAtom(d->native, "Abs Wheel", False);
        d->tool_type = XInternAtom(d->native, "Wacom Tool Type", False);
        d->eraser = XInternAtom(d->native, "ERASER", False);
        d->brush = XInternAtom(d->native, "BRUSH", False);
        d->airbrush = XInternAtom(d->native, "AIRBRUSH", False);
        d->pencil = XInternAtom(d->native, "PENCIL", False);
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
        XISetMask(bits, XI_HierarchyChanged);
        XISetMask(bits, XI_DeviceChanged);
        XIEventMask mask = {XIAllDevices, sizeof(bits), bits};
        XISelectEvents(d->native, RootWindow(d->native, d->screen), &mask, 1);
    }
    if (!loop) {
        d->loop = yasl_loop_create();
        d->owns_loop = true;
    } else {
        d->loop = loop;
    }
    d->base.loop = d->loop;
    d->app_id = strdup(spec && spec->app_id ? spec->app_id : "YASL");
    d->base.app_id = d->app_id;
    if (!d->loop || !d->app_id) {
        if (d->owns_loop) yasl_loop_destroy(d->loop);
        free(d->app_id);
        XCloseDisplay(d->native);
        display_unregister(d);
        free(d);
        return NULL;
    }
    d->im = XOpenIM(d->native, NULL, NULL, NULL);
    d->selection_window = XCreateSimpleWindow(d->native,
            RootWindow(d->native, d->screen), 0, 0, 1, 1, 0, 0, 0);
#define ATOM(field, name) d->field = XInternAtom(d->native, name, False)
    ATOM(protocols, "WM_PROTOCOLS");
    ATOM(delete_window, "WM_DELETE_WINDOW");
    ATOM(utf8, "UTF8_STRING");
    ATOM(clipboard, "CLIPBOARD");
    ATOM(targets, "TARGETS");
    ATOM(paste_property, "YASL_PRIVATE_SELECTION");
    ATOM(incr, "INCR");
    ATOM(timestamp, "TIMESTAMP");
    ATOM(multiple, "MULTIPLE");
    ATOM(pair, "ATOM_PAIR");
    ATOM(net_name, "_NET_WM_NAME");
    ATOM(net_state, "_NET_WM_STATE");
    ATOM(motif_hints, "_MOTIF_WM_HINTS");
    ATOM(fullscreen, "_NET_WM_STATE_FULLSCREEN");
    ATOM(maximized_h, "_NET_WM_STATE_MAXIMIZED_HORZ");
    ATOM(maximized_v, "_NET_WM_STATE_MAXIMIZED_VERT");
    ATOM(xdnd_aware, "XdndAware");
    ATOM(xdnd_enter, "XdndEnter");
    ATOM(xdnd_position, "XdndPosition");
    ATOM(xdnd_status, "XdndStatus");
    ATOM(xdnd_leave, "XdndLeave");
    ATOM(xdnd_drop, "XdndDrop");
    ATOM(xdnd_finished, "XdndFinished");
    ATOM(xdnd_selection, "XdndSelection");
    ATOM(xdnd_action_copy, "XdndActionCopy");
    ATOM(xdnd_type_list, "XdndTypeList");
    ATOM(uri_list, "text/uri-list");
#undef ATOM
    if (d->loop) {
        d->watch = yasl_loop_add_fd(d->loop, ConnectionNumber(d->native), POLLIN, ready, &d->base);
        if (d->watch < 0) {
            if (d->im) XCloseIM(d->im);
            XDestroyWindow(d->native, d->selection_window);
            XCloseDisplay(d->native);
            display_unregister(d);
            free(d->app_id);
            if (d->owns_loop) yasl_loop_destroy(d->loop);
            free(d);
            return NULL;
        }
    }
    return &d->base;
}

static void display_close(struct yasl_private_display *base)
{
    struct x11_display *d = (struct x11_display *)base;
    assert(!d->surfaces && "destroy every surface before its display");
    drag_cancel(base);
    cancel_pastes(d, NULL);
    incr_cancel_all(d);
    d->pending_drop.active = false;
    if (d->loop && d->watch > 0) yasl_loop_del_fd(d->loop, d->watch);
    if (d->owns_loop && d->loop) {
        yasl_loop_destroy(d->loop);
        d->loop = NULL;
        d->base.loop = NULL;
    }
    if (d->im) XCloseIM(d->im);
    clear_pointers(d);
#if YASL_HAS_EGL
    if (d->egl_ready) eglTerminate(d->egl);
#endif
    for (size_t i = 0; i < sizeof(d->cursors) / sizeof(d->cursors[0]); i++) {
        if (d->cursors[i]) {
            if (!d->lost) XFreeCursor(d->native, d->cursors[i]);
            d->cursors[i] = None;
        }
    }
    if (!d->lost) XDestroyWindow(d->native, d->selection_window);
    /* Still registered during the close: a late IO error must reach our handlers,
     * because the default handler would exit. */
    XCloseDisplay(d->native);
    display_unregister(d);
    free(d->selection[0]);
    free(d->selection[1]);
    free(d->app_id);
    free(d);
}

static uint32_t caps(const struct yasl_private_display *d)
{
    return YASL_PRIVATE_CAP_POPUPS | YASL_PRIVATE_CAP_DRAG | YASL_PRIVATE_CAP_DRAG_CARRY | YASL_PRIVATE_CAP_PLACE_WINDOW |
        YASL_PRIVATE_CAP_PRIMARY_SELECTION | YASL_PRIVATE_CAP_CSD | YASL_PRIVATE_CAP_NATIVE_GRAPHICS |
        (((const struct x11_display *)d)->xi2 ? YASL_PRIVATE_CAP_TABLET : 0);
}

static bool copy_text(struct yasl_private_display *base, enum yasl_private_selection sel, const char *text)
{
    if (!text || (sel != YASL_PRIVATE_CLIPBOARD && sel != YASL_PRIVATE_PRIMARY)) {
        errno = EINVAL;
        return false;
    }
    struct x11_display *d = (struct x11_display *)base;
    char *value = strdup(text);
    if (!value) { errno = ENOMEM; return false; }
    free(d->selection[sel]);
    d->selection[sel] = value;
    /* Take ownership at the latest key or button time; TIMESTAMP reports it. */
    d->selection_time[sel] = d->last_time;
    XSetSelectionOwner(d->native, sel == YASL_PRIVATE_PRIMARY ? XA_PRIMARY : d->clipboard,
                        d->selection_window, d->last_time);
    XFlush(d->native);
    return true;
}

static bool request_paste(struct yasl_private_display *base, enum yasl_private_selection sel,
                          struct yasl_surface *target, uint64_t id)
{
    if (!target || (sel != YASL_PRIVATE_CLIPBOARD && sel != YASL_PRIVATE_PRIMARY)) {
        errno = EINVAL;
        return false;
    }
    struct x11_display *d = (struct x11_display *)base;
    struct x11_surface *s = (struct x11_surface *)target;
    if (s->destroyed || target->display != base) { errno = EINVAL; return false; }
    Atom selection = sel == YASL_PRIVATE_PRIMARY ? XA_PRIMARY : d->clipboard;
    Window owner = XGetSelectionOwner(d->native, selection);
    if (owner == None) {
        yasl_private_surface_complete_paste(target, id, YASL_PRIVATE_TRANSFER_UNAVAILABLE, NULL, 0);
        return true;
    }
    if (owner == d->selection_window) {
        const char *text = d->selection[sel];
        if (text)
            yasl_private_surface_complete_paste(target, id, YASL_PRIVATE_TRANSFER_OK, text, strlen(text));
        else
            yasl_private_surface_complete_paste(target, id, YASL_PRIVATE_TRANSFER_UNAVAILABLE, NULL, 0);
        return true;
    } else {
        struct paste_request *r = calloc(1, sizeof(*r));
        if (!r) { errno = ENOMEM; return false; }
        r->window = XCreateSimpleWindow(d->native, RootWindow(d->native, d->screen),
                                        0, 0, 1, 1, 0, 0, 0);
        if (!r->window) { free(r); errno = EIO; return false; }
        /* INCR chunks arrive as PropertyNotify events on this window. */
        XSelectInput(d->native, r->window, PropertyChangeMask);
        r->display = d;
        r->target = s;
        r->selection = selection;
        r->id = id;
        errno = 0;
        r->timeout = yasl_loop_add_timer(d->loop, PASTE_TIMEOUT_NS,
                paste_timeout, r);
        if (!r->timeout) {
            XDestroyWindow(d->native, r->window);
            free(r);
            if (!errno) errno = ENOMEM;
            return false;
        }
        r->next = d->pastes;
        d->pastes = r;
        XConvertSelection(d->native, selection, d->utf8, d->paste_property,
                            r->window, CurrentTime);
        XFlush(d->native);
        return true;
    }
}

static struct input_queue *surface_input_queue(struct yasl_surface *base)
{
    return &((struct x11_surface *)base)->input;
}

static void schedule_redraw(struct yasl_surface *base)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (!s->destroyed) {
        s->want_draw = true;
        struct x11_display *d = (struct x11_display *)base->display;
        if (d)
            yasl_private_loop_wake(d->loop);
    }
}

static struct yasl_private_display *surface_display(const struct yasl_surface *s) { return s->display; }
static struct yasl_private_geometry geometry(const struct yasl_surface *s)
{
    const struct x11_surface *x = (const struct x11_surface *)s;
    return (struct yasl_private_geometry){x->size, x->base.scale_120};
}
static int mods(const struct yasl_private_display *d) { return (int)((const struct x11_display *)d)->mods; }
static void *native_display(struct yasl_private_display *d) { return ((struct x11_display *)d)->native; }
static void *native_surface(struct yasl_surface *s) { return (void *)(uintptr_t)((struct x11_surface *)s)->window; }

static struct yasl_private_timing timing(const struct yasl_surface *base)
{
    const struct x11_surface *s = (const struct x11_surface *)base;
    struct yasl_private_timing t = {.refresh_ns = s->clock.refresh_ns,
        .last_present_ns = s->clock.last_present_ns,
        .next_present_ns = yasl_private_clock_next_present_ns(&s->clock),
        .source = s->clock.source};
    yasl_private_clock_resize_stats(&s->clock, &t.resize);
    return t;
}

static uint64_t frame_time(struct yasl_surface *s, uint64_t now)
{
    return yasl_private_clock_frame_timestamp_ns(&((struct x11_surface *)s)->clock, now);
}

static void cursor(struct yasl_surface *base, enum yasl_private_cursor cursor)
{
    static const unsigned shapes[] = {XC_left_ptr, XC_xterm, XC_hand2, XC_crosshair,
        XC_fleur, XC_bottom_right_corner, XC_bottom_left_corner,
        XC_sb_h_double_arrow, XC_sb_v_double_arrow};
    struct x11_surface *s = (struct x11_surface *)base;
    if (s->destroyed || (unsigned)cursor > YASL_PRIVATE_CURSOR_HIDDEN) return;
    struct x11_display *d = (struct x11_display *)base->display;
    if (!d->native) return;

    if (!d->cursors[cursor]) {
        if (cursor == YASL_PRIVATE_CURSOR_HIDDEN) {
            char bits[] = {0};
            Pixmap pixmap = XCreateBitmapFromData(d->native, s->window, bits, 1, 1);
            XColor color = {0};
            d->cursors[cursor] = XCreatePixmapCursor(d->native, pixmap, pixmap, &color, &color, 0, 0);
            XFreePixmap(d->native, pixmap);
        } else if ((unsigned)cursor < sizeof(shapes) / sizeof(shapes[0])) {
            d->cursors[cursor] = XCreateFontCursor(d->native, shapes[cursor]);
        }
    }
    if (d->cursors[cursor])
        XDefineCursor(d->native, s->window, d->cursors[cursor]);
}

static bool place(struct yasl_surface *base, struct yasl_private_vector2i at)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    Window root, child;
    int root_x, root_y, x, y;
    unsigned mask;
    if (s->destroyed || !d->native || !XQueryPointer(d->native, s->window,
            &root, &child, &root_x, &root_y, &x, &y, &mask)) return false;
    XMoveWindow(d->native, s->window, root_x - at.x, root_y - at.y);
    return true;
}

static uint32_t xsettings_card(const unsigned char *p, int size, bool msb)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * (msb ? size - 1 - i : i));
    return v;
}

/* Find an integer in an _XSETTINGS_SETTINGS blob: a byte-order byte, three
 * pad bytes, a serial, a count, then entries of type, pad, name length, name
 * padded to four bytes, serial, and a typed value. */
static bool xsettings_int(const unsigned char *p, size_t len, const char *name, int32_t *out)
{
    if (len < 12) return false;
    bool msb = p[0] == MSBFirst;
    uint32_t n = xsettings_card(p + 8, 4, msb);
    size_t want = strlen(name);
    size_t off = 12;
    for (uint32_t i = 0; i < n; i++) {
        if (len - off < 4) return false;
        unsigned type = p[off];
        size_t name_len = xsettings_card(p + off + 2, 2, msb);
        size_t padded = (name_len + 3) & ~(size_t)3;
        const unsigned char *entry_name = p + off + 4;
        off += 4;
        if (len - off < padded + 4) return false;
        off += padded + 4;
        size_t value_len;
        if (type == 0) {
            value_len = 4;
        } else if (type == 1) {
            if (len - off < 4) return false;
            size_t str_len = xsettings_card(p + off, 4, msb);
            if (str_len > len) return false;
            value_len = 4 + ((str_len + 3) & ~(size_t)3);
        } else if (type == 2) {
            value_len = 8;
        } else {
            return false;
        }
        if (len - off < value_len) return false;
        if (type == 0 && name_len == want && memcmp(entry_name, name, want) == 0) {
            *out = (int32_t)xsettings_card(p + off, 4, msb);
            return true;
        }
        off += value_len;
    }
    return false;
}

static int double_click_ms(const struct yasl_private_display *base)
{
    const struct x11_display *d = (const struct x11_display *)base;
    if (d->native) {
        char name[32];
        snprintf(name, sizeof(name), "_XSETTINGS_S%d", d->screen);
        Atom selection = XInternAtom(d->native, name, True);
        Atom property = XInternAtom(d->native, "_XSETTINGS_SETTINGS", True);
        Window owner = selection ? XGetSelectionOwner(d->native, selection) : None;
        Atom actual;
        int format;
        unsigned long count, left;
        unsigned char *data = NULL;
        int32_t ms;
        if (owner && property &&
            XGetWindowProperty(d->native, owner, property, 0, 0x10000, False, property,
                               &actual, &format, &count, &left, &data) == Success &&
            data && actual == property && format == 8 &&
            xsettings_int(data, count, "Net/DoubleClickTime", &ms)) {
            XFree(data);
            return ms;
        }
        if (data) XFree(data);
    }
    return portal_double_click_ms();
}

static bool read_pixels(struct yasl_surface *base, uint32_t *out, size_t cap,
                        struct yasl_private_vector2i *size)
{
    struct x11_surface *s = (struct x11_surface *)base;
    /* presented stays set from the last draw until the next one starts. */
    if (s->destroyed || s->drawing || !s->presented || !s->pixels) return false;
    return yasl_private_copy_frame(s->pixels, s->size, out, cap, size);
}

static bool bell(struct yasl_surface *base)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || !d->native) return false;
    /* The XKB bell names the window, so the window manager can flash it. */
    if (!XkbBell(d->native, s->window, 0, None))
        XBell(d->native, 0);
    XFlush(d->native);
    return true;
}

static void drag_end(struct x11_display *d, enum yasl_private_drag_end how)
{
    if (!d || !d->drag.active) return;
    struct yasl_private_drag_listener l = d->drag.listener;
    void *ud = d->drag.ud;
    struct x11_surface *hover = d->drag.hover;
    struct yasl_private_vector2i hover_pos = d->drag.hover_pos;

    d->drag.active = false;
    d->drag.listener = (struct yasl_private_drag_listener){0};
    d->drag.hover = NULL;
    d->drag.origin = NULL;
    d->drag.carried = NULL;

    if (d->drag.icon_window && d->native) {
        XDestroyWindow(d->native, d->drag.icon_window);
        d->drag.icon_window = None;
    }
    if (d->drag.icon_pixmap && d->native) {
        XFreePixmap(d->native, d->drag.icon_pixmap);
        d->drag.icon_pixmap = None;
    }

    if (hover && !hover->destroyed) {
        if (how == YASL_PRIVATE_DRAG_DROPPED)
            yasl_private_surface_notify_drag(&hover->base, YASL_PRIVATE_DRAG_DROP, hover_pos);
        else
            yasl_private_surface_notify_drag(&hover->base, YASL_PRIVATE_DRAG_LEAVE, hover_pos);
    }

    if (l.end)
        l.end(ud, how);
}

static bool drag_start(struct yasl_private_display *base, struct yasl_surface *origin,
                       const struct yasl_private_drag_image *image,
                       const struct yasl_private_drag_listener *l, void *ud)
{
    struct x11_display *d = (struct x11_display *)base;
    if (!d || !origin || !l) { errno = EINVAL; return false; }
    if (d->drag.active) { errno = EBUSY; return false; }
    /* A drag starts from a press. Without a connection there is no pointer state to read. */
    if (d->native && !held_button(d, ((struct x11_surface *)origin)->window, NULL, NULL)) {
        errno = ESTALE;
        return false;
    }
    d->drag.active = true;
    d->drag.origin = (struct x11_surface *)origin;
    d->drag.carried = NULL;
    d->drag.hover = NULL;
    d->drag.listener = *l;
    d->drag.ud = ud;
    d->drag.icon_window = None;
    d->drag.icon_pixmap = None;

    if (image && image->pixels && image->size.x > 0 && image->size.y > 0) {
        d->drag.hotspot = image->hotspot;
        int rx = 0, ry = 0;
        if (d->native) {
            Window root, child;
            int wx, wy;
            unsigned mask;
            XQueryPointer(d->native, RootWindow(d->native, d->screen),
                          &root, &child, &rx, &ry, &wx, &wy, &mask);
            int x = rx - image->hotspot.x;
            int y = ry - image->hotspot.y;
            XSetWindowAttributes attrs = {
                .override_redirect = True,
                .save_under = True,
            };
            d->drag.icon_window = XCreateWindow(d->native, RootWindow(d->native, d->screen),
                    x, y, (unsigned)image->size.x, (unsigned)image->size.y, 0,
                    CopyFromParent, InputOutput, CopyFromParent,
                    CWOverrideRedirect | CWSaveUnder, &attrs);
            if (d->drag.icon_window) {
                GC gc = XCreateGC(d->native, d->drag.icon_window, 0, NULL);
                XImage *im = XCreateImage(d->native, DefaultVisual(d->native, d->screen),
                        (unsigned)DefaultDepth(d->native, d->screen), ZPixmap, 0, NULL,
                        (unsigned)image->size.x, (unsigned)image->size.y, 32, 0);
                if (gc && im) {
                    im->data = malloc((size_t)im->bytes_per_line * (size_t)image->size.y);
                    if (im->data) {
                        bool direct = im->bits_per_pixel == 32 && im->red_mask == 0xff0000 &&
                                      im->green_mask == 0xff00 && im->blue_mask == 0xff;
                        for (int r = 0; r < image->size.y; r++) {
                            const uint32_t *row = image->pixels + (size_t)r * image->size.x;
                            if (direct) memcpy(im->data + (size_t)r * im->bytes_per_line, row, (size_t)image->size.x * 4);
                            else for (int c = 0; c < image->size.x; c++) {
                                uint32_t p = row[c];
                                XPutPixel(im, c, r, component((p >> 16) & 255, im->red_mask) |
                                        component((p >> 8) & 255, im->green_mask) | component(p & 255, im->blue_mask));
                            }
                        }
                        d->drag.icon_pixmap = XCreatePixmap(d->native, RootWindow(d->native, d->screen),
                                (unsigned)image->size.x, (unsigned)image->size.y,
                                (unsigned)DefaultDepth(d->native, d->screen));
                        if (d->drag.icon_pixmap) {
                            XPutImage(d->native, d->drag.icon_pixmap, gc, im, 0, 0, 0, 0,
                                      (unsigned)image->size.x, (unsigned)image->size.y);
                            XSetWindowBackgroundPixmap(d->native, d->drag.icon_window, d->drag.icon_pixmap);
                        }
                    }
                }
                if (im) XDestroyImage(im);
                if (gc) XFreeGC(d->native, gc);
                XMapRaised(d->native, d->drag.icon_window);
                XFlush(d->native);
            }
        }
    }
    return true;
}

static void drag_cancel(struct yasl_private_display *base)
{
    drag_end((struct x11_display *)base, YASL_PRIVATE_DRAG_CANCELLED);
}

static bool drag_carry(struct yasl_surface *base, struct yasl_private_vector2i offset)
{
    struct x11_display *d = (struct x11_display *)base->display;
    if (!d || !d->drag.active) { errno = EINVAL; return false; }
    if (d->drag.icon_window && d->native) {
        XDestroyWindow(d->native, d->drag.icon_window);
        d->drag.icon_window = None;
    }
    if (d->drag.icon_pixmap && d->native) {
        XFreePixmap(d->native, d->drag.icon_pixmap);
        d->drag.icon_pixmap = None;
    }
    d->drag.carried = (struct x11_surface *)base;
    d->drag.carry_offset = offset;
    return d->native ? place(base, offset) : true;
}

/* Shared checks for _NET_WM_MOVERESIZE: a held button, and a window manager that
 * supports the request. Returns the held button, or 0 with errno set. */
static unsigned moveresize_button(struct x11_display *d, struct x11_surface *s, int *rx, int *ry)
{
    unsigned button = held_button(d, s->window, rx, ry);
    if (!button) errno = ESTALE;
    else if (!wm_supports(d, "_NET_WM_MOVERESIZE")) { errno = ENOTSUP; button = 0; }
    return button;
}

static bool interactive_move(struct yasl_surface *base)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || !d->native) { errno = s->destroyed ? EINVAL : ENODEV; return false; }
    int rx, ry;
    unsigned button = moveresize_button(d, s, &rx, &ry);
    if (!button) return false;
    XUngrabPointer(d->native, CurrentTime);
    net_message(s, XInternAtom(d->native, "_NET_WM_MOVERESIZE", False), rx, ry, 8, button, 1);
    return true;
}

static bool interactive_resize(struct yasl_surface *base, enum yasl_private_resize_edge edge)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || !d->native) { errno = s->destroyed ? EINVAL : ENODEV; return false; }

    int dir;
    switch (edge) {
    case YASL_PRIVATE_EDGE_TOP_LEFT:     dir = 0; break;
    case YASL_PRIVATE_EDGE_TOP:          dir = 1; break;
    case YASL_PRIVATE_EDGE_TOP_RIGHT:    dir = 2; break;
    case YASL_PRIVATE_EDGE_RIGHT:        dir = 3; break;
    case YASL_PRIVATE_EDGE_BOTTOM_RIGHT: dir = 4; break;
    case YASL_PRIVATE_EDGE_BOTTOM:       dir = 5; break;
    case YASL_PRIVATE_EDGE_BOTTOM_LEFT:  dir = 6; break;
    case YASL_PRIVATE_EDGE_LEFT:         dir = 7; break;
    default: errno = EINVAL; return false;
    }

    int rx, ry;
    unsigned button = moveresize_button(d, s, &rx, &ry);
    if (!button) return false;
    XUngrabPointer(d->native, CurrentTime);
    net_message(s, XInternAtom(d->native, "_NET_WM_MOVERESIZE", False), rx, ry, dir, button, 1);
    return true;
}

static bool show_window_menu(struct yasl_surface *base, struct yasl_private_vector2i pos)
{
    struct x11_surface *s = (struct x11_surface *)base;
    struct x11_display *d = (struct x11_display *)base->display;
    if (s->destroyed || !d->native) { errno = s->destroyed ? EINVAL : ENODEV; return false; }
    int rx, ry;
    if (!held_button(d, s->window, &rx, &ry)) { errno = ESTALE; return false; }
    int device = 0;
    if (!d->xi2 || !XIGetClientPointer(d->native, s->window, &device)) { errno = ENOTSUP; return false; }
    /* The menu position is relative to the window; the message wants root coordinates. */
    Window child;
    int root_x, root_y;
    XTranslateCoordinates(d->native, s->window, RootWindow(d->native, d->screen),
                          pos.x, pos.y, &root_x, &root_y, &child);
    XUngrabPointer(d->native, CurrentTime);
    Atom atom = XInternAtom(d->native, "_GTK_SHOW_WINDOW_MENU", False);
    net_message(s, atom, device, root_x, root_y, 0, 0);
    return true;
}

/* Only a press can move a window under the pointer here; see the ESTALE rule. */
static bool place_under_pointer(struct yasl_surface *base, struct yasl_private_vector2i at)
{
    return place(base, at);
}

static bool surface_update_text_input(struct yasl_surface *base)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (!s || s->destroyed || !base->display) return false;
    if (s->ic) {
        if (s->focused && base->text_input_active) XSetICFocus(s->ic);
        else XUnsetICFocus(s->ic);
    }
    return true;
}

static void set_ime_caret(struct yasl_surface *base, struct yasl_private_rect2i caret)
{
    struct x11_surface *s = (struct x11_surface *)base;
    if (!s) return;
    s->ime_caret = caret;
    if (s->ic) {
        XPoint spot = { (short)caret.x, (short)(caret.y + caret.height) };
        XVaNestedList preedit_attr = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
        if (preedit_attr) {
            XSetICValues(s->ic, XNPreeditAttributes, preedit_attr, NULL);
            XFree(preedit_attr);
        }
    }
}

static const char *ime_preedit(const struct yasl_surface *base)
{
    const struct x11_surface *s = (const struct x11_surface *)base;
    return s ? s->ime_preedit : NULL;
}

static bool ime_preedit_cursor(const struct yasl_surface *base, int *begin, int *end)
{
    const struct x11_surface *s = (const struct x11_surface *)base;
    if (!s || !s->ime_preedit || s->ime_cursor_begin < 0)
        return false;
    if (begin) *begin = s->ime_cursor_begin;
    if (end) *end = s->ime_cursor_end;
    return true;
}

const struct yasl_private_backend_ops yasl_private_x11_backend = {
#if YASL_HAS_EGL
    .surface_create_gl = create_gl_surface,
    .gl_make_current = gl_make_current,
    .gl_get_proc_address = gl_get_proc_address,
    .gl_swap_interval = gl_swap_interval,
    .gl_swap_buffers = gl_swap_buffers,
#endif
    .type = YASL_PRIVATE_BACKEND_X11,
    .display_open = display_open,
    .display_close = display_close,
    .display_caps = caps,
    .display_prepare = prepare,
    .display_check = check,
    .display_flush = flush,
    .display_modifiers = mods,
    .display_copy_text = copy_text,
    .display_request_paste = request_paste,
    .display_drag_start = drag_start,
    .display_drag_cancel = drag_cancel,
    .surface_create_toplevel = create_toplevel,
    .surface_create_popup = surface_create_popup,
    .surface_popup_reposition = surface_popup_reposition,
    .surface_destroy = surface_destroy,
    .surface_input_queue = surface_input_queue,
    .surface_set_size = surface_set_size,
    .surface_set_fullscreen = surface_set_fullscreen,
    .surface_set_maximized = surface_set_maximized,
    .surface_minimize = surface_minimize,
    .surface_display = surface_display,
    .surface_set_callbacks = set_callbacks,
    .surface_set_title = set_title,
    .surface_schedule_redraw = schedule_redraw,
    .surface_present = present,
    .surface_geometry = geometry,
    .surface_timing = timing,
    .surface_frame_time_ns = frame_time,
    .surface_set_cursor = cursor,
    .surface_drag_carry = drag_carry,
    .surface_place_under_pointer = place_under_pointer,
    .surface_bell = bell,
    .surface_read_pixels = read_pixels,
    .display_double_click_ms = double_click_ms,
    .surface_start_interactive_move = interactive_move,
    .surface_start_interactive_resize = interactive_resize,
    .surface_show_window_menu = show_window_menu,
    .surface_update_text_input = surface_update_text_input,
    .surface_set_ime_caret = set_ime_caret,
    .surface_ime_preedit = ime_preedit,
    .surface_ime_preedit_cursor = ime_preedit_cursor,
    .display_get_native_handle = native_display,
    .surface_get_native_handle = native_surface,
};
