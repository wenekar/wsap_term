/**
 * @file
 * Portable desktop surfaces, CPU drawing, input, and display dispatch.
 *
 * All calls and callbacks use the display's opening thread (the main thread on
 * Cocoa), unless explicitly documented otherwise. Userdata is borrowed; callback
 * tables and creation specs are copied. The display drives its loop: each
 * yasl_loop_step dispatches it. Do not recursively step or flush, or destroy a
 * display/loop while a step or a callback is active. Destroy child
 * popups before parents, surfaces before displays, displays before borrowed loops.
 * Surface destruction from its own callback is supported; stop using it immediately.
 *
 * Zero-initialize option and callback structs before setting fields. Pointers to
 * destroyed objects are invalid, even for getters and destroy functions. NULL
 * handling is documented per function; it does not make freed pointers safe.
 * Only functions explicitly documenting errno guarantee a diagnostic there.
 * Other boolean results report success/availability; void setters are best effort.
 * New window requests, clipboard operations, frame presentation, and posting
 * document errno on failure. Event strings belong to the display queue.
 *
 * Toplevel creation sizes use logical pixels. Geometry, framebuffers, pointer
 * positions, damage, IME carets, popup specs, and drag offsets use device pixels.
 * Coordinates have their origin at the upper left; x grows right and y grows down.
 * Scale is an integer ratio with YASL_PRIVATE_SCALE_UNIT (120) representing 1x.
 * See the application guide for the display-wide dispatch/poll/flush cycle
 * and backend limits.
 */
#ifndef YASL_PRIVATE_H
#define YASL_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <float.h>
#include <math.h>

#include "common/private_timing.h"
#include "yasl/yasl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct yasl_private_display;
struct yasl_surface;
struct yasl_loop;

/* Geometry */
/** A two-dimensional integer size, position, or offset; units depend on the caller. */
struct yasl_private_vector2i {
    /** Horizontal coordinate or width. */
    int x;
    /** Vertical coordinate or height. */
    int y;
};

/** Two-dimensional fractional offset; units depend on the containing structure. */
struct yasl_private_vector2d {
    /** Horizontal component. */
    double x;
    /** Vertical component. */
    double y;
};

/** Integer rectangle; origin is upper left and extents must be nonnegative. */
struct yasl_private_rect2i {
    /** Left coordinate. */
    int x;
    /** Top coordinate. */
    int y;
    /** Nonnegative horizontal extent. */
    int width;
    /** Nonnegative vertical extent. */
    int height;
};

/** Scale numerator representing 1x. */
#define YASL_PRIVATE_SCALE_UNIT 120
/** Backend minimum logical dimension used when constraining toplevel sizes. */
#define YASL_PRIVATE_MIN_LOGICAL_PX 64

/* Nearest integer, ties away from zero. Zero scale returns zero; finite
 * out-of-range results saturate. Non-finite logical coordinates return zero. */
/** Convert a logical coordinate to device pixels, rounding ties away from zero.
 * @param logical Logical coordinate; non-finite values return zero.
 * @param scale_120 Scale numerator; zero returns zero.
 * @return Rounded device coordinate, saturated to the range of int.
 */
static inline int yasl_private_scale_to_device(double logical, uint32_t scale_120)
{
    if (!scale_120 || !(logical >= -DBL_MAX && logical <= DBL_MAX)) return 0;
    double px = (scale_120 == YASL_PRIVATE_SCALE_UNIT) ? logical : (logical * ((double)scale_120 / YASL_PRIVATE_SCALE_UNIT));
    if (px >= INT_MAX) return INT_MAX;
    if (px <= INT_MIN) return INT_MIN;
    return (int)(px + copysign(0.5, px));
}

/** Convert a device coordinate to logical pixels, rounding ties away from zero.
 * @param device Device coordinate.
 * @param scale_120 Scale numerator; zero returns zero.
 * @return Rounded logical coordinate, saturated to the range of int.
 */
static inline int yasl_private_scale_to_logical(int device, uint32_t scale_120)
{
    if (!scale_120) return 0;
    if (scale_120 == YASL_PRIVATE_SCALE_UNIT) return device;
    int64_t px = (int64_t)device * YASL_PRIVATE_SCALE_UNIT;
    int64_t half = (int64_t)(scale_120 / 2);
    int64_t bias = (px < 0) ? -half : half;
    px = (px + bias) / scale_120;
    if (px > INT_MAX) return INT_MAX;
    if (px < INT_MIN) return INT_MIN;
    return (int)px;
}

/* Capabilities */
/** Optional backend/server features, returned as a bitmask by yasl_private_display_caps(). */
enum yasl_private_display_cap : uint32_t {
    YASL_PRIVATE_CAP_POPUPS            = 1 << 0, /**< Popup creation and placement. */
    YASL_PRIVATE_CAP_DRAG              = 1 << 1, /**< Surface-drag source and target support. */
    YASL_PRIVATE_CAP_DRAG_CARRY        = 1 << 2, /**< Carry a toplevel during a drag. */
    YASL_PRIVATE_CAP_PLACE_WINDOW      = 1 << 3, /**< Place a window relative to the pointer. */
    YASL_PRIVATE_CAP_PRIMARY_SELECTION = 1 << 4, /**< Separate primary selection. */
    YASL_PRIVATE_CAP_CSD               = 1 << 5, /**< Backend advertises client decoration support; check each action result. */
    YASL_PRIVATE_CAP_NATIVE_GRAPHICS   = 1 << 6, /**< Application-managed native rendering. */
    YASL_PRIVATE_CAP_FRAME_PACING      = 1 << 7, /**< Compositor/display-driven draws. */
    /* Backend/protocol supports tablet axes, independent of attached hardware. */
    YASL_PRIVATE_CAP_TABLET            = 1 << 8,
    YASL_PRIVATE_CAP_RELATIVE_POINTER  = 1 << 9, /**< YASL_PRIVATE_POINTER_RELATIVE is available. */
    YASL_PRIVATE_CAP_TEXT_INPUT        = 1 << 11, /**< Input method support. */
};

/** Independent selections. An unsupported selection never aliases the clipboard. */
enum yasl_private_selection : uint8_t {
    YASL_PRIVATE_CLIPBOARD, /**< Explicit copy/paste selection. */
    YASL_PRIVATE_PRIMARY, /**< Independent primary selection, if supported. */
};

/** Reported surface state flags; absent flags may also mean unsupported state. */
enum yasl_private_surface_state : uint32_t {
    YASL_PRIVATE_STATE_FULLSCREEN = 1 << 0, /**< Fullscreen. */
    YASL_PRIVATE_STATE_MAXIMIZED  = 1 << 1, /**< Maximized. */
    YASL_PRIVATE_STATE_ACTIVATED  = 1 << 2, /**< Active window. */
    YASL_PRIVATE_STATE_SUSPENDED  = 1 << 3, /**< Compositor reports suspension. */
    YASL_PRIVATE_STATE_CSD        = 1 << 4, /**< Backend reports client-side decorations. */
    YASL_PRIVATE_STATE_TILED      = 1 << 5, /**< At least one edge is tiled; currently reported by Wayland. */
};

