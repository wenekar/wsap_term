/** @file YASL: desktop windows, input and events for application-owned rendering.
 *
 * Calls return 0 or a negative errno; constructors return NULL and set errno;
 * queries name their fallback. Errors: EINVAL bad argument, ENOTSUP not
 * supported here, EPERM wrong thread, ENODEV not initialized, ENOTCONN display
 * lost, EBUSY not allowed now, ESTALE needs a pointer press in progress.
 * Call from the thread that initialized YASL (the main thread on macOS) unless
 * a function says it is thread-safe. Positions and sizes are logical units;
 * multiply by the scale for pixels. Names with "pixel" are in pixels. */
#ifndef YASL_H
#define YASL_H
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "yasl/keycodes.h"
#if defined(__GNUC__)
#define YASL_API __attribute__((visibility("default")))
#else
#define YASL_API
#endif
#define YASL_VERSION_MAJOR 0
#define YASL_VERSION_MINOR 2
#define YASL_VERSION_PATCH 0
#ifdef __cplusplus
extern "C" {
#endif
typedef struct yasl_surface yasl_surface;
typedef struct yasl_event yasl_event;
typedef struct yasl_loop yasl_loop;

/* Lifecycle */
enum yasl_backend { YASL_BACKEND_AUTO, YASL_BACKEND_WAYLAND, YASL_BACKEND_X11, YASL_BACKEND_COCOA };
enum yasl_feature {
    YASL_FEATURE_POPUPS = 1, YASL_FEATURE_DRAG = 2, YASL_FEATURE_DRAG_CARRY = 4,
    YASL_FEATURE_PLACE_WINDOW = 8, YASL_FEATURE_PRIMARY_SELECTION = 16, YASL_FEATURE_TABLET = 32,
    YASL_FEATURE_TEXT_INPUT = 64, YASL_FEATURE_LOCKED_CURSOR = 128, YASL_FEATURE_OPENGL = 256,
    YASL_FEATURE_VULKAN = 512
};
/** Zero is the default: AUTO, no app ID, and yasl_wait_event delivers events.
 * app_id is a reverse-DNS name for window grouping and notifications. Set loop
 * and on_event together to run the display on an application loop: each
 * yasl_loop_step then delivers events to on_event, and yasl_wait_event fails
 * with EBUSY. */
struct yasl_options {
    enum yasl_backend backend;
    const char *app_id;
    yasl_loop *loop;
    void (*on_event)(void *userdata, const yasl_event *event);
    void *userdata;
};
/** Optional: the first surface initializes with defaults; NULL options too.
 * EINVAL when only one of loop and on_event is set; EPERM off the loop's
 * thread. EALREADY once initialized with the same options, EBUSY with others;
 * a failed initialization may be retried. */
YASL_API int yasl_init(const struct yasl_options *options);
/** Destroys every surface; on_event is not called again. EBUSY inside a loop
 * step or while a Vulkan surface exists. */
YASL_API int yasl_shutdown(void);
/** AUTO and 0 before initialization. */
YASL_API enum yasl_backend yasl_get_backend(void);
YASL_API unsigned yasl_get_features(void);

/* Surfaces */
enum yasl_surface_type {
    YASL_SURFACE_CPU,   /**< Draw into yasl_cpu_pixels. */
    YASL_SURFACE_GL,    /**< OpenGL through EGL; Linux only. */
    YASL_SURFACE_NATIVE /**< Draw with Vulkan, Metal or the native handles. */
};
/** Zero selects OpenGL 3.2 core (ES 3.0 with es), RGBA8, 24-bit depth, 8-bit stencil. */
struct yasl_gl_options {
    int major, minor, samples;
    bool es, compatibility, debug, srgb, no_depth_stencil;
    yasl_surface *share; /**< A GL surface whose objects the new context shares. */
};
struct yasl_surface_options {
    const char *title;
    int width, height; /**< Required. */
    enum yasl_surface_type type;
    bool undecorated, fixed_size, tearing;
    void *userdata;
    /** Called when the window needs drawing while the system blocks the event
     * loop, as during a macOS live resize; draw and present before returning.
     * REFRESH events cover every other case. Events stay queued meanwhile, so
     * apply the size from yasl_surface_get_info, already current here; one
     * WINDOW event reports the final size once the system's loop returns.
     * Waiting, creating and destroying fail with EBUSY. */
    void (*refresh)(yasl_surface *surface);
    struct yasl_gl_options gl;
};
/** Strings are copied. */
YASL_API yasl_surface *yasl_surface_create(const struct yasl_surface_options *options);
/** Also destroys its popups. NULL is a no-op. EBUSY while one has a Vulkan surface. */
YASL_API int yasl_surface_destroy(yasl_surface *surface);

enum yasl_state {
    YASL_STATE_FULLSCREEN = 1,
    YASL_STATE_MAXIMIZED = 2,
    YASL_STATE_ACTIVATED = 4,
    YASL_STATE_SUSPENDED = 8, /**< Hidden from view; skip drawing. */
    YASL_STATE_CSD = 16,      /**< No system title bar: draw your own. */
    YASL_STATE_TILED = 32
};
struct yasl_surface_info {
    int width, height, pixel_width, pixel_height;
    double scale;
    unsigned state; /**< YASL_STATE_* */
};
/** Zero sizes and scale 1 for NULL. */
YASL_API struct yasl_surface_info yasl_surface_get_info(const yasl_surface *surface);
YASL_API void *yasl_surface_get_userdata(const yasl_surface *surface);
YASL_API int yasl_surface_set_userdata(yasl_surface *surface, void *userdata);
YASL_API int yasl_surface_set_title(yasl_surface *surface, const char *title);

/** Window requests; the outcome arrives as a WINDOW event. RESTORE leaves
 * fullscreen and maximized. */
enum yasl_request { YASL_REQUEST_RESTORE, YASL_REQUEST_FULLSCREEN, YASL_REQUEST_MAXIMIZE, YASL_REQUEST_MINIMIZE };
YASL_API int yasl_surface_request(yasl_surface *surface, enum yasl_request request);
YASL_API int yasl_surface_request_size(yasl_surface *surface, int width, int height);
/** Asks for a REFRESH after the next frame is shown. A frame presented after
 * the request, in the same callback, is that frame, so an animation can ask
 * before it draws. With no frame in flight, the REFRESH comes one refresh
 * interval after the last present or REFRESH. Requests coalesce. Use it to
 * pace animation; draw at once for input. */
YASL_API int yasl_surface_request_frame(yasl_surface *surface);

enum yasl_cursor {
    YASL_CURSOR_ARROW, YASL_CURSOR_TEXT, YASL_CURSOR_LINK, YASL_CURSOR_CROSSHAIR, YASL_CURSOR_MOVE,
    YASL_CURSOR_RESIZE_NWSE, YASL_CURSOR_RESIZE_NESW, YASL_CURSOR_RESIZE_EW, YASL_CURSOR_RESIZE_NS,
    YASL_CURSOR_HIDDEN,
    /** Hidden and held in place while focused; POINTER events then report
     * motion only in dx/dy (FEATURE_LOCKED_CURSOR). */
    YASL_CURSOR_LOCKED
};
YASL_API int yasl_surface_set_cursor(yasl_surface *surface, enum yasl_cursor cursor);

enum yasl_text_purpose { YASL_TEXT_NORMAL, YASL_TEXT_PASSWORD, YASL_TEXT_NUMBER, YASL_TEXT_TERMINAL };
/** The focused text field, for input methods. text is UTF-8 around the caret,
 * selection offsets are bytes into it, and revision comes back in COMPOSITION
 * and TEXT_EDIT events. The caret rectangle places the candidate window. */
struct yasl_text_input {
    enum yasl_text_purpose purpose;
    const char *text;
    size_t length, selection_begin, selection_end;
    uint64_t revision;
    double caret_x, caret_y, caret_width, caret_height;
};
/** Copied. NULL turns input methods off. */
YASL_API int yasl_surface_set_text_input(yasl_surface *surface, const struct yasl_text_input *input);

enum yasl_edge { YASL_EDGE_TOP = 1, YASL_EDGE_BOTTOM = 2, YASL_EDGE_LEFT = 4, YASL_EDGE_RIGHT = 8 };
/** While a pointer or pen button is held: let the user move the window (edges 0)
 * or resize it from one edge or a corner (two adjacent edges). */
YASL_API int yasl_surface_begin_move_resize(yasl_surface *surface, unsigned edges);
YASL_API int yasl_surface_show_window_menu(yasl_surface *surface, double x, double y);
YASL_API int yasl_surface_bell(yasl_surface *surface);
/** Sets the surface background color. The bg value uses 0xAARRGGBB or
 * 0x00RRGGBB format. On macOS, this sets the window background color and
 * updates the titlebar. Returns 0 on success, or a negative errno on failure. */
YASL_API int yasl_surface_set_background_color(yasl_surface *surface, uint32_t bg);

/* Popups */
enum yasl_popup_adjust {
    YASL_ADJUST_SLIDE_X = 1, YASL_ADJUST_SLIDE_Y = 2, YASL_ADJUST_FLIP_X = 4, YASL_ADJUST_FLIP_Y = 8,
    YASL_ADJUST_RESIZE_X = 16, YASL_ADJUST_RESIZE_Y = 32
};
/** Placed against an anchor rectangle in the parent: anchor picks a point on it
 * and gravity the direction the popup grows, both as YASL_EDGE_* bits (0 is the
 * center). adjust keeps it on screen; 0 flips, then slides. */
struct yasl_popup_options {
    int width, height;
    double x, y, anchor_width, anchor_height;
    unsigned anchor, gravity, adjust;
    void *userdata;
};
/** A CPU surface. Create it while handling the input that opens it; it takes
 * the input grab and gets CLOSE when dismissed. */
YASL_API yasl_surface *yasl_popup_create(yasl_surface *parent, const struct yasl_popup_options *options);
YASL_API int yasl_popup_reposition(yasl_surface *popup, const struct yasl_popup_options *options);

/* Clipboard, drag and drop */
enum yasl_selection { YASL_CLIPBOARD, YASL_PRIMARY };
/** Offers copied UTF-8 text; call while handling input on one of your windows. */
YASL_API int yasl_clipboard_set(enum yasl_selection selection, const char *text);
/** The text arrives as a CLIPBOARD event on target, in request order. */
YASL_API int yasl_clipboard_request(yasl_surface *target, enum yasl_selection selection);
/** Premultiplied 0xAARRGGBB pixels, rows tightly packed; copied. */
struct yasl_drag_image {
    const uint32_t *pixels;
    int pixel_width, pixel_height, hotspot_x, hotspot_y;
};
/** While a pointer button is held on origin: drag it over YASL surfaces, which
 * get DRAG events. image may be NULL. */
YASL_API int yasl_drag_start(yasl_surface *origin, const struct yasl_drag_image *image);
YASL_API int yasl_drag_cancel(void);
/** During a drag: carry this toplevel with (x, y) under the pointer (FEATURE_DRAG_CARRY). */
YASL_API int yasl_drag_carry(yasl_surface *surface, double x, double y);
/** Move the toplevel so (x, y) is under the pointer (FEATURE_PLACE_WINDOW). */
YASL_API int yasl_surface_place_under_pointer(yasl_surface *surface, double x, double y);
/** Next local path in a DROP event's text/uri-list. Returns its length, or 0 at
 * the end. If capacity exceeds the length, writes it with a NUL and advances
 * cursor; otherwise changes nothing, so retry larger. Thread-safe. */
YASL_API size_t yasl_drop_next_path(const char **cursor, char *out, size_t capacity);

/* Events */
enum yasl_event_type {
    YASL_EVENT_NONE,
    YASL_EVENT_CLOSE,       /**< The user asked to close the window; destroy it to comply. */
    YASL_EVENT_QUIT,        /**< The system asked the app to quit; exit to comply. */
    YASL_EVENT_REOPEN,      /**< macOS: the app was activated with no window open. */
    YASL_EVENT_WINDOW,      /**< window: size, scale or state changed. It reports the
                             * current geometry, so changes made since the last
                             * one fold into it. */
    YASL_EVENT_REFRESH,     /**< Draw now: once sized, after every resize, and on expose. */
    YASL_EVENT_KEY,         /**< key; action is a yasl_key_action. */
    YASL_EVENT_TEXT,        /**< text: typed text to insert. */
    YASL_EVENT_COMPOSITION, /**< text: input method preedit to show at the caret,
                             * ime.begin..end its cursor; empty ends it. */
    YASL_EVENT_TEXT_EDIT,   /**< text: replaces bytes ime.begin..end of the
                             * yasl_text_input text with revision ime.revision. */
    YASL_EVENT_POINTER,     /**< pointer, x, y; action is a yasl_pointer_action. */
    YASL_EVENT_SCROLL,      /**< scroll, x, y. */
    YASL_EVENT_PEN,         /**< pen, x, y; action is a yasl_pen_action. */
    YASL_EVENT_INPUT_RESET, /**< Forget held keys, buttons and strokes. */
    YASL_EVENT_CLIPBOARD,   /**< text; action is a yasl_transfer. */
    YASL_EVENT_DROP,        /**< External data: x, y; action is a yasl_drop_action;
                             * text on RELEASE is a text/uri-list. */
    YASL_EVENT_DRAG,        /**< A yasl_drag_start drag: x, y; action is a yasl_drop_action.
                             * Each drag ends once: RELEASE on the surface it drops on,
                             * or CANCEL on its origin when it ends anywhere else. */
    /** The display server went away; action is a yasl_lost. Before the next
     * wait, or before returning to the loop, destroy Vulkan surfaces and detach
     * GL contexts on every thread; popups close. On RECONNECTING, DISPLAY_RESTORED
     * follows: recreate them (GL contexts are new). After FINAL, yasl_wait_event
     * fails with ENOTCONN once no event is left. */
    YASL_EVENT_DISPLAY_LOST,
    YASL_EVENT_DISPLAY_RESTORED
};
enum yasl_key_action { YASL_KEY_PRESS, YASL_KEY_RELEASE, YASL_KEY_REPEAT };
enum yasl_mod { YASL_MOD_SHIFT = 1, YASL_MOD_ALT = 2, YASL_MOD_CTRL = 4, YASL_MOD_SUPER = 8, YASL_MOD_CAPS = 16 };
enum yasl_pointer_action { YASL_POINTER_MOVE, YASL_POINTER_DOWN, YASL_POINTER_UP, YASL_POINTER_LEAVE };
enum yasl_button { YASL_BUTTON_PRIMARY, YASL_BUTTON_MIDDLE, YASL_BUTTON_SECONDARY, YASL_BUTTON_BACK, YASL_BUTTON_FORWARD };
#define YASL_BUTTON_BIT(button) (1u << (button))
enum yasl_pen_action { YASL_PEN_ENTER, YASL_PEN_LEAVE, YASL_PEN_MOVE, YASL_PEN_DOWN, YASL_PEN_UP, YASL_PEN_BUTTON, YASL_PEN_CANCEL };
enum yasl_pen_tool { YASL_PEN_UNKNOWN, YASL_PEN_PEN, YASL_PEN_ERASER, YASL_PEN_BRUSH, YASL_PEN_AIRBRUSH, YASL_PEN_PENCIL };
enum yasl_pen_axis {
    YASL_PEN_PRESSURE = 1, YASL_PEN_TILT = 2, YASL_PEN_ROTATION = 4, YASL_PEN_DISTANCE = 8,
    YASL_PEN_TANGENTIAL = 16, YASL_PEN_WHEEL = 32
};
enum yasl_scroll_source { YASL_SCROLL_UNKNOWN, YASL_SCROLL_WHEEL, YASL_SCROLL_FINGER, YASL_SCROLL_CONTINUOUS };
enum yasl_scroll_phase { YASL_SCROLL_PHASE_NONE, YASL_SCROLL_BEGIN, YASL_SCROLL_UPDATE, YASL_SCROLL_END };
enum yasl_transfer { YASL_TRANSFER_OK, YASL_TRANSFER_UNAVAILABLE, YASL_TRANSFER_FAILED };
/** CANCEL ends a drag that dropped on no YASL surface, or was canceled; x and y are 0. */
enum yasl_drop_action { YASL_DROP_OVER, YASL_DROP_LEAVE, YASL_DROP_RELEASE, YASL_DROP_CANCEL };
enum yasl_lost { YASL_LOST_RECONNECTING, YASL_LOST_FINAL };

/** Physical key, layout labels (codepoint unshifted, shifted; 0 if none) and the
 * native code (evdev, X11 keycode or macOS virtual key). Type with TEXT, not this. */
struct yasl_key_event {
    enum yasl_key key;
    uint32_t codepoint, shifted, native_code;
};
/** button changed on DOWN/UP; buttons are held YASL_BUTTON_BIT()s. */
struct yasl_pointer_event {
    double dx, dy;
    unsigned button, buttons;
};
/** Positive right/down; a delta is 0 when the device does not report that unit. */
struct yasl_scroll_event {
    double lines_x, lines_y, pixels_x, pixels_y;
    enum yasl_scroll_source source;
    enum yasl_scroll_phase phase;
    bool momentum;
};
/** pressure and distance 0..1, tangential -1..1, tilt and rotation in degrees.
 * axes holds the valid YASL_PEN_* axes; button changed on BUTTON. */
struct yasl_pen_event {
    uint64_t device_id, tool_id;
    double pressure, tilt_x, tilt_y, rotation, distance, tangential, wheel_degrees;
    enum yasl_pen_tool tool;
    unsigned axes, buttons, button;
    bool in_contact;
};
struct yasl_ime_event {
    size_t begin, end;
    uint64_t revision;
};
struct yasl_event {
    enum yasl_event_type type;
    yasl_surface *surface;
    uint64_t time_ns;
    unsigned action, mods; /**< mods: YASL_MOD_* for KEY, POINTER and SCROLL. */
    double x, y;
    const char *text; /**< NUL-terminated UTF-8 until the next wait or until
                       * on_event returns, or NULL. */
    size_t length;
    union {
        struct yasl_surface_info window;
        struct yasl_key_event key;
        struct yasl_pointer_event pointer;
        struct yasl_scroll_event scroll;
        struct yasl_pen_event pen;
        struct yasl_ime_event ime;
    };
};
/** 1 with an event, 0 once timeout_ms passes or after yasl_wake. 0 polls and -1
 * waits forever. On failure out->type is NONE. EBUSY with on_event. */
YASL_API int yasl_wait_event(yasl_event *out, int timeout_ms);
/** Makes a waiting, or the next, yasl_wait_event return 0 once no event is
 * pending. With on_event, wakes the loop. Thread-safe. */
YASL_API void yasl_wake(void);
/** Monotonic nanoseconds, the clock of event times. Thread-safe. */
YASL_API uint64_t yasl_time_ns(void);

/* Loop: one wait for the whole application. Give it to yasl_init to run the
 * display on it; it also runs descriptors, timers and signals without one.
 * Call it and get its callbacks on the thread that created it. */
/** NULL with errno on failure. */
YASL_API yasl_loop *yasl_loop_create(void);
/** EBUSY while a step runs or the display runs on it. Leaves descriptors open. */
YASL_API int yasl_loop_destroy(yasl_loop *loop);
/** Waits at most timeout_ms (0 polls, -1 waits) and runs what is ready:
 * display events, then descriptors, then due timers, then signals. Returns
 * how many descriptor and timer callbacks ran. EBUSY inside a step; EPERM off
 * the loop's thread. */
YASL_API int yasl_loop_step(yasl_loop *loop, int timeout_ms);
/** Steps until yasl_loop_quit. A quit made before the run ends it at once. */
YASL_API int yasl_loop_run(yasl_loop *loop);
YASL_API void yasl_loop_quit(yasl_loop *loop);
/** Calls callback with the poll(2) revents, errors and hangup included; it
 * must consume them or change the watch. Returns an ID above zero. */
YASL_API int yasl_loop_add_fd(yasl_loop *loop, int fd, short events,
                              void (*callback)(void *userdata, int fd, short revents),
                              void *userdata);
/** ENOENT for an unknown ID. Leaves the descriptor open. */
YASL_API int yasl_loop_del_fd(yasl_loop *loop, int id);
YASL_API int yasl_loop_set_fd_events(yasl_loop *loop, int id, short events);
/** Calls callback after delay_ns, 0 meaning the next step. It returns the next
 * delay, or 0 to stop. Returns an ID that is never reused, or 0 with errno. */
YASL_API uint64_t yasl_loop_add_timer(yasl_loop *loop, uint64_t delay_ns,
                                      uint64_t (*callback)(void *userdata, uint64_t id,
                                                           uint64_t delay_ns),
                                      void *userdata);
/** True when it cancels an armed timer, also from its own callback. */
YASL_API bool yasl_loop_del_timer(yasl_loop *loop, uint64_t id);
/** Calls callback on the loop's thread for each signal; repeats before the
 * call coalesce. EBUSY when this loop already watches signals or another loop
 * watches one of them. Unwatching restores the previous actions. */
YASL_API int yasl_loop_watch_signals(yasl_loop *loop, const int *signals, int count,
                                     void (*callback)(void *userdata, int signo),
                                     void *userdata);
YASL_API void yasl_loop_unwatch_signals(yasl_loop *loop);

/* Rendering */
/** CPU surfaces: the window's pixel_width * pixel_height premultiplied
 * 0xAARRGGBB pixels, rows tightly packed. They persist, and a resize keeps the
 * overlap and zeroes the rest. Valid until the next wait, the return of the
 * callback that got them, or the next yasl_cpu_ call. Present to show them;
 * the system may also show them when it redraws the window. */
struct yasl_damage {
    int x, y, width, height;
};
YASL_API uint32_t *yasl_cpu_pixels(yasl_surface *surface, int *pixel_width, int *pixel_height);
YASL_API int yasl_cpu_present(yasl_surface *surface);
YASL_API int yasl_cpu_present_damage(yasl_surface *surface, const struct yasl_damage *damage, int count);

typedef void (*yasl_gl_proc)(void);
/** These four may run on a rendering thread; synchronize them with destruction.
 * NULL detaches the calling thread's context, also after DISPLAY_LOST. */
YASL_API int yasl_gl_make_current(yasl_surface *surface);
YASL_API yasl_gl_proc yasl_gl_get_proc_address(const char *name);
YASL_API int yasl_gl_set_swap_interval(yasl_surface *surface, int interval);
YASL_API int yasl_gl_present(yasl_surface *surface);

struct wl_display;
struct wl_surface;
struct _XDisplay;
/** Borrowed handles of the active backend; the rest are zero. They expire with
 * the surface or on DISPLAY_LOST. Never destroy, dispatch or close them. */
struct yasl_native {
    struct wl_display *wl_display;
    struct wl_surface *wl_surface;
    struct _XDisplay *x11_display;
    unsigned long x11_window;
    void *ns_window, *ns_view; /**< NSWindow *, NSView * */
};
YASL_API int yasl_surface_get_native(const yasl_surface *surface, struct yasl_native *out);
/** NATIVE surfaces on macOS: the view's CAMetalLayer, reserving the surface for
 * Metal. YASL sizes it; choose the device and format, and present yourself. */
YASL_API void *yasl_metal_get_layer(yasl_surface *surface);

/* Desktop */
enum yasl_appearance { YASL_APPEARANCE_DARK = 1, YASL_APPEARANCE_LIGHT = 2, YASL_APPEARANCE_HIGH_CONTRAST = 4 };
/** Opens uri with its default application. Thread-safe. */
YASL_API int yasl_open_uri(const char *uri);
YASL_API int yasl_notify(const char *title, const char *body);
/** These two may block briefly on the session bus. 0 is no preference. */
YASL_API unsigned yasl_get_appearance(void);
/** Milliseconds; 400 when unknown. */
YASL_API int yasl_get_double_click_ms(void);
/** LOCALE is a POSIX name such as en_US; RESOURCE_DIR is macOS only. */
enum yasl_system_string { YASL_EXECUTABLE_PATH, YASL_RESOURCE_DIR, YASL_LOCALE };
/** Returns the length, or 0 with errno. Writes it with a NUL only if capacity
 * exceeds the length. Thread-safe. */
YASL_API size_t yasl_get_system_string(enum yasl_system_string which, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
