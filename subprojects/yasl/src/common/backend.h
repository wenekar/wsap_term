#ifndef YASL_PRIVATE_BACKEND_H
#define YASL_PRIVATE_BACKEND_H

#include "common/types.h"
#include "common/private_loop.h"
#include "common/private_native.h"
#include "common/input.h"
#include "common/gl_options.h"
#include "common/timing.h"

#include <stdatomic.h>
#include <string.h>

struct yasl_private_backend_ops;
struct yasl_private_event_state;

struct yasl_private_display {
    const struct yasl_private_backend_ops *ops;
    void *native_handle;
    /* Connection lost; native handles are unusable until it is restored. Read by GL
     * rendering threads, so it is atomic. */
    atomic_bool offline;
    /* Set by the backend while a native modal loop (macOS live resize) blocks the
     * event loop; refresh callbacks run only then. */
    bool modal;
    struct yasl_loop *loop;
    /* Backend's copy of yasl_private_display_spec.app_id, or NULL; for desktop services. */
    const char *app_id;
    struct yasl_private_event_state *events;
    struct yasl_surface *surfaces;
    /* Set by the backend: a lost connection comes back (Wayland). */
    bool reconnects;
    /* The drag in progress dropped on one of the display's surfaces. */
    bool drag_dropped;
    /* Set by the public layer, or NULL. pending: an event is ready that the
     * queue does not hold, such as an owed REFRESH. deliver: hand queued events
     * to the application during the loop step. */
    bool (*pending)(struct yasl_private_display *d);
    void (*deliver)(struct yasl_private_display *d);
};

struct yasl_surface {
    struct yasl_vulkan_binding *vulkan;
    unsigned creation_flags;
    enum yasl_private_presenter presenter;
    struct yasl_surface *parent;
    /* CPU canvas returned by yasl_cpu_pixels and copied into each drawn frame. */
    uint32_t *canvas;
    int canvas_width, canvas_height;
    struct yasl_private_damage_list cpu_damage;
    /* Window values last reported through public WINDOW events. */
    struct yasl_private_vector2i reported_size;
    uint32_t reported_scale_120, reported_state;
    bool refresh_owed;
    /* yasl_surface_request_frame waits for the next frame shown. Each loop
     * step resolves it after the callbacks that present; see backend.c. */
    bool frame_requested;
    /* Written by GL presents on a rendering thread. */
    _Atomic uint64_t presented_ns;
    /* When the last frame request was answered. */
    uint64_t frame_answered_ns;
    yasl_private_timer_id frame_timer;
    void (*refresh)(struct yasl_surface *);
    bool text_input_active;
    unsigned text_purpose;
    char *text_surrounding;
    size_t text_length, text_begin, text_end;
    uint64_t text_revision, text_generation;
    struct yasl_private_display *display;
    void *native_handle;
    struct yasl_private_vector2i size;
    uint32_t scale_120;
    uint32_t state; /* Authoritative YASL_PRIVATE_STATE_* bitmask, maintained by the backend. */
    uint32_t bg_color;
    struct yasl_private_window_cbs callbacks;
    void *ud;
    uint64_t frame_generation;
    struct input_queue input;
    enum yasl_private_pointer_mode pointer_mode;
    struct yasl_private_frame_clock clock;
    uint64_t frames_skipped;
    yasl_private_timer_id *timers;
    int timer_count;
    int timer_capacity;
    struct yasl_surface *event_next;
    uint64_t id;
    void *userdata;
    bool popup;
    bool drawing;
    bool frame_presented;
    uint32_t reported_scale;
    uint64_t draw_index;
    struct yasl_private_frame *active_frame;
    yasl_private_pacing_mode pacing;
    yasl_private_timer_id pacing_timer;
};

bool yasl_private_events_open(struct yasl_private_display *d);
void yasl_private_events_close(struct yasl_private_display *d);
int yasl_private_events_error(struct yasl_private_display *d);
/* Whether an event was lost, without reporting it. */
bool yasl_private_events_failed(struct yasl_private_display *d);
bool yasl_private_events_pending(struct yasl_private_display *d);
struct yasl_surface *yasl_private_events_find(struct yasl_private_display *d, uint64_t id);
bool yasl_private_events_register(struct yasl_surface *s);
void yasl_private_events_unregister(struct yasl_surface *s);
bool yasl_private_events_push(struct yasl_private_display *d, struct yasl_surface *queue_owner,
                      const struct yasl_private_event *event);