/** System cursor shapes. HIDDEN preserves pointer input. */
enum yasl_private_cursor {
    YASL_PRIVATE_CURSOR_ARROW, /**< Default arrow. */
    YASL_PRIVATE_CURSOR_TEXT, /**< Text insertion. */
    YASL_PRIVATE_CURSOR_LINK, /**< Link/pointer hand. */
    YASL_PRIVATE_CURSOR_CROSSHAIR, /**< Crosshair. */
    YASL_PRIVATE_CURSOR_MOVE, /**< Move in any direction. */
    YASL_PRIVATE_CURSOR_RESIZE_NWSE, /**< Northwest/southeast resize. */
    YASL_PRIVATE_CURSOR_RESIZE_NESW, /**< Northeast/southwest resize. */
    YASL_PRIVATE_CURSOR_RESIZE_EW, /**< Horizontal resize. */
    YASL_PRIVATE_CURSOR_RESIZE_NS, /**< Vertical resize. */
    YASL_PRIVATE_CURSOR_HIDDEN, /**< Hidden system cursor; pointer input continues. */
};

/** Valid interactive resize edges; combinations are restricted to the named values. */
enum yasl_private_resize_edge : uint32_t {
    YASL_PRIVATE_EDGE_NONE         = 0, /**< No resize edge; rejected by interactive resize. */
    YASL_PRIVATE_EDGE_TOP          = 1, /**< Top edge. */
    YASL_PRIVATE_EDGE_BOTTOM       = 2, /**< Bottom edge. */
    YASL_PRIVATE_EDGE_LEFT         = 4, /**< Left edge. */
    YASL_PRIVATE_EDGE_TOP_LEFT     = 5, /**< Top-left corner. */
    YASL_PRIVATE_EDGE_BOTTOM_LEFT  = 6, /**< Bottom-left corner. */
    YASL_PRIVATE_EDGE_RIGHT        = 8, /**< Right edge. */
    YASL_PRIVATE_EDGE_TOP_RIGHT    = 9, /**< Top-right corner. */
    YASL_PRIVATE_EDGE_BOTTOM_RIGHT = 10, /**< Bottom-right corner. */
};

/* Damage & Framebuffer
 * CPU pixels are tightly packed premultiplied ARGB8888 (0xAARRGGBB).
 * Size, pointer positions, IME caret and damage use device pixels; surface
 * toplevel creation sizes use logical pixels. Damage is a hint: backends may upload
 * the whole frame. Every pixel must contain valid content on each draw.
 */
/** Maximum number of damage rectangles in one presentation. */
#define YASL_PRIVATE_DAMAGE_MAX 64

/** Damage hints for one presentation; every target still needs complete valid pixels. */
struct yasl_private_damage_list {
    /** -1: full target; 0: no damaged rectangles; otherwise number of rects. */
    int n;
    /** Reserved hint; callers may leave zero. Core presentation uses rects. */
    struct yasl_private_rect2i bounds;
    /** First n rectangles, in device pixels; ignored for n == -1. */
    struct yasl_private_rect2i rects[YASL_PRIVATE_DAMAGE_MAX];
};

/** Borrowed draw target, valid only for the current draw callback. Never free pixels. */
struct yasl_private_framebuffer {
    /** Opaque draw identity. Preserve it and all target fields when copying. */
    uint64_t generation;
    /** Writable premultiplied ARGB8888, row stride size.x * sizeof(uint32_t); may be NULL for native rendering. */
    uint32_t *pixels;
    /** Target extent in device pixels. */
    struct yasl_private_vector2i size;
    /** Row stride in bytes, populated for the active draw callback. */
    int stride_bytes;
};

/** One draw invocation. Do not retain its pointer after the callback returns. */
struct yasl_private_frame {
    /** Surface being drawn. */
    struct yasl_surface *surface;
    /** Target to initialize and present before returning. */
    struct yasl_private_framebuffer target;
    /** Zero-based index of this surface draw. */
    uint64_t frame_index;
    /** Estimated animation time; not confirmed presentation. */
    uint64_t target_time_ns;
    /** Device scale numerator, with 120 representing 1x. */
    uint32_t scale_120;
};

/** Coalesced redraw policy, valid with either loop style. */
typedef enum yasl_private_pacing_mode {
    YASL_PRIVATE_PACING_ON_DEMAND = 0, /**< Draw only when invalidated or exposed. */
    YASL_PRIVATE_PACING_CONTINUOUS, /**< Draw continuously using native pacing or a timer. */
} yasl_private_pacing_mode;

/* Drag and Drop */
/** Target notifications for YASL surface drags, separate from external file drops. */
enum yasl_private_drag_event {
    YASL_PRIVATE_DRAG_OVER, /**< Surface drag entered or moved over the target. */
    YASL_PRIVATE_DRAG_LEAVE, /**< Surface drag left the target. */
    YASL_PRIVATE_DRAG_DROP, /**< Surface drag dropped on the target. */
    YASL_PRIVATE_DRAG_CANCEL, /**< Surface drag ended on no target; sent to its origin. */
};

/** External text/file drag hover notifications; completed data arrives through input. */
enum yasl_private_drop_event {
    YASL_PRIVATE_DROP_OVER, /**< External data drag entered or moved over the target. */
    YASL_PRIVATE_DROP_LEAVE, /**< External data drag left the target. */
};

/** Source-side drag completion; DROPPED does not prove application-level acceptance. */
enum yasl_private_drag_end {
    YASL_PRIVATE_DRAG_DROPPED, /**< Drag ended in a drop; not an application acceptance receipt. */
    YASL_PRIVATE_DRAG_CANCELLED, /**< Drag canceled, including display close. */
};

/** Copied drag icon in tightly packed premultiplied 0xAARRGGBB device pixels. */
struct yasl_private_drag_image {
    /** Copied pixel array; required when an image is supplied. */
    const uint32_t *pixels;
    /** Positive width and height in device pixels. */
    struct yasl_private_vector2i size;
    /** Pointer position within the image, in device pixels. */
    struct yasl_private_vector2i hotspot;
};

/** Copied drag completion listener; userdata remains owned by the application. */
struct yasl_private_drag_listener {
    /** Called exactly once after a successful start, possibly synchronously on cancel/close. */
    void (*end)(void *ud, enum yasl_private_drag_end how);
};

