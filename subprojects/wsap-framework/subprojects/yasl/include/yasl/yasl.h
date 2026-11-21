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
    YASL_FEATURE_VULKAN = 512,
    YASL_FEATURE_TITLEBAR_OVERLAY = 1024 /**< Support for titlebar_overlay and title bar areas. */
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
/** Optional: creating the first surface initializes YASL with the default
 * options, as does a NULL argument. EINVAL when only one of loop and on_event
 * is set; EPERM off the loop's thread. EALREADY once initialized with the same
 * options, EBUSY with others; a failed initialization may be retried. */
YASL_API int yasl_init(const struct yasl_options *options);
/** Destroys every surface; on_event is not called again. EBUSY inside a loop
 * step or while a Vulkan surface exists. */
YASL_API int yasl_shutdown(void);
/** AUTO and 0 before initialization. */
YASL_API enum yasl_backend yasl_get_backend(void);
/** The feature bits that the backend supports. A graphics bit does not prove
 * that the loader and driver are on the machine. Zero before initialization. */
YASL_API unsigned yasl_get_features(void);
/** True when the feature works on this machine. A graphics feature probes the
 * loader on the first call, and the result is cached. A caller that does not
 * ask maps no graphics library. False before initialization. */
YASL_API bool yasl_has_feature(enum yasl_feature feature);

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
    /** Extends the content under the system title bar
     * (FEATURE_TITLEBAR_OVERLAY). width and height then include the title
     * bar. The system draws its title bar over the content, as on a window
     * without the overlay, until yasl_surface_set_titlebar_style changes it.
     * yasl_surface_get_info reports the title bar height and the button area.
     * Ignored where the feature is missing. */
    bool titlebar_overlay;
    /** Ask for these states at creation; a WINDOW event reports the result. */
    bool fullscreen, maximized, hidden;
    void *userdata;
    /** Called when the window needs drawing while the system blocks the event
     * loop, as during a macOS live resize, and when the window changes scale
     * while the application waits for events; draw and present before
     * returning. The system then shows the frame together with the new size
     * or scale. REFRESH events cover every other case. Events stay queued
     * meanwhile, so apply the size and scale from yasl_surface_get_info,
     * already current here; one WINDOW event reports them once the callback
     * returns. Waiting, creating and destroying fail with EBUSY. */
    void (*refresh)(yasl_surface *surface);
    struct yasl_gl_options gl;
};
/** The title string is copied. */
YASL_API yasl_surface *yasl_surface_create(const struct yasl_surface_options *options);
/** Also destroys its popups. NULL is a no-op. EBUSY while one has a Vulkan surface. */
YASL_API int yasl_surface_destroy(yasl_surface *surface);