void yasl_private_surface_notify_configure(struct yasl_surface *s, struct yasl_private_geometry geometry);
void yasl_private_surface_notify_refresh(struct yasl_surface *s);
/* The system showed the surface's last frame; answers a pending frame request. */
void yasl_private_surface_notify_frame_shown(struct yasl_surface *s);
void yasl_private_surface_notify_close(struct yasl_surface *s);
void yasl_private_surface_notify_state(struct yasl_surface *s, uint32_t state);
void yasl_private_surface_notify_drag(struct yasl_surface *s, enum yasl_private_drag_event ev, struct yasl_private_vector2i pos);
void yasl_private_surface_notify_drop(struct yasl_surface *s, enum yasl_private_drop_event ev, struct yasl_private_vector2i pos);
void yasl_private_surface_notify_composition(struct yasl_surface *s, const char *text, int begin, int end);
void yasl_private_surface_draw(struct yasl_surface *s, struct yasl_private_frame *frame);
void yasl_private_display_notify_event(struct yasl_private_display *d, enum yasl_private_event_type type);
void yasl_private_surface_complete_paste(struct yasl_surface *s, uint64_t request_id,
        enum yasl_private_transfer_status status, const char *text, size_t len);
uint64_t yasl_private_events_request_id(struct yasl_private_display *d);

void yasl_private_surface_clear_timers(struct yasl_surface *s);
struct yasl_surface *yasl_private_create_toplevel(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device);

/* Report a presented CPU frame's extent, and copy it when out can hold it. */
static inline bool yasl_private_copy_frame(const uint32_t *pixels, struct yasl_private_vector2i extent,
        uint32_t *out, size_t cap, struct yasl_private_vector2i *size)
{
    *size = extent;
    size_t count = (size_t)extent.x * (size_t)extent.y;
    if (!count || count > cap) return false;
    memcpy(out, pixels, count * sizeof(*pixels));
    return true;
}

static inline uint64_t yasl_private_frame_begin(struct yasl_surface *s)
{
    if (++s->frame_generation == 0) ++s->frame_generation;
    return s->frame_generation;
}

typedef void (*yasl_private_gl_proc)(void);
struct yasl_private_backend_ops {
    struct yasl_surface *(*surface_create_gl)(struct yasl_private_display *, const struct yasl_private_window_spec *, const int *, struct yasl_surface *share);
    bool (*gl_make_current)(struct yasl_private_display *, struct yasl_surface *);
    yasl_private_gl_proc (*gl_get_proc_address)(struct yasl_private_display *, const char *);
    bool (*gl_swap_interval)(struct yasl_private_display *, struct yasl_surface *, int);
    bool (*gl_swap_buffers)(struct yasl_private_display *, struct yasl_surface *);
    void *(*surface_get_metal_layer)(struct yasl_surface *);
    bool (*surface_update_text_input)(struct yasl_surface *);