/** Configured device-pixel content size and the corresponding display scale. */
struct yasl_private_geometry {
    /** Current content extent in device pixels. */
    struct yasl_private_vector2i size;
    /** Positive scale numerator; 120 means 1x and 180 means 1.5x. */
    uint32_t scale_120;
};

/* Input Model */
/** Keyboard press, release, or autorepeat; releases must not insert text. */
enum yasl_private_key_action {
    YASL_PRIVATE_KEY_PRESS, /**< Key pressed. */
    YASL_PRIVATE_KEY_RELEASE, /**< Key released. */
    YASL_PRIVATE_KEY_REPEAT, /**< Key autorepeat. */
};

/** Modifier bitmask. SUPER is Command on macOS. */
enum yasl_private_mod : uint32_t {
    YASL_PRIVATE_MOD_SHIFT = 1 << 0, /**< Shift modifier. */
    YASL_PRIVATE_MOD_ALT   = 1 << 1, /**< Alt/Option modifier. */
    YASL_PRIVATE_MOD_CTRL  = 1 << 2, /**< Control modifier. */
    YASL_PRIVATE_MOD_SUPER = 1 << 3, /**< Super/Command modifier. */
    YASL_PRIVATE_MOD_CAPS  = 1 << 4, /**< Caps Lock modifier. */
};

/** Pointer source class; capability support does not imply an attached device. */
enum yasl_private_pointer_device {
    YASL_PRIVATE_PTR_DEV_MOUSE, /**< Mouse or trackpad pointer. */
    YASL_PRIVATE_PTR_DEV_TOUCH, /**< Touch translated into pointer input. */
    YASL_PRIVATE_PTR_DEV_PEN, /**< Tablet stylus. */
    YASL_PRIVATE_PTR_DEV_ERASER, /**< Tablet eraser tool. */
};

/** Pointer button; meaningful only for POINTER_DOWN and POINTER_UP. */
enum yasl_private_pointer_button {
    YASL_PRIVATE_BUTTON_PRIMARY = 0, /**< Left button, pen tip, or touch contact. */
    YASL_PRIVATE_BUTTON_MIDDLE = 1, /**< Middle button / wheel click. */
    YASL_PRIVATE_BUTTON_SECONDARY = 2, /**< Right button or pen barrel button. */
    YASL_PRIVATE_BUTTON_BACK = 3, /**< Side "back" button. */
    YASL_PRIVATE_BUTTON_FORWARD = 4, /**< Side "forward" button. */
};

/** Bit for a yasl_private_pointer_button in yasl_private_pointer_event.buttons. */
#define YASL_PRIVATE_BUTTON_BIT(button) (1u << (button))

/** Pointer capture mode for a surface. */
enum yasl_private_pointer_mode {
    YASL_PRIVATE_POINTER_NORMAL = 0, /**< Absolute positions; cursor per yasl_private_surface_set_cursor(). */
    /** Cursor hidden and locked in place while the surface has focus. POINTER_MOTION
     * reports unbounded relative motion in delta; pos stays at the lock position.
     * The lock is released on focus loss and re-acquired on focus gain. */
    YASL_PRIVATE_POINTER_RELATIVE,
};

/** Normalized tablet axes; zero for unavailable axes and non-tablet devices. */
struct yasl_private_pointer_pen {
    /** 0..65535 normalized to device range. Zero alone does not end contact. */
    uint16_t pressure;
    /** Degrees in -90..90; positive right. */
    int8_t tilt_x;
    /** Degrees in -90..90; positive down. */
    int8_t tilt_y;
};

/** Event tag. Each tag lists the union member it selects. */
enum yasl_private_event_type {
    YASL_PRIVATE_EVENT_NONE = 0,

    /* Input: produced by backends and consumed from the display FIFO. */
    YASL_PRIVATE_EVENT_KEY, /**< .key */
    YASL_PRIVATE_EVENT_TEXT, /**< .text — committed UTF-8; insert this, not key codepoints. */
    YASL_PRIVATE_EVENT_POINTER_DOWN, /**< .pointer — button/tip pressed; button valid. */
    YASL_PRIVATE_EVENT_POINTER_UP, /**< .pointer — button/tip released; button valid. */
    YASL_PRIVATE_EVENT_POINTER_MOTION, /**< .pointer — position, relative motion, or pen axes changed. */
    YASL_PRIVATE_EVENT_POINTER_SCROLL, /**< .pointer — scroll valid. */
    YASL_PRIVATE_EVENT_POINTER_LEAVE, /**< .pointer — last known pos/device; cancel hover state. */
    YASL_PRIVATE_EVENT_FOCUS, /**< .focus — keyboard focus changed; cancel held state on loss. */
    YASL_PRIVATE_EVENT_PASTE, /**< .paste — clipboard request completion. */
    YASL_PRIVATE_EVENT_DROP, /**< .text — dropped text/uri-list: CRLF-separated file URLs, not paths. */
    YASL_PRIVATE_EVENT_INPUT_RESET, /**< No payload. Input stream interrupted; clear held keys, buttons, strokes, IME. */

    /* Window and application notifications in the display FIFO. */
    YASL_PRIVATE_EVENT_WINDOW_RESIZE, /**< .resize — size and/or scale changed; reallocate and redraw. */
    YASL_PRIVATE_EVENT_WINDOW_CLOSE, /**< No payload. User asked to close the window. */
    YASL_PRIVATE_EVENT_USER, /**< .user — posted application message. */
    YASL_PRIVATE_EVENT_WINDOW_STATE, /**< .state — reported state changed. */
    YASL_PRIVATE_EVENT_DRAG, /**< .drag — surface-drag target notification. */
    YASL_PRIVATE_EVENT_DROP_HOVER, /**< .drop — external drag hover/leave. */
    YASL_PRIVATE_EVENT_COMPOSITION, /**< .composition — preedit snapshot, including ending. */
    YASL_PRIVATE_EVENT_DISPLAY_QUIT, /**< No payload; system requested application quit. */
    YASL_PRIVATE_EVENT_DISPLAY_REOPEN, /**< No payload; system requested activation. */
    YASL_PRIVATE_EVENT_DISPLAY_LOST,
    YASL_PRIVATE_EVENT_PEN,
    YASL_PRIVATE_EVENT_TEXT_EDIT,
    YASL_PRIVATE_EVENT_REFRESH, /**< No payload; surface contents are needed. */
    YASL_PRIVATE_EVENT_DISPLAY_RESTORED, /**< No payload; display reconnected after DISPLAY_LOST. */
};

/** Physical keyboard transition with optional layout labels.
 * key is independent of modifiers/layout and stable between press and release.
 * codepoint and shifted are optional single Unicode scalars for the current
 * layout at levels zero/one, not text input. Zero means unavailable, a control
 * key, or a layout mapping that produces multiple characters. Insert committed
 * YASL_PRIVATE_EVENT_TEXT into documents instead. */
