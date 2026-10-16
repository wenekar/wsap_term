#ifndef YASL_PRIVATE_WAYLAND_INTERNAL_H
#define YASL_PRIVATE_WAYLAND_INTERNAL_H

#define _GNU_SOURCE

#include "api.h"
#include "common/backend.h"
#include "common/timing.h"
#include "clipboard.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#if YASL_HAS_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>
#endif
#include "protocols/xdg-shell-client-protocol.h"
#include "protocols/xdg-decoration-client-protocol.h"
#include "protocols/cursor-shape-v1-client-protocol.h"
#include "protocols/fractional-scale-v1-client-protocol.h"
#include "protocols/viewporter-client-protocol.h"
#include "protocols/tablet-v2-client-protocol.h"
#include "protocols/text-input-unstable-v3-client-protocol.h"
#include "protocols/tearing-control-v1-client-protocol.h"
#include "protocols/presentation-time-client-protocol.h"
#include "protocols/fifo-v1-client-protocol.h"
#include "protocols/commit-timing-v1-client-protocol.h"
#include "protocols/xdg-toplevel-drag-v1-client-protocol.h"
#include "protocols/primary-selection-unstable-v1-client-protocol.h"
#include "protocols/relative-pointer-unstable-v1-client-protocol.h"
#include "protocols/pointer-constraints-unstable-v1-client-protocol.h"
#include "protocols/xdg-system-bell-v1-client-protocol.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>

#define NBUF 3

struct wayland_surface;
struct tablet_tool;
struct tablet_device;
struct tablet_pad;

struct buffer {
    struct wayland_surface *owner;
    struct wl_shm_pool *pool;
    struct wl_buffer *proxy;
    uint32_t *pixels;
    size_t capacity;
    int width, height;
    bool busy;
    bool presented;
};

struct wl_conn_output {
    struct wayland_display *conn;
    struct wl_output *proxy;
    struct wl_conn_output *next;
    uint64_t mask;
    uint32_t name;
    int32_t scale, pending_scale;
    uint32_t refresh_ns, pending_refresh_ns;
};

struct pending_press {
    struct wayland_surface *surface;
    struct yasl_private_vector2i pos;
    struct yasl_private_vector2d precise;
    int button;
    bool release;
};

struct press_queue {
    struct pending_press ev[8];
    int head;
    int n;
};

struct drag_icon {
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    struct wl_shm_pool *pool;
    struct wp_viewport *viewport;
    void *mapping;
    size_t size;
    int fd;
};

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

struct conn_drag {
    struct wl_data_source *source;
    struct xdg_toplevel_drag_v1 *toplevel;
    struct drag_icon icon;
    struct yasl_private_drag_listener listener;
    void *ud;
    struct seat *seat;
    uint32_t scale_120;
    bool dropped;
    bool carrying;
};

#define SEAT_TOUCH_POINTS 10

struct touch_point {
    struct wayland_surface *surface;
    double x, y;
    int32_t id;
    bool active;
    bool down;
    bool moved;
    bool up;
};

struct seat {
    /* 8-byte aligned pointers and 64-bit values */
    struct wayland_display *conn;
    struct seat *next;
    struct wl_seat *proxy;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct wp_cursor_shape_device_v1 *cursor_shape_dev;
    struct zwp_relative_pointer_v1 *relative_pointer;
    struct wl_surface *cursor_surface;
    struct wp_viewport *cursor_viewport;
    struct zwp_tablet_seat_v2 *tablet_seat;
    struct tablet_tool *tablet_tools;
    struct tablet_device *tablet_devices;
    struct tablet_pad *tablet_pads;
    struct zwp_text_input_v3 *text_input;
    struct clipboard *clipboard;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_state *compose;
    bool compose_attempted;
    uint64_t repeat_started_ns;
    struct wayland_surface *pointer_focus;
    struct wayland_surface *keyboard_focus;
    struct wayland_surface *tablet_focus;
    struct wayland_surface *ime_focus;
    struct wayland_surface *focus_leaving;
    struct wayland_surface *press_surface;
    struct wayland_surface *ptr_left;
    struct tablet_tool *tool;
    struct touch_point *touch_primary;
    uint64_t pointer_buttons;
    uint64_t tool_buttons;
    double axis[2], axis_value120[2];
    double axis_pixels[2];
    int64_t axis_discrete[2];
    uint32_t axis_source;
    bool scroll_gesture, axis_stop_pending;
    unsigned axis_phase;
    bool ime_enabled;
    bool axis_has_value120[2], axis_has_discrete[2];
    double touch_scroll_y;