    enum yasl_private_backend type;
    struct yasl_private_display *(*display_open)(struct yasl_loop *loop, const struct yasl_private_display_spec *spec);
    void (*display_close)(struct yasl_private_display *d);
    uint32_t (*display_caps)(const struct yasl_private_display *d);
    /* The loop driver hooks; see struct yasl_private_loop_driver. prepare and
     * check are required. wait is optional, for platforms whose events do not
     * arrive on a descriptor. */
    bool (*display_prepare)(struct yasl_private_display *d);
    int (*display_wait)(struct yasl_private_display *d, int timeout_ms);
    void (*display_check)(struct yasl_private_display *d);
    void (*display_flush)(struct yasl_private_display *d);
    int (*display_modifiers)(const struct yasl_private_display *d);
    /* Optional; returns zero or less when the system reports no interval. */
    int (*display_double_click_ms)(const struct yasl_private_display *d);
    bool (*display_copy_text)(struct yasl_private_display *d, enum yasl_private_selection sel, const char *text);
    bool (*display_request_paste)(struct yasl_private_display *d, enum yasl_private_selection sel,
                                  struct yasl_surface *target, uint64_t request_id);
    bool (*display_drag_start)(struct yasl_private_display *d, struct yasl_surface *origin, const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *l, void *ud);
    void (*display_drag_cancel)(struct yasl_private_display *d);
    /* Create with callbacks disabled; common code installs them after return. */
    struct yasl_surface *(*surface_create_toplevel)(struct yasl_private_display *d,
            const struct yasl_private_window_spec *spec, void *graphics_device);
    struct yasl_surface *(*surface_create_popup)(struct yasl_surface *parent, const struct yasl_private_popup_spec *spec);
    bool (*surface_popup_reposition)(struct yasl_surface *popup, const struct yasl_private_popup_spec *spec);
    void (*surface_destroy)(struct yasl_surface *s);
    struct input_queue *(*surface_input_queue)(struct yasl_surface *s);
    bool (*surface_set_size)(struct yasl_surface *s, struct yasl_private_vector2i size);
    bool (*surface_set_fullscreen)(struct yasl_surface *s, bool enabled);
    bool (*surface_set_maximized)(struct yasl_surface *s, bool enabled);
    bool (*surface_minimize)(struct yasl_surface *s);
    struct yasl_private_display *(*surface_display)(const struct yasl_surface *s);
    /* Copy only; must not invoke application callbacks. */
    void (*surface_set_callbacks)(struct yasl_surface *s, const struct yasl_private_window_cbs *cbs, void *ud);
    void (*surface_set_title)(struct yasl_surface *s, const char *title);
    void (*surface_schedule_redraw)(struct yasl_surface *s);
    /* Optional; true while the system has not yet shown the last presented
     * frame. Without it, frame requests are paced by the refresh interval. */
    bool (*surface_frame_in_flight)(const struct yasl_surface *s);
    bool (*surface_present)(struct yasl_surface *s, const struct yasl_private_framebuffer *fb, const struct yasl_private_damage_list *damage);
    /* Optional; NULL means only YASL_PRIVATE_POINTER_NORMAL is supported. Backends that
     * implement RELATIVE push POINTER_MOTION with delta set and pos frozen. */
    bool (*surface_set_pointer_mode)(struct yasl_surface *s, enum yasl_private_pointer_mode mode);
    struct yasl_private_geometry (*surface_geometry)(const struct yasl_surface *s);
    struct yasl_private_timing (*surface_timing)(const struct yasl_surface *s);
    uint64_t (*surface_frame_time_ns)(struct yasl_surface *s, uint64_t now);
    void (*surface_set_cursor)(struct yasl_surface *s, enum yasl_private_cursor cursor);
    void (*surface_set_background_color)(struct yasl_surface *s, uint32_t bg);
    const char *(*surface_ime_preedit)(const struct yasl_surface *s);
    bool (*surface_ime_preedit_cursor)(const struct yasl_surface *s, int *begin, int *end);
    void (*surface_set_ime_caret)(struct yasl_surface *s, struct yasl_private_rect2i caret);
    bool (*surface_start_interactive_move)(struct yasl_surface *s);
    bool (*surface_start_interactive_resize)(struct yasl_surface *s, enum yasl_private_resize_edge edge);
    bool (*surface_show_window_menu)(struct yasl_surface *s, struct yasl_private_vector2i pos);
    bool (*surface_drag_carry)(struct yasl_surface *s, struct yasl_private_vector2i offset);
    bool (*surface_place_under_pointer)(struct yasl_surface *s, struct yasl_private_vector2i at);
    /* Optional; NULL means the bell is unsupported. */
    bool (*surface_bell)(struct yasl_surface *s);
    /* Optional; NULL means no readback. Fills size only for an available frame. */
    bool (*surface_read_pixels)(struct yasl_surface *s, uint32_t *out, size_t cap,
            struct yasl_private_vector2i *size);
    void *(*display_get_native_handle)(struct yasl_private_display *d);
    void *(*surface_get_native_handle)(struct yasl_surface *s);
    bool (*surface_get_native_frame)(struct yasl_surface *s,
            const struct yasl_private_framebuffer *target, struct yasl_private_native_frame *out);
};

#if YASL_HAS_WAYLAND
extern const struct yasl_private_backend_ops yasl_private_wayland_backend;
#endif
#if YASL_HAS_X11
extern const struct yasl_private_backend_ops yasl_private_x11_backend;
#endif
#if YASL_HAS_COCOA
extern const struct yasl_private_backend_ops yasl_private_cocoa_backend;
#endif

#endif