struct yasl_private_key_event {
    /** Portable physical key; YASL_KEY_UNKNOWN when unmapped. */
    enum yasl_key key;
    /** Press, release, or autorepeat. */
    enum yasl_key_action action;
    /** yasl_private_mod bitmask at this event. */
    uint32_t mods;
    /** Optional unshifted layout Unicode scalar; zero if unavailable. */
    uint32_t codepoint;
    /** Optional shifted layout Unicode scalar; zero if unavailable. */
    uint32_t shifted;
    /** Native code: Wayland evdev, X11 server keycode, Cocoa virtual keycode. */
    uint32_t native_code;
    enum yasl_key_text text_input;
    uint32_t consumed_mods;
    uint64_t text_id;
};

/** Bits of yasl_private_pointer_event.scroll_valid. */
enum yasl_private_scroll_valid { YASL_PRIVATE_SCROLL_LINES = 1u, YASL_PRIVATE_SCROLL_PIXELS = 2u };

/** Pointer sample shared by all POINTER_* events. Pen and eraser share it with
 * mouse input. Track contact with DOWN/UP of the primary button, not pressure.
 * This is not a multi-contact touch API. */
struct yasl_private_pointer_event {
    struct yasl_private_vector2d pixel_scroll;
    unsigned scroll_valid, scroll_source, scroll_phase;
    bool scroll_momentum;
    double precise_x, precise_y;
    bool precise_position;
    /** Surface-relative device-pixel position. Frozen while YASL_PRIVATE_POINTER_RELATIVE is locked. */
    struct yasl_private_vector2i pos;
    /** MOTION: device-pixel movement since the previous sample on this surface.
     * Unbounded under YASL_PRIVATE_POINTER_RELATIVE; zero after enter, leave, or reset. */
    struct yasl_private_vector2d delta;
    /** SCROLL: fractional line offsets after system preferences; positive right/down. */
    struct yasl_private_vector2d scroll;
    /** DOWN/UP: which button changed. */
    enum yasl_private_pointer_button button;
    /** YASL_PRIVATE_BUTTON_BIT() mask of buttons held after this event, on every
     * POINTER_* type. Tracked from this queue's DOWN and UP events for all
     * devices; INPUT_RESET clears it. On Wayland LEAVE clears it too, since the
     * compositor sends leave during a press only when it takes the pointer.
     * X11 and Cocoa keep sending motion and the release after LEAVE. */
    uint32_t buttons;
    /** Mouse, touch, pen, or eraser. */
    enum yasl_private_pointer_device device;
    /** yasl_private_mod bitmask at this event. */
    uint32_t mods;
    /** Current tablet axes, including on pressure-only motion; missing axes are zero. */
    struct yasl_private_pointer_pen pen;
};

/** Borrowed NUL-terminated UTF-8, shared by TEXT, PASTE and DROP. */
struct yasl_private_text_event {
    /** Borrowed NUL-terminated bytes; valid until the next successful display poll or display close. */
    const char *data;
    /** Length in bytes excluding the terminating NUL. */
    size_t len;
};

/** Clipboard transfer outcome. */
enum yasl_private_transfer_status {
    YASL_PRIVATE_TRANSFER_OK = 0, /**< Text received successfully, including empty text. */
    YASL_PRIVATE_TRANSFER_UNAVAILABLE, /**< No text selection is available. */
    YASL_PRIVATE_TRANSFER_FAILED, /**< Transfer or decoding failed. */
};

/** Clipboard completion; data/len share the text payload layout. */
struct yasl_private_paste_event {
    const char *data; /**< Borrowed NUL-terminated UTF-8; empty on failure. */
    size_t len; /**< Byte length excluding NUL. */
    uint64_t request_id; /**< Nonzero clipboard request identity. */
    enum yasl_private_transfer_status status; /**< Completion outcome. */
};

/** Borrowed UTF-8 preedit snapshot, valid until the next successful display poll. */
struct yasl_private_composition_event {
    const char *data; /**< Borrowed NUL-terminated UTF-8 snapshot. */
    size_t len; /**< Byte length excluding NUL. */
    int begin; /**< UTF-8 byte offset, or -1 if unavailable. */
    int end; /**< UTF-8 byte offset, or -1 if unavailable. */
    bool active; /**< Whether composition text is present. */
    uint64_t revision, generation;
};

/** Reported window-state change. */
struct yasl_private_state_event { uint32_t flags; /**< Current YASL_PRIVATE_STATE_* flags. */ };
/** Surface-drag target notification; position is in device pixels. */
struct yasl_private_drag_target_event {
    enum yasl_private_drag_event action; /**< Hover, leave, or drop. */
    struct yasl_private_vector2i pos; /**< Surface-relative device pixels. */
};
/** External drag hover notification; position is in device pixels. */
struct yasl_private_drop_hover_event {
    enum yasl_private_drop_event action; /**< Hover or leave. */
    struct yasl_private_vector2i pos; /**< Surface-relative device pixels. */
};

/** Keyboard focus transition. */
struct yasl_private_focus_event {
    /** true on focus gain, false on loss. */
    bool focused;
};

/** New configured geometry for a surface. */
struct yasl_private_resize_event {
    /** Content size in device pixels and display scale. */
    struct yasl_private_geometry geometry;
    /** true when scale_120 differs from the previous resize of this surface. */
    bool scale_changed;
};

/** Application message posted with yasl_private_post_event(). */
struct yasl_private_user_event {
    /** Application-defined message code. */
    uint32_t code;
    /** Pointer passed through unchanged; ownership stays with the application. */
    void *payload;
    /** Optional payload size, passed through unchanged. */
    size_t size;
};