    /* Embedded structs */
    xkb_mod_mask_t input_mod_masks[8];
    struct yasl_private_vector2i pointer_pos;
    struct yasl_private_vector2i ptr_left_pos;
    struct yasl_private_vector2i tool_pos;
    /* Unrounded device-pixel positions behind the rounded ones above. */
    struct yasl_private_vector2d pointer_precise, ptr_left_precise, tool_precise;
    struct touch_point touch_points[SEAT_TOUCH_POINTS];
    struct press_queue ptr_presses;
    struct press_queue tool_presses;

    /* 4-byte integers */
    uint32_t name;
    xkb_mod_index_t mod_shift, mod_alt, mod_ctrl, mod_super;
    int repeat_fd, repeat_watch;
    int32_t repeat_rate, repeat_delay;
    uint32_t repeat_key;
    uint32_t input_serial;
    uint32_t press_serial;
    uint32_t pointer_serial;
    int cursor_shape;
    struct yasl_private_pointer_pen pen;
    int tool_wheel;

    /* 1-byte booleans grouped tightly */
    bool input_mods_ready;
    bool repeat_held;
    bool motion_pending;
    bool ptr_left_motion;
    bool tool_motion;
    bool touch_pressed;
    bool touch_scrolling;
};

#include "common/input.h"

struct wayland_display {
    struct yasl_private_display base;
    /* 8-byte pointers & 64-bit masks */
    struct wl_display *wl;
    /* The thread that opened the display: the only one that arms frame callbacks. */
    pthread_t owner;
#if YASL_HAS_EGL
    EGLDisplay egl_display;
    EGLint egl_api;
    bool egl_initialized;
#endif
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_conn_output *outputs;
    uint64_t output_masks;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *decor_mgr;
    struct wp_fractional_scale_manager_v1 *scale_mgr;
    struct wp_viewporter *viewporter;
    struct seat *seats;
    struct seat *active_seat;
    struct wl_data_device_manager *data_manager;
    struct zwp_primary_selection_device_manager_v1 *primary_selection_mgr;
    struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_mgr;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct xdg_system_bell_v1 *system_bell;
    struct wl_cursor_theme *cursor_theme;
    const char *cursor_theme_name;
    struct zwp_tablet_manager_v2 *tablet_mgr;
    struct zwp_text_input_manager_v3 *text_input_mgr;
    struct wp_tearing_control_manager_v1 *tearing_mgr;
    struct wp_presentation *presentation;
    struct wp_fifo_manager_v1 *fifo_mgr;
    struct wp_commit_timing_manager_v1 *commit_timing_mgr;
    struct xkb_context *xkb_ctx;
    struct yasl_loop *loop;
    char *app_id;
    struct xdg_toplevel_drag_manager_v1 *toplevel_drag_mgr;
    struct conn_drag drag;
    struct wayland_surface *surfaces;
    struct wayland_surface *dead;
    /* While lost: retries a connection; stale marks objects of the dead one. */
    yasl_private_timer_id reconnect_timer;
    uint64_t reconnect_delay_ns;
    bool stale;

    /* 4-byte integers */
    int cursor_size;
    int cursor_theme_scale;
    clockid_t pres_clock;
    int watch;
    int depth;
    uint64_t next_tool_id;

    /* 1-byte booleans */
    bool owns_loop;
    bool pres_clock_known;
    bool reading;
    bool lost;
};