enum yasl_state {
    YASL_STATE_FULLSCREEN = 1,
    YASL_STATE_MAXIMIZED = 2,
    YASL_STATE_ACTIVATED = 4,
    YASL_STATE_SUSPENDED = 8, /**< Hidden from view; skip drawing. */
    YASL_STATE_CSD = 16,      /**< No system title bar: draw your own. */
    YASL_STATE_TILED = 32,
    YASL_STATE_MINIMIZED = 64 /**< Minimized to the taskbar or dock. */
};
struct yasl_surface_info {
    int width, height, pixel_width, pixel_height;
    double scale;
    unsigned state; /**< YASL_STATE_* */
    /** The height of the system title bar over the top of a titlebar_overlay
     * window, and the area of its window buttons, in logical units from the
     * top left of the content. Draw the title bar in that height and leave the
     * button area clear. All zero without the overlay and in fullscreen. A
     * WINDOW event reports each change. */
    int titlebar_height;
    int controls_x, controls_y, controls_width, controls_height;
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
/** Puts the window so that its upper left corner is at (x, y) in logical
 * screen units. ENOTSUP where the platform places windows itself. */
YASL_API int yasl_surface_set_position(yasl_surface *surface, int x, int y);
/** Writes the logical screen position of the window and returns 0, or returns a
 * negative errno and changes nothing. ENOTSUP when the position is unknown. */
YASL_API int yasl_surface_get_position(const yasl_surface *surface, int *x, int *y);
/** Limits the size that the user can resize the window to, in logical units. A
 * zero minimum or maximum means no limit on that axis. A maximum below the
 * minimum is an EINVAL error. */
YASL_API int yasl_surface_set_size_limits(yasl_surface *surface, int min_width,
                                          int min_height, int max_width, int max_height);
/** Locks the width to height ratio of the user's resize to numerator : denominator.
 * Zero for both restores free resizing. */
YASL_API int yasl_surface_set_aspect_ratio(yasl_surface *surface, int numerator,
                                           int denominator);
/** Maps a point in the window, in logical units, to logical screen units. It
 * returns 0, or a negative errno with the outputs unchanged. ENOTSUP when the
 * platform does not report window placement. */
YASL_API int yasl_surface_client_to_screen(const yasl_surface *surface, double x, double y,
                                           double *screen_x, double *screen_y);
/** Sets how opaque the window is, from 0.0 to 1.0; 1.0 is the default. The
 * platform may refuse the value, and the window then stays at its last value. */
YASL_API int yasl_surface_set_opacity(yasl_surface *surface, double opacity);
/** Shows or hides the window without destroying it. A hidden window gets no
 * REFRESH events. */
YASL_API int yasl_surface_set_visible(yasl_surface *surface, bool visible);
/** Keeps the window above other windows, or stops it, where the platform has a
 * stacking layer for it. */
YASL_API int yasl_surface_set_always_on_top(yasl_surface *surface, bool enabled);
/** Keeps the window out of the taskbar and window list, or stops it. */
YASL_API int yasl_surface_set_skip_taskbar(yasl_surface *surface, bool enabled);
/** Asks for a REFRESH after the next frame is shown. A frame presented after
 * the request, in the same callback, is that frame, so an animation can ask
 * before it draws. The platform answers at its next display callback, or one
 * refresh interval after the last present or REFRESH when it has none. A
 * request never draws at once, so it cannot outrun the display. Requests
 * coalesce. A draw that follows a request waits for the display, so ask for one
 * frame per change. */
YASL_API int yasl_surface_request_frame(yasl_surface *surface);
/** The display frame interval of the surface, in nanoseconds. It follows the
 * display that the window is on. It is the interval of a 60 Hz display when the
 * platform has no estimate. Zero for a surface that is not valid. */
YASL_API uint64_t yasl_surface_frame_interval_ns(const yasl_surface *surface);

enum yasl_pacing {
    YASL_PACING_ON_DEMAND,  /**< Draw after an invalidation or an exposure. The default. */
    YASL_PACING_CONTINUOUS, /**< Draw at the display interval without a request. */
};
/** Sets how often the surface draws. YASL_PACING_CONTINUOUS asks for a frame
 * each display interval, for a continuous render loop. YASL_PACING_ON_DEMAND
 * restores the default. */
YASL_API int yasl_surface_set_pacing(yasl_surface *surface, enum yasl_pacing pacing);

enum yasl_cursor {
    YASL_CURSOR_ARROW, YASL_CURSOR_TEXT, YASL_CURSOR_LINK, YASL_CURSOR_CROSSHAIR, YASL_CURSOR_MOVE,
    YASL_CURSOR_RESIZE_NWSE, YASL_CURSOR_RESIZE_NESW, YASL_CURSOR_RESIZE_EW, YASL_CURSOR_RESIZE_NS,
    YASL_CURSOR_HIDDEN,
    /** Hidden and held in place while focused; POINTER events then report
     * unaccelerated motion only in dx/dy (FEATURE_LOCKED_CURSOR). */
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
/** Requires a held pointer or pen button: lets the user move the window (edges
 * 0) or resize it from one edge or a corner (two adjacent edges). */
YASL_API int yasl_surface_begin_move_resize(yasl_surface *surface, unsigned edges);
/** A rectangle in logical units from the top left of the content. */
struct yasl_rect {
    double x, y, width, height;
};
/** The parts of its title bar that the system draws over a titlebar_overlay
 * window. */
enum yasl_titlebar_style {
    /** The whole title bar, opaque, as on a window without the overlay. It
     * takes the presses in its height. The style at creation. */
    YASL_TITLEBAR_SYSTEM,
    /** The title text and the window buttons. The background and separator
     * are transparent, so the content shows through, and the content gets the
     * presses outside the title text and the buttons. */
    YASL_TITLEBAR_TRANSPARENT,
    /** The window buttons only. The content shows through and gets the
     * presses outside the buttons; the application draws the title. */
    YASL_TITLEBAR_BUTTONS
};
/** Sets the parts of its title bar that the system draws over a
 * titlebar_overlay window (FEATURE_TITLEBAR_OVERLAY). EINVAL for an unknown
 * style, a popup, or a window without the overlay. */
YASL_API int yasl_surface_set_titlebar_style(yasl_surface *surface, enum yasl_titlebar_style style);
/** Sets the parts of a toplevel that act as its title bar
 * (FEATURE_TITLEBAR_OVERLAY). Call it after each layout change of the title
 * bar that the application draws. A primary press in an area moves the
 * window. A double click zooms or minimizes it, as the system setting asks.
 * yasl consumes these presses, so the application gets no POINTER event for
 * them. Motion, other buttons, a Control press, and presses in fullscreen
 * still arrive. The list is copied and replaces the previous one; count 0
 * clears it. EINVAL for a popup, or for a rectangle with a negative or
 * non-finite value. */
YASL_API int yasl_surface_set_titlebar_areas(yasl_surface *surface,
                                             const struct yasl_rect *areas, size_t count);
YASL_API int yasl_surface_show_window_menu(yasl_surface *surface, double x, double y);
YASL_API int yasl_surface_bell(yasl_surface *surface);
/** Sets the surface background color. The bg value uses 0xAARRGGBB or
 * 0x00RRGGBB format. On macOS, this sets the window background color and
 * updates the titlebar. */
YASL_API int yasl_surface_set_background_color(yasl_surface *surface, uint32_t bg);

/* Popups */
enum yasl_popup_adjust {
    YASL_ADJUST_SLIDE_X = 1, YASL_ADJUST_SLIDE_Y = 2, YASL_ADJUST_FLIP_X = 4, YASL_ADJUST_FLIP_Y = 8,
    YASL_ADJUST_RESIZE_X = 16, YASL_ADJUST_RESIZE_Y = 32
};
/** Placed against an anchor rectangle in the parent: anchor picks a point on it
 * and gravity the direction the popup grows, both as YASL_EDGE_* bits (0 is the
 * center). adjust keeps it on screen; with 0, the platform flips the popup and
 * then slides it. */
struct yasl_popup_options {
    int width, height;
    double x, y, anchor_width, anchor_height;
    unsigned anchor, gravity, adjust;
    void *userdata;
    double content_x, content_y, content_width, content_height;
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
/** Sets the data types that external drops on `surface` can deliver, best first.
 * Each entry is a MIME type such as "text/plain;charset=utf-8", or a major type
 * with a star such as "image/" "*". A drag takes the first entry that it offers,
 * and DROP events report the offered type in drop.mime. A drag that offers no
 * entry is refused. Without a call, a surface takes "text/uri-list" only.
 * The list is copied; count 0 restores that default. */
YASL_API int yasl_surface_set_drop_types(yasl_surface *surface, const char *const *types,
                                         size_t count);

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
    YASL_EVENT_TEXT,        /**< text: typed text to insert. ime.key_id links its key. */
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
                             * drop.mime on OVER and RELEASE; text on RELEASE holds
                             * length bytes of that type, which can include NULs. */
    YASL_EVENT_DRAG,        /**< A yasl_drag_start drag: x, y; action is a yasl_drop_action.
                             * Each drag ends once: RELEASE on the surface it drops on,
                             * or CANCEL on its origin when it ends anywhere else. */
    /** The display server went away; action is a yasl_lost. Before the next
     * wait, or before returning to the loop, destroy Vulkan surfaces and detach
     * GL contexts on every thread; popups close. On RECONNECTING, DISPLAY_RESTORED
     * follows: recreate them (GL contexts are new). After FINAL, yasl_wait_event
     * fails with ENOTCONN once no event is left. */
    YASL_EVENT_DISPLAY_LOST,
    YASL_EVENT_DISPLAY_RESTORED,
    YASL_EVENT_MONITORS_CHANGED, /**< A monitor was added, removed, or changed. It
                                  * has no payload, so query the monitors again. */
    YASL_EVENT_USER        /**< user: a message from yasl_post_user_event. */
};
enum yasl_key_action { YASL_KEY_PRESS, YASL_KEY_RELEASE, YASL_KEY_REPEAT };
enum yasl_mod {
    YASL_MOD_SHIFT = 1, YASL_MOD_ALT = 2, YASL_MOD_CTRL = 4, YASL_MOD_SUPER = 8,
    YASL_MOD_CAPS = 16, YASL_MOD_HYPER = 32, YASL_MOD_META = 64,
    YASL_MOD_NUM_LOCK = 128, YASL_MOD_SCROLL_LOCK = 256,
};
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

enum yasl_key_text {
    YASL_KEY_TEXT_UNSPECIFIED, /**< The backend does not report text ownership. */
    YASL_KEY_TEXT_NONE,        /**< Native text input did not insert text. */
    YASL_KEY_TEXT_COMMIT,      /**< Native text input inserted text for this key. */
    YASL_KEY_TEXT_COMPOSITION, /**< The input method owns this key. */
};

/** Physical key, layout labels (codepoint unshifted, shifted; 0 if none) and the
 * native code (evdev, X11 keycode or macOS virtual key). Insert typed text from
 * a TEXT event, not from this one. */
struct yasl_key_event {
    enum yasl_key key;
    uint32_t codepoint, shifted, native_code;
    enum yasl_key_text text_input;
    unsigned consumed_mods; /**< Modifiers used by native text input. */
    uint64_t text_id; /**< Nonzero matches ime.key_id on the text edit for this key. */
};
/** button is the button that changed on DOWN and UP; buttons holds the pressed
 * YASL_BUTTON_BIT()s. */
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
    uint64_t key_id; /**< Zero for input method commits without a physical key. */
};
#define YASL_DROP_MIME_MAX 64
struct yasl_drop_event {
    /** The offered type that this drop delivers, from yasl_surface_set_drop_types.
     * Empty on LEAVE and CANCEL. A type that does not fit is never picked. */
    char mime[YASL_DROP_MIME_MAX];
};
/** A posted message. code, payload and size pass through unchanged. payload is
 * borrowed until its release callback runs. */
struct yasl_user_event {
    uint32_t code;
    void *payload;
    size_t size;
};
struct yasl_event {
    enum yasl_event_type type;
    yasl_surface *surface;
    uint64_t time_ns;
    unsigned action, mods; /**< mods: YASL_MOD_* for KEY, POINTER and SCROLL. */
    double x, y;
    const char *text; /**< NUL-terminated UTF-8, valid until the next wait or
                       * until on_event returns; NULL when the event has no
                       * text. A DROP RELEASE holds length bytes of drop.mime,
                       * which can include NULs. */
    size_t length;
    union {
        struct yasl_surface_info window;
        struct yasl_key_event key;
        struct yasl_pointer_event pointer;
        struct yasl_scroll_event scroll;
        struct yasl_pen_event pen;
        struct yasl_ime_event ime;
        struct yasl_drop_event drop;
        struct yasl_user_event user;
    };
};
/** 1 with an event, 0 once timeout_ms passes or after yasl_wake. 0 polls and -1
 * waits forever. On failure out->type is NONE. EBUSY when on_event is set. */
YASL_API int yasl_wait_event(yasl_event *out, int timeout_ms);
/** Makes a waiting yasl_wait_event return 0, or the next call when none waits,
 * once no event is pending. With on_event, wakes the loop. Thread-safe. */
YASL_API void yasl_wake(void);
/** Queues YASL_EVENT_USER for the next wait or on_event, from any thread. A
 * NULL surface sends it to the display. payload is borrowed until release
 * runs, after delivery or cancellation, exactly once and on the loop's thread;
 * release is optional. 0 on success; ENODEV before yasl_init, EINVAL for a
 * surface of another display, ENOENT for a destroyed surface, ENOMEM when the
 * queue cannot grow. Stop producer threads before yasl_shutdown. */
YASL_API int yasl_post_user_event(yasl_surface *surface, uint32_t code, void *payload,
                                  size_t size, void (*release)(void *payload));
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
/** Runs callback on the loop's thread during a later step, from any thread.
 * Callbacks run in posting order; one posted while they run waits for a later
 * step. 0 on success, EINVAL for NULL arguments, ENOMEM when the post cannot
 * be allocated. Use it from other threads instead of changing watches or
 * timers, which belong to the loop's thread. */
YASL_API int yasl_loop_post(yasl_loop *loop, void (*callback)(void *userdata),
                            void *userdata);
/** Calls callback with the poll(2) revents, errors and hangup included; it
 * must consume them or change the watch. Returns an ID above zero. */
YASL_API int yasl_loop_add_fd(yasl_loop *loop, int fd, short events,
                              void (*callback)(void *userdata, int fd, short revents),
                              void *userdata);
/** ENOENT for an unknown ID. Leaves the descriptor open. */
YASL_API int yasl_loop_del_fd(yasl_loop *loop, int id);
YASL_API int yasl_loop_set_fd_events(yasl_loop *loop, int id, short events);
/** Calls callback after delay_ns, 0 meaning the next step; callback returns
 * the next delay, or 0 to stop. Returns an ID that is never reused, or 0 with
 * errno. Loop thread only: another thread must post with yasl_loop_post and arm
 * the timer there. */
YASL_API uint64_t yasl_loop_add_timer(yasl_loop *loop, uint64_t delay_ns,
                                      uint64_t (*callback)(void *userdata, uint64_t id,
                                                           uint64_t delay_ns),
                                      void *userdata);
/** True when it cancels an armed timer, also from its own callback. Loop
 * thread only, as for yasl_loop_add_timer. */
YASL_API bool yasl_loop_del_timer(yasl_loop *loop, uint64_t id);
/** Calls callback on the loop's thread for each signal; repeats before the
 * call coalesce. Several loops can watch one signal, and each gets it. A loop
 * has one watch: EBUSY when this loop already watches signals. ENOMEM when too
 * many loops watch one signal. Unwatching restores the previous actions when
 * the last loop stops watching. Call on the loop's thread. */
YASL_API int yasl_loop_watch_signals(yasl_loop *loop, const int *signals, int count,
                                     void (*callback)(void *userdata, int signo),
                                     void *userdata);
YASL_API void yasl_loop_unwatch_signals(yasl_loop *loop);

/* Rendering */
/** One rectangle of a CPU surface to redraw, in pixels. */
struct yasl_damage {
    int x, y, width, height;
};
/** Returns the window's pixel_width * pixel_height premultiplied 0xAARRGGBB
 * pixels, rows tightly packed. They persist, and a resize keeps the overlap and
 * zeroes the rest. Valid until the next wait, the return of the callback that
 * got them, or the next yasl_cpu_ call. Present to show them; the system may
 * also show them when it redraws the window. */
YASL_API uint32_t *yasl_cpu_pixels(yasl_surface *surface, int *pixel_width, int *pixel_height);
YASL_API int yasl_cpu_present(yasl_surface *surface);
/** Presents the canvas with partial damage. Damage adds up across presents
 * until the surface draws; a NULL list or count 0 marks the whole surface. */
YASL_API int yasl_cpu_present_damage(yasl_surface *surface, const struct yasl_damage *damage, int count);

struct yasl_cpu_frame {
    uint32_t *pixels;
    int pixel_width, pixel_height;
    int stride_bytes;
};
/** Acquires the backend CPU buffer directly without an intermediate canvas.
 * The caller must fill every pixel before yasl_cpu_commit(). Valid until
 * yasl_cpu_commit(), yasl_cpu_discard(), or the next wait. Returns 0 on success,
 * -EAGAIN when waiting for configure or a free buffer, -ENOTSUP when direct
 * access is unavailable, or -EBUSY when a frame is already acquired. */
YASL_API int yasl_cpu_acquire(yasl_surface *surface, struct yasl_cpu_frame *out);
YASL_API int yasl_cpu_commit(yasl_surface *surface, const struct yasl_damage *damage, int count);
YASL_API void yasl_cpu_discard(yasl_surface *surface);

typedef void (*yasl_gl_proc)(void);
/** These four GL calls may run on a rendering thread; synchronize them with
 * yasl_surface_destroy(). yasl_gl_make_current(NULL) detaches the calling
 * thread's context, also after DISPLAY_LOST. */
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

/* Monitors */
/** Clockwise rotation of the content of a monitor, in degrees. */
enum yasl_orientation {
    YASL_ORIENTATION_0, YASL_ORIENTATION_90, YASL_ORIENTATION_180, YASL_ORIENTATION_270
};
#define YASL_MONITOR_NAME_MAX 64
/** Bounds and workareas are in logical screen units. The workarea is the part
 * of the bounds that no system panel or dock covers. A physical size of zero is
 * unknown. dpi is the physical dot density and scale the logical factor. */
struct yasl_monitor_info {
    char name[YASL_MONITOR_NAME_MAX];
    int x, y, width, height;
    int work_x, work_y, work_width, work_height;
    int physical_width_mm, physical_height_mm;
    double scale, dpi, refresh_hz;
    enum yasl_orientation orientation;
    bool primary;
};
/** Counts the monitors of the display. It returns 0 before yasl_init and when
 * the platform reports no monitor. */
YASL_API int yasl_get_monitor_count(void);
/** Writes the monitor at index, from 0 below yasl_get_monitor_count(). It
 * returns 0, or a negative errno: EINVAL for a bad index or a NULL out, ENOTSUP
 * when this platform cannot list monitors. */
YASL_API int yasl_get_monitor(int index, struct yasl_monitor_info *out);
/** Writes the primary monitor. It returns 0, or a negative errno: ENOTSUP when
 * the platform cannot list monitors, ENOENT when it reports no primary one. */
YASL_API int yasl_get_primary_monitor(struct yasl_monitor_info *out);

/* Files */
/** Standard folders of the user. PREFERENCE, DATA and CACHE belong to the app
 * and use its app ID. The others belong to the user. */
enum yasl_user_dir {
    YASL_DIR_PREFERENCE, YASL_DIR_DATA, YASL_DIR_CACHE,
    YASL_DIR_HOME, YASL_DIR_DOCUMENTS, YASL_DIR_DOWNLOADS, YASL_DIR_DESKTOP
};
/** Returns the folder path length, or 0 with errno. Writes it with a NUL only
 * if capacity exceeds the length. Creates none of the folders. Thread-safe. */
YASL_API size_t yasl_get_user_dir(enum yasl_user_dir which, char *out, size_t capacity);
/** Size in bytes and modification time in nanoseconds since the epoch. */
struct yasl_file_info {
    uint64_t size;
    int64_t modified_ns;
    bool is_file, is_dir, is_symlink; /**< is_symlink describes the link itself. */
};
/** Reads the state of path, which may be relative to the working directory. It
 * follows symbolic links. It returns 0, or a negative errno such as ENOENT. */
YASL_API int yasl_file_stat(const char *path, struct yasl_file_info *out);
typedef struct yasl_dir yasl_dir;
#define YASL_DIR_NAME_MAX 256
struct yasl_dir_entry {
    char name[YASL_DIR_NAME_MAX];
    struct yasl_file_info info;
};
/** Opens a directory for listing; NULL with errno. The dot entries are skipped.
 * One listing can be open more than once. Destroy it with yasl_dir_close. */
YASL_API yasl_dir *yasl_dir_open(const char *path);
/** Reads the next entry into out. It returns 1 for an entry, 0 at the end, or a
 * negative errno. A name that does not fit is skipped. */
YASL_API int yasl_dir_next(yasl_dir *dir, struct yasl_dir_entry *out);
/** Also valid for NULL. */
YASL_API void yasl_dir_close(yasl_dir *dir);
/** Loads the file name of an app asset into memory, with a NUL after the data.
 * It looks in the app bundle's resource directory, then next to the executable,
 * then in the working directory. On success returns 0, sets *data to a buffer
 * of *size bytes that free() releases, and *size to the length. Returns ENOENT
 * when no such asset exists, or another negative errno. Thread-safe. */
YASL_API int yasl_load_asset(const char *name, void **data, size_t *size);

/* Desktop */
enum yasl_appearance { YASL_APPEARANCE_DARK = 1, YASL_APPEARANCE_LIGHT = 2, YASL_APPEARANCE_HIGH_CONTRAST = 4 };
/** Opens uri with its default application. May block briefly on the session
 * bus. Thread-safe. */
YASL_API int yasl_open_uri(const char *uri);
/** May block briefly on the session bus. */
YASL_API int yasl_notify(const char *title, const char *body);
/** Returns 0 for no preference. */
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