/** One tagged event; read only the union member selected by type. */
enum yasl_private_surface_flags { YASL_PRIVATE_SURFACE_CPU = 1u, YASL_PRIVATE_SURFACE_HIDDEN = 2u, YASL_PRIVATE_SURFACE_UNDECORATED = 4u, YASL_PRIVATE_SURFACE_FIXED_SIZE = 8u };
enum yasl_private_pen_action { YASL_PRIVATE_PEN_ENTER, YASL_PRIVATE_PEN_LEAVE, YASL_PRIVATE_PEN_MOVE, YASL_PRIVATE_PEN_DOWN, YASL_PRIVATE_PEN_UP, YASL_PRIVATE_PEN_BUTTON, YASL_PRIVATE_PEN_CANCEL };
enum yasl_private_pen_axis { YASL_PRIVATE_PEN_PRESSURE = 1u, YASL_PRIVATE_PEN_TILT = 2u, YASL_PRIVATE_PEN_ROTATION = 4u, YASL_PRIVATE_PEN_DISTANCE = 8u, YASL_PRIVATE_PEN_TANGENTIAL = 16u, YASL_PRIVATE_PEN_WHEEL = 32u };
enum yasl_private_presenter { YASL_PRIVATE_PRESENTER_NONE, YASL_PRIVATE_PRESENTER_CPU, YASL_PRIVATE_PRESENTER_GL, YASL_PRIVATE_PRESENTER_VULKAN, YASL_PRIVATE_PRESENTER_METAL };
struct yasl_private_pen_sample {
    uint64_t device_id, tool_id;
    double x, y, pressure, tilt_x, tilt_y, rotation, distance, tangential, wheel_degrees;
    uint32_t axes_valid, axes_changed, buttons, changed_button;
    bool in_contact;
    unsigned tool, action;
};
struct yasl_private_event {
    /** Selects the valid union member. */
    enum yasl_private_event_type type;
    /** Recipient surface, or NULL for display-wide events. Can differ from the
     * polled surface for routed popup input. */
    struct yasl_surface *surface;
    /** Monotonic timestamp in nanoseconds, same clock as yasl_private_loop_now_ns(). */
    uint64_t time_ns;
    /** Increasing display-wide enqueue order; zero for standalone private queues. */
    uint64_t sequence;
    /** Stable recipient identity; zero for display-wide events. */
    uint64_t surface_id;
    /** Physical key that produced TEXT or TEXT_EDIT. Zero means no key. */
    uint64_t text_key_id;
    union {
        struct yasl_private_pen_sample pen;
        struct { const char *data; size_t len; uint64_t revision, generation; size_t begin, end; } text_edit;
        struct yasl_private_key_event key; /**< KEY */
        struct yasl_private_pointer_event pointer; /**< POINTER_* */
        struct yasl_private_text_event text; /**< TEXT, DROP */
        struct yasl_private_paste_event paste; /**< PASTE */
        struct yasl_private_composition_event composition; /**< COMPOSITION */
        struct yasl_private_state_event state; /**< WINDOW_STATE */
        struct yasl_private_drag_target_event drag; /**< DRAG */
        struct yasl_private_drop_hover_event drop; /**< DROP_HOVER */
        struct yasl_private_focus_event focus; /**< FOCUS */
        struct yasl_private_resize_event resize; /**< WINDOW_RESIZE */
        struct yasl_private_user_event user; /**< USER */
    };
};

/* Ownership and threading
 * All calls and callbacks run on the display's opening thread (main thread on
 * Cocoa). Displays borrow supplied loops; destroy surfaces before displays,
 * popups before parents, and displays before their supplied loop.
 * Specs, strings, callback tables and drag listeners are copied during calls.
 * Userdata is borrowed until callbacks are removed or the object/drag ends.
 * Surface destruction is allowed from its callbacks; display/loop destruction
 * during dispatch or callbacks is not supported.
 */

/* Callbacks */
/** Internal CPU presenter callback, copied on installation. */
struct yasl_private_window_cbs {
    void (*draw)(struct yasl_surface *s, struct yasl_private_frame *frame, void *ud);
};

/* Display Lifecycle */
/** Runtime backend choice. AUTO selects a native backend and allows fallback. */
enum yasl_private_backend {
    YASL_PRIVATE_BACKEND_AUTO = 0, /**< Choose native backend with fallback. */
    YASL_PRIVATE_BACKEND_WAYLAND, /**< Require Wayland. */
    YASL_PRIVATE_BACKEND_X11, /**< Require X11. */
    YASL_PRIVATE_BACKEND_COCOA, /**< Require Cocoa. */
};

/** Display options. Zero-initialize for automatic backend selection. */
struct yasl_private_display_spec {
    /** Copied application identifier, conventionally a reverse-domain UTF-8 string; NULL uses backend defaults. */
    const char *app_id;

    /** Zero/AUTO chooses native backend; explicit choices never fall back. */
    enum yasl_private_backend backend;
};

/* Return NULL and set errno if the requested backend cannot open. */
/**
 * Open a desktop connection and its event machinery.
 * @param loop Borrowed loop, or NULL to create a display-owned loop.
 * @param spec Copied options, or NULL for automatic backend selection and defaults.
 * @return An owned display, or NULL with errno set. EINVAL means an invalid backend;
 * ENOSYS means the requested backend was not built; ENODEV is the fallback when
 * opening fails without a more specific platform error. Explicit choices never
 * fall back. Close with yasl_private_display_close() after destroying its surfaces.
 */
struct yasl_private_display *yasl_private_display_open(struct yasl_loop *loop,
                                               const struct yasl_private_display_spec *spec);
/**
 * Read the selected backend.
 * @param d Display, or NULL.
 * @return The actual backend, or YASL_PRIVATE_BACKEND_AUTO for NULL.
 */
enum yasl_private_backend yasl_private_display_backend(const struct yasl_private_display *d);
/**
 * Close a display and release its owned loop, if any.
 * @param d Display to close, or NULL for no action.
 * Destroy all surfaces first. Cancels an active drag and may invoke its end
 * listener. Must not be called during dispatch or from a callback.
 */
void yasl_private_display_close(struct yasl_private_display *d);
/**
 * Query currently available optional features.
 * @param d Display, or NULL.
 * @return A bitmask of yasl_private_display_cap values, or zero for NULL.
 * Capabilities describe backend/server support, not attached input hardware or
 * permission to perform an operation at any arbitrary moment.
 */
uint32_t yasl_private_display_caps(const struct yasl_private_display *d);
/**
 * Draw scheduled surfaces and send pending display requests.
 * @param d Display, or NULL for no action.
 * May synchronously invoke draw and other callbacks. A paced surface may wait
 * for its next display/compositor signal. This is not a GPU completion wait.
 */
void yasl_private_display_flush(struct yasl_private_display *d);
/**
 * Read the latest modifier state known to the backend.
 * @param d Display, or NULL.
 * @return A bitmask of yasl_private_mod values, or zero for NULL.
 * For an individual event use its own mods field instead.
 */
int yasl_private_display_modifiers(const struct yasl_private_display *d);
/** Double-click interval used when the system reports none. */
#define YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT 400
/**
 * Read the system's double-click interval.
 * @param d Display, or NULL.
 * @return Maximum milliseconds between the clicks of a double click, or
 * YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT when d is NULL or the system reports none.
 * X11 reads the XSETTINGS Net/DoubleClickTime value. Wayland, and X11 without
 * XSETTINGS, read the KDE or GNOME setting through the XDG Settings portal,
 * which needs D-Bus and can wait briefly. Cocoa reads the system interval.
 * The user can change it at any time; read it again when a window gains focus
 * rather than on every click.
 */
int yasl_private_display_double_click_ms(const struct yasl_private_display *d);

/* A successful drag invokes the copied listener's non-NULL end once, including
 * cancel and display close. Failed starts do not invoke it. */