struct latency_feedback {
    struct wayland_surface *window;
    struct wp_presentation_feedback *proxy;
    struct latency_feedback *next;
    bool in_pool;
};

struct wayland_surface {
    struct yasl_surface base;
    /* 8-byte pointers & 64-bit values */
    struct wayland_display *conn;
    struct wayland_surface *conn_next;
    struct wayland_surface *dead_next;
    struct wayland_surface *parent;
    struct wl_surface *surface;
#if YASL_HAS_EGL
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    EGLContext egl_context;
    EGLConfig egl_config;
    struct yasl_private_gl_options gl_options;
    struct wayland_surface *gl_share;
    bool gl_built;
#endif
    char *title;
    bool tearing;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct xdg_popup *popup;
    struct zxdg_toplevel_decoration_v1 *toplevel_decor;
    struct wp_fractional_scale_v1 *frac_scale;
    struct wp_viewport *viewport;
    char *ime_preedit;
    char *ime_commit;
    char *ime_shown;
    struct seat *ime_seat;
    size_t ime_delete_begin, ime_delete_end;
    uint64_t ime_delete_revision, ime_delete_generation;
    struct wp_tearing_control_v1 *tearing_control;
    struct latency_feedback *latency_feedbacks;
    struct latency_feedback latency_pool[8];
    struct wp_fifo_v1 *fifo;
    struct wp_commit_timer_v1 *commit_timer;
    struct wl_callback *frame_cb;
    /* Persistent lock held while relative; lock_seat owns the locked wl_pointer. */
    struct zwp_locked_pointer_v1 *pointer_lock;
    struct seat *lock_seat;
    const struct yasl_private_window_cbs *cbs;
    void *ud;
    uint64_t frames_skipped;
    uint64_t outputs;
    double scale_factor;

    /* Sub-structures */
    struct yasl_private_rect2i ime_caret;
    struct yasl_private_rect2i content;
    struct yasl_private_frame_clock clock;
    struct buffer buffers[NBUF];
    struct yasl_private_frame frame;
    struct yasl_private_damage_list damage;
    int ime_cursor_begin;
    struct input_queue input;

    /* 4-byte integers */
    enum yasl_private_cursor cursor;
    int ime_cursor_end;
    int ime_shown_begin, ime_shown_end;
    int floating_width, floating_height;
    int cur;
    /* Buffer holding the last presented frame, valid while has_shown is set. */
    int shown;
    int width, height;
    int logical_width, logical_height;
    int req_width, req_height;
    int bound_width, bound_height;
    uint32_t scale_120;
    uint32_t pending_scale_120;
    uint32_t pending_serial;

    /* 1-byte booleans packed tightly */
    bool frame_active;
    bool frame_presented;
    bool has_shown;
    bool size_pending;
    bool sample_refresh;
    bool has_pending_serial;
    bool configured;
    bool closed;
    bool destroyed;
    bool opaque;
    bool want_draw;
    bool relative;
    bool pointer_locked;
    bool ime_delete_pending;
    /* Set by a swap on a rendering thread; the owner arms the frame request at its next flush. */
    atomic_bool frame_deferred;
};


int  device_px(int logical, uint32_t scale_120);
bool buffer_size(int width, int height, size_t *bytes);
int  buffer_reserve(struct wayland_surface *w, struct buffer *b, size_t needed);
void buffer_destroy(struct buffer *b);
int  buffer_pick_free_index(const struct buffer *bufs, int cur);
int  buffer_prepare(struct wayland_surface *w, struct buffer *b, int width, int height,
                    const struct wl_buffer_listener *listener);
int  apply_pending_geometry(struct wayland_surface *w);

static inline void wl_damage_list(struct wl_surface *s, const struct yasl_private_damage_list *d)
{
    if (d->n < 0) {
        wl_surface_damage_buffer(s, d->bounds.x, d->bounds.y,
                                 d->bounds.width, d->bounds.height);
        return;
    }
    for (int i = 0; i < d->n; i++)
        wl_surface_damage_buffer(s, d->rects[i].x, d->rects[i].y,
                                 d->rects[i].width, d->rects[i].height);
}

void surface_update_opaque_region(struct wayland_surface *w);
int  surface_init_scaling(struct wayland_surface *w);
void surface_arm_deferred(struct wayland_surface *w);

uint64_t conn_output_mask(const struct wayland_display *c, const struct wl_output *proxy);
uint32_t conn_output_scale_120(const struct wayland_display *c, uint64_t outputs);
void     conn_destroy_outputs(struct wayland_display *c);
void     surface_follow_outputs(struct wayland_surface *w);
uint64_t conn_pres_to_mono(const struct wayland_display *c, uint64_t ns);
uint64_t conn_mono_to_pres(const struct wayland_display *c, uint64_t ns);
/* True when surface_draw_frame would draw now. */
bool surface_draw_ready(const struct wayland_surface *w);
void surface_draw_frame(struct wayland_surface *w);
void wayland_surface_arm_frame(struct wayland_surface *w);
void surface_fail(struct wayland_surface *w);
void display_leave(struct wayland_display *c);
/* Native objects of a toplevel: built at creation and again after a reconnect. */
int  surface_build_toplevel(struct wayland_surface *w, bool fullscreen);
void surface_release_native(struct wayland_surface *w);
#if YASL_HAS_EGL
bool surface_build_gl(struct wayland_surface *w);
#endif

static inline struct wayland_surface *surface_root(struct wayland_surface *w)
{
    while (w && w->parent)
        w = w->parent;
    return w;
}

void conn_drag_end(struct wayland_display *c);
void conn_drop_target(struct wayland_display *c, enum yasl_private_drop_event ev,
                      struct wl_surface *on, double x, double y);
void conn_drag_target(struct wayland_display *c, enum yasl_private_drag_event ev,
                      struct wl_surface *on, double x, double y);

void seat_bind(struct wayland_display *c, struct wl_registry *registry,
               uint32_t name, uint32_t version);
bool seat_remove(struct wayland_display *c, uint32_t name);
void seat_destroy(struct seat *s);
void seat_note_input(struct seat *s, uint32_t serial);
/* Serial of the latest press while a pointer button or tablet tip is still down, else 0. */
uint32_t seat_held_press_serial(const struct seat *s);
/* Locks the pointer through the seat that last delivered input, or any seat with a pointer. */
void surface_lock_preferred(struct wayland_surface *w);
void seat_ensure_text_input(struct seat *s);
void seat_forget_surface(struct seat *s, struct wayland_surface *w);
void seat_clipboard_ensure(struct seat *s);
int  display_input_init(struct wayland_display *c);
extern const struct wl_seat_listener seat_listener;

void keyboard_repeat_stop(struct seat *s);
void keyboard_repeat_sync(struct seat *s);
int  keyboard_repeat_init(struct seat *s);
void keyboard_settle_focus(struct wayland_display *c);
bool conn_keyboard_on(const struct wayland_display *c, const struct wayland_surface *w,
                      const struct seat *except);
extern const struct wl_keyboard_listener keyboard_listener;
int  seat_modifiers(const struct seat *s);

void surface_init_cursor(struct wayland_display *c);
void seat_destroy_tablet(struct seat *s);
void seat_pointer_reset(struct seat *s);
void surface_unlock_pointer(struct wayland_surface *w);

extern const struct zwp_text_input_v3_listener text_input_listener;
void ime_reset(struct wayland_surface *w);
void keyboard_text_reset(struct seat *s);
void ime_send_caret(struct wayland_surface *w);
bool wayland_surface_update_text_input(struct wayland_surface *w);

int  surface_init_shell(struct wayland_surface *w, const char *title);
extern const struct xdg_surface_listener shell_surface_listener;
extern const struct wl_registry_listener registry_listener;

void surface_feedback_destroy(struct wayland_surface *w);
void surface_request_presentation_feedback(struct wayland_surface *w);

#endif