/**
 * Start a surface drag from a recent pointer press.
 * @param d Display that owns origin.
 * @param origin Origin surface.
 * @param image Optional copied premultiplied ARGB8888 drag image, or NULL.
 * @param l Optional copied completion listener, or NULL.
 * @param ud Borrowed listener userdata until completion.
 * @return true if started; false if unsupported, invalid, or not currently permitted.
 * A successful start invokes a non-NULL end callback exactly once, including
 * cancellation or display close. Failed starts do not invoke it. This API carries
 * surfaces, not arbitrary MIME payloads; use application state to identify the item.
 */
bool yasl_private_display_drag_start(struct yasl_private_display *d, struct yasl_surface *origin,
                                     const struct yasl_private_drag_image *image,
                                     const struct yasl_private_drag_listener *l, void *ud);
/**
 * Cancel the active surface drag, if any.
 * @param d Display, or NULL for no action.
 * May invoke the end listener synchronously.
 */
void yasl_private_display_drag_cancel(struct yasl_private_display *d);

/* Surface Lifecycle */
/** Toplevel options. Zero-initialize, then supply a positive logical-pixel size. */
struct yasl_private_window_spec {
    unsigned flags;
    bool external_gpu;
    /** Copied UTF-8 title; NULL uses backend defaults. */
    const char *title;
    /** Requested logical-pixel width and height; both must be positive. */
    struct yasl_private_vector2i size;
    /** Request initial fullscreen; actual state is reported asynchronously. */
    bool fullscreen;
    /** Permit unpaced presentation where supported; a hint, not a guarantee. */
    bool tearing;
    /** Copied optional window callbacks; NULL disables them. */
    const struct yasl_private_window_cbs *cbs;
    /** Borrowed CPU presenter userdata. */
    void *ud;
    /** Borrowed application data, independent of the CPU presenter. */
    void *userdata;
};

/** Popup options, entirely in device pixels, including size.
The content rectangle excludes transparent margins/shadows used for placement.
An axis of content_size equal to zero extends to that axis of size. The offset
and resolved content size must fit in size. Anchor extents must be nonnegative.
Dimensions must be positive, at most 32767, and fit an INT32_MAX-byte buffer;
backends may impose further limits after scaling or resolving placement. */
struct yasl_private_popup_spec {
    unsigned anchor_edges, gravity, constraints;
    /** Full popup framebuffer extent in device pixels, including transparent margins. */
    struct yasl_private_vector2i size;
    /** Rectangle in parent framebuffer coordinates, in device pixels. */
    struct yasl_private_rect2i anchor;
    /** Top-left content offset within the popup, in device pixels. */
    struct yasl_private_vector2i content_offset;
    /** Content extent; zero per axis extends to the corresponding size edge. */
    struct yasl_private_vector2i content_size;
};

/**
 * Create a resizable desktop window with CPU drawing targets.
 * @param d Owning display.
 * @param spec Required copied options; size is in logical pixels.
 * @return An owned surface, or NULL with errno set. EINVAL means invalid options,
 * including a nonpositive dimension, a dimension above 32767, or a buffer above
 * INT32_MAX bytes. ENODEV is the fallback backend failure. Actual configured
 * size may differ from the request. The callback table in spec->cbs is copied;
 * spec->ud stays borrowed until callbacks are replaced or the surface is destroyed.
 * This window's callbacks cannot run before creation returns. Use
 * yasl_private_surface_geometry() for initial geometry. Schedule an initial redraw
 * after setting up the CPU presenter. Lifecycle notifications arrive in the display FIFO.
 */
struct yasl_surface *yasl_private_surface_create_toplevel(struct yasl_private_display *d,
                                                          const struct yasl_private_window_spec *spec);
/**
 * Create an owned popup attached to a parent surface.
 * @param parent Borrowed parent; destroy the popup before the parent.
 * @param spec Required copied options, entirely in device pixels.
 * @return A popup, or NULL with errno set: EINVAL for invalid options, ENOTSUP
 * for missing popup support, or a backend error (ENODEV if none was supplied).
 * Wayland requires a recent input serial for its popup grab. The backend may
 * flip or slide placement to fit the screen. See yasl_private_popup_spec.
 */
struct yasl_surface *yasl_private_surface_create_popup(struct yasl_surface *parent,
                                                       const struct yasl_private_popup_spec *spec);
/**
 * Update a popup's geometry and placement.
 * @param popup An existing popup.
 * @param spec Required copied replacement options, in device pixels.
 * @return true on success; false with errno set: EINVAL for invalid options,
 * ENOTSUP for missing popup support, or a backend error (ENODEV if unspecified).
 * Older Wayland popup protocol versions can reject repositioning.
 */
bool yasl_private_surface_popup_reposition(struct yasl_surface *popup,
                                           const struct yasl_private_popup_spec *spec);
/**
 * Destroy a surface and cancel its surface timers and pending input.
 * @param s Surface, or NULL for no action.
 * Destroy child popups first. May be called from a surface callback; return from
 * that callback without further use of the surface or its draw target. Every
 * pointer or native handle obtained from this surface becomes invalid.
 */
void yasl_private_surface_destroy(struct yasl_surface *s);
/**
 * Get the display that owns a surface.
 * @param s Surface, or NULL.
 * @return A borrowed display, or NULL.
 */
struct yasl_private_display *yasl_private_surface_display(const struct yasl_surface *s);

/**
 * Replace the surface's callback table and userdata.
 * @param s Surface.
 * @param cbs Table copied by value; NULL removes all callbacks. NULL members are ignored.
 * @param ud Borrowed userdata passed to the installed callbacks.
 * @return true if replaced; false with EINVAL for NULL.
 * This call only replaces callbacks; it does not invoke them.
 * Use yasl_private_surface_geometry() to read current geometry; do not rely on receiving
 * an initial configure callback when registering.
 */
bool yasl_private_surface_set_callbacks(struct yasl_surface *s,
                                        const struct yasl_private_window_cbs *cbs, void *ud);
/**
 * Replace the window title with copied UTF-8 text.
 * @param s Surface.
 * @param title NUL-terminated UTF-8 title. Pass an empty string to clear it.
 * Backends may truncate the title.
 */
void yasl_private_surface_set_title(struct yasl_surface *s, const char *title);
/**
 * Request a future draw callback, coalescing repeated requests.
 * @param s Surface, or NULL for no action.
 * Call yasl_private_display_flush() to service the request. Schedule again from draw to
 * animate. Without YASL_PRIVATE_CAP_FRAME_PACING, throttle animation with a timer.
 */
void yasl_private_surface_schedule_redraw(struct yasl_surface *s);
/* Present exactly the target supplied to the current draw callback, at most
 * once. Targets expire when that callback returns or the surface is destroyed.
 * damage is required: n must be -1 or 0..YASL_PRIVATE_DAMAGE_MAX. Rectangles must have
 * nonnegative extents; they are clipped to the target. Invalid input returns
 * false without consuming the frame. See yasl/native.h for native rendering. */
/**
 * Submit the current draw target at most once.
 * @param s Surface currently being drawn.
 * @param fb Exact target from the current draw callback, including its generation.
 * @param damage Required damage hint: n is -1 for all pixels or 0..YASL_PRIVATE_DAMAGE_MAX.
 * @return true if accepted; false for invalid, stale, already presented, or failed targets.
 * Rectangles must have nonnegative extents and are clipped to the target. Invalid
 * input does not consume the frame. All pixels must be initialized even with
 * partial damage. The backend may upload all pixels. Success records submission,
 * not confirmed scanout. The target expires when draw returns or s is destroyed.
 */
bool yasl_private_surface_present(struct yasl_surface *s, const struct yasl_private_framebuffer *fb,
                                  const struct yasl_private_damage_list *damage);
/**
 * Decode the next local file path from a YASL_PRIVATE_EVENT_DROP payload.
 * @param cursor Required in/out position in the NUL-terminated list. Start it
 * at event.text.data; a NULL *cursor returns zero.
 * @param out Output for the NUL-terminated path, or NULL when cap is zero.
 * @param cap Size of out in bytes.
 * @return Path length in bytes, excluding the NUL, or zero when no paths remain.
 * When the result is less than cap, out holds the path and *cursor moves past
 * it. Otherwise out and *cursor are unchanged; call again with a buffer of at
 * least the result plus one.
 * Accepts file:///path, file:/path, and file://host/path when host is
 * localhost or this machine's name. Percent escapes decode to raw bytes, and
 * a '?' or '#' ends the path. Skips blank lines, comments, other schemes,
 * other hosts, and lines with a malformed escape or an escaped NUL. May be
 * called from any thread.
 */
size_t yasl_private_drop_next_path(const char **cursor, char *out, size_t cap);
/**
 * Copy the last frame presented from the surface's CPU target.
 * @param s Surface.
 * @param out Output for size->x * size->y premultiplied ARGB8888 pixels with
 * tightly packed rows, or NULL when cap is zero.
 * @param cap Capacity of out in pixels.
 * @param size Required output: the frame extent in device pixels, or zero when
 * no frame is available.
 * @return true if copied. false leaves out unchanged; when *size is nonzero,
 * out was too small, so allocate size->x * size->y pixels and call again.
 * A frame is available from a successful yasl_private_surface_present() until the
 * next draw callback starts, and again once that draw presents. After a draw
 * that does not present or a resize that replaces the target, no frame may be
 * available; native rendering never has one. Call outside draw callbacks. This copies YASL's frame, not
 * the composited screen: decorations, other windows, and color conversion are
 * not included.
 */
bool yasl_private_surface_read_pixels(struct yasl_surface *s, uint32_t *out, size_t cap,
                                      struct yasl_private_vector2i *size);
/**
 * Select absolute or relative (locked) pointer input for a surface.
 * @param s Surface.
 * @param mode YASL_PRIVATE_POINTER_NORMAL or YASL_PRIVATE_POINTER_RELATIVE.
 * @return true if the mode is supported; false leaves the current mode.
 * RELATIVE hides the cursor and locks it while the surface is focused;
 * POINTER_MOTION then carries unbounded delta. Returning to NORMAL restores
 * the cursor selected with yasl_private_surface_set_cursor(). RELATIVE is supported on
 * Wayland compositors with the relative-pointer and pointer-constraints
 * protocols; X11 and Cocoa return false.
 */
bool yasl_private_surface_set_pointer_mode(struct yasl_surface *s, enum yasl_private_pointer_mode mode);

/**
 * Consume one event from the display-wide FIFO without waiting.
 * Returns false for an empty queue or invalid arguments, leaving ev unchanged.
 * Strings remain valid until the next successful poll or display close.
 * Surface destruction cancels undelivered events, not the last polled payload.
 * Use the display thread. Do not mix display and surface polling in one app.
 */
bool yasl_private_display_poll_event(struct yasl_private_display *d, struct yasl_private_event *ev);

/** Stable nonzero identity within a display; zero for NULL. Use on the display thread. */
uint64_t yasl_private_surface_id(const struct yasl_surface *s);
/** Store borrowed application data independently of callback userdata. */
void yasl_private_surface_set_userdata(struct yasl_surface *s, void *userdata);
/** Read borrowed application data, or NULL for NULL; use on the display thread. */
void *yasl_private_surface_userdata(const struct yasl_surface *s);

/** Thread-safe message descriptor. Zero target_id means display-wide. */
struct yasl_private_message {
    uint64_t target_id; /**< Surface identity, or zero for the display. */
    uint32_t code; /**< Application-defined message code. */
    void *payload; /**< Borrowed until release; passed through unchanged. */
    size_t size; /**< Application-defined payload length. */
    /** Optional cleanup on display thread after delivery/cancellation, exactly once.
     * May post messages, but must not dispatch, poll, or destroy YASL objects. */
    void (*release)(void *payload);
};
/**
 * Copy and enqueue a message from any thread; the display must stay alive.
 * Stop producer threads before closing the display. On success release runs
 * once after consumption or cancellation. On failure ownership stays with the
 * caller; errno is EINVAL, ENOENT (retired identity), ENOMEM, or EOVERFLOW.
 */
bool yasl_private_display_post(struct yasl_private_display *d, const struct yasl_private_message *message);

/**
 * Present the active frame once. NULL/0 means full damage; non-NULL/0 means none.
 * Validate all rectangles; excess rectangles fall back to full damage.
 * Returns false with EINVAL for invalid/stale targets, EALREADY for a consumed
 * frame, or errno from the backend (EIO if unspecified).
 * Frame and pixels expire at callback return; use the display thread.
 */
bool yasl_private_frame_present(struct yasl_private_frame *frame,
                                const struct yasl_private_rect2i *rects, size_t count);
/** Coalesce a redraw request; use on the display thread. NULL is ignored. */
void yasl_private_surface_request_frame(struct yasl_surface *s);
/** Owe s a REFRESH after the next frame shown. The next loop step decides
 * which frame that is, so a present after the request counts. */
void yasl_private_surface_request_shown_frame(struct yasl_surface *s);
/** Set drawing policy for either loop style. Returns false with errno on failure. */
bool yasl_private_surface_set_pacing(struct yasl_surface *s, yasl_private_pacing_mode mode);

/** Request a positive logical-pixel size. Success means issued, not configured. */
bool yasl_private_surface_set_size(struct yasl_surface *s, struct yasl_private_vector2i size);
/** Request fullscreen state; returns acceptance, not confirmation. */
bool yasl_private_surface_set_fullscreen(struct yasl_surface *s, bool enabled);
/** Request maximized state; returns acceptance, not confirmation. */
bool yasl_private_surface_set_maximized(struct yasl_surface *s, bool enabled);
/** Request minimization. Window requests set errno on failure; display thread only. */
bool yasl_private_surface_minimize(struct yasl_surface *s);

/** Offer copied UTF-8 text. Returns false with errno if invalid/unsupported/failed. */
bool yasl_private_clipboard_set(struct yasl_private_display *d, enum yasl_private_selection selection,
                                const char *text);
/**
 * Request text; returns a nonzero ID or zero with errno. One PASTE completion
 * follows each accepted request unless its surface/display is destroyed.
 * Inspect paste.status even when data is empty. Use the display thread.
 */
uint64_t yasl_private_clipboard_request(struct yasl_surface *s, enum yasl_private_selection selection);

/* Current device-pixel size and scale, matching the configure callback. */
/**
 * Read current device-pixel size and scale together.
 * @param s Surface, or NULL.
 * @return Geometry matching configure callbacks; NULL gives zero size and unit scale.
 */
struct yasl_private_geometry yasl_private_surface_geometry(const struct yasl_surface *s);
/**
 * Read current window state.
 * @param s Surface, or NULL.
 * @return A bitmask of yasl_private_surface_state values, or zero for NULL.
 * Flags are reported state, not requests; unsupported states are absent.
 * Flags may coexist. Windowed and floating have no separate flags. Suspension
 * does not imply minimization, and no minimized flag is reported.
 */
uint32_t yasl_private_surface_state(const struct yasl_surface *s);
/* Timing uses monotonic nanoseconds. Check source before treating timestamps
 * as presentation confirmation. refresh/next_present are estimates. Without
 * YASL_PRIVATE_CAP_FRAME_PACING, redraws run on flush without display synchronization.
 * Tearing may bypass pacing even when the capability is present. */
/**
 * Read frame timing estimates and their provenance.
 * @param s Surface, or NULL.
 * @return Current timing, or a zero-initialized result for NULL.
 * Timestamps use monotonic nanoseconds. Inspect source before treating a timestamp
 * as confirmation of presentation. Refresh and next-present values are estimates.
 */
struct yasl_private_timing yasl_private_surface_timing(const struct yasl_surface *s);
/**
 * Get a nondecreasing animation timestamp aligned to estimated refresh.
 * @param s Surface, or NULL.
 * @param now Current monotonic nanoseconds, usually from yasl_private_loop_now_ns().
 * @return Animation timestamp, or zero for NULL. Before timing is known, returns now.
 * Use this to advance animations; it is not presentation feedback.
 */
uint64_t yasl_private_surface_frame_time_ns(struct yasl_surface *s, uint64_t now);
/* Set the mouse and tablet cursor for this surface, including future entries.
 * YASL_PRIVATE_CURSOR_HIDDEN stays hidden until another cursor is selected. */
/**
 * Select the surface's system mouse and tablet cursor.
 * @param s Surface.
 * @param cursor Cursor shape; YASL_PRIVATE_CURSOR_HIDDEN hides the cursor without disabling input.
 * The choice persists across motion and re-entry until another shape is selected.
 */
void yasl_private_surface_set_cursor(struct yasl_surface *s, enum yasl_private_cursor cursor);
/**
 * Set a backend window background hint.
 * @param s Surface.
 * @param bg Packed 0xAARRGGBB color; backends may ignore alpha or the entire hint.
 * Does not initialize framebuffer pixels or replace drawing.
 */
void yasl_private_surface_set_background_color(struct yasl_surface *s, uint32_t bg);

/* IME Support: preedit is borrowed until the next dispatch or destruction. */
/**
 * Read the current uncommitted UTF-8 composition text.
 * @param s Surface, or NULL.
 * @return Borrowed NUL-terminated text, or NULL if absent/unsupported.
 * Copy to retain across the next dispatch or surface destruction. Composition
 * changes can schedule a redraw. Insert committed YASL_PRIVATE_EVENT_TEXT into documents,
 * not preedit text.
 */
const char *yasl_private_surface_ime_preedit(const struct yasl_surface *s);
/**
 * Read the cursor/selection range in the preedit string.
 * @param s Surface.
 * @param begin Required output UTF-8 byte offset for the range start.
 * @param end Required output UTF-8 byte offset for the range end.
 * @return true if available; false without changing outputs otherwise.
 * Currently available on Wayland; Cocoa preedit text has no range result.
 */
bool yasl_private_surface_ime_preedit_cursor(const struct yasl_surface *s, int *begin, int *end);
/**
 * Place the composition candidate window near the application caret.
 * @param s Surface.
 * @param caret Caret rectangle relative to the surface, in device pixels.
 * Update after caret motion or scale changes. Unsupported backends ignore it.
 */
void yasl_private_surface_set_ime_caret(struct yasl_surface *s, struct yasl_private_rect2i caret);

/* Client-Side Decorations (CSD) Actions */
/**
 * Ask the window system to begin an interactive move.
 * @param s Toplevel surface, normally during handling of a pointer press.
 * @return true if the request was issued; false if unsupported or not permitted.
 * A recent input serial may be required. Success does not confirm a final position.
 */
bool yasl_private_surface_start_interactive_move(struct yasl_surface *s);
/**
 * Ask the window system to begin an interactive resize.
 * @param s Toplevel surface, normally during handling of a pointer press.
 * @param edge One nonzero yasl_private_resize_edge enumerator.
 * @return true if issued; false for an invalid edge, missing support, or rejected request.
 */
bool yasl_private_surface_start_interactive_resize(struct yasl_surface *s, enum yasl_private_resize_edge edge);
/**
 * Ask the window system to display its window management menu.
 * @param s Toplevel surface.
 * @param pos Surface-relative position in device pixels.
 * @return true if issued; false if unsupported or not currently permitted.
 */
bool yasl_private_surface_show_window_menu(struct yasl_surface *s, struct yasl_private_vector2i pos);

/* Drag Support */
/**
 * Attach a toplevel surface to the current surface drag.
 * @param s Surface to carry; check YASL_PRIVATE_CAP_DRAG_CARRY first.
 * @param offset Device-pixel offset of the pointer within the carried surface.
 * @return true if accepted; false if unsupported or no compatible drag is active.
 */
bool yasl_private_surface_drag_carry(struct yasl_surface *s, struct yasl_private_vector2i offset);
/**
 * Place a toplevel surface relative to the current pointer.
 * @param s Surface; check YASL_PRIVATE_CAP_PLACE_WINDOW first.
 * @param at Device-pixel position within the surface to place under the pointer.
 * @return true if issued; false if unsupported or unavailable.
 */
bool yasl_private_surface_place_under_pointer(struct yasl_surface *s, struct yasl_private_vector2i at);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_H */
