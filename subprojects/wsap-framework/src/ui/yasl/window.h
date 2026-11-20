#ifndef WINDOW_WINDOW_H
#define WINDOW_WINDOW_H

#include <stdbool.h>

#include "input/input.h"
#include "input/keybind.h"
#include "yasl/yasl.h"
#include "paint/framebuffer.h"
#include "util/damage.h"
#include "util/geom.h"

struct frame_stats;
struct json_writer;
struct window;
struct window_host;
struct window_native;

/*
 * Composes one frame of native surface `n`. The caller fills `target`. Set
 * `target.fresh` only when the pixels are not what the last frame drew: the
 * first frame or a reallocation. A retained canvas keeps its pixels, so the
 * renderer repaints only what changed. A failed present makes the next frame
 * repaint every pixel.
 */
void window_native_compose(struct window *w, struct window_native *n,
                           struct framebuffer target);

struct window_config {
    char *font_family;
    struct vector2i size;
    int font_size;
    int menu_hover_delay_ms;
    bool fullscreen;
    bool tearing;
};

struct window_host_hooks {
    void (*empty)(void *ud, struct window *last);
    /* The close request of one window. Return false to keep it open. The
     * host asks again on the next request. Without this hook the window
     * closes. */
    bool (*close)(void *ud, struct window *w);
    /* A quit request. Return false to cancel the quit. The host asks again
     * on the next request. Without this hook the quit runs. */
    bool (*quit_request)(void *ud);
    /* The quit action. It runs after a quit request is allowed. */
    void (*quit)(void *ud);
    void (*reopen)(void *ud);
    bool (*command)(void *ud, struct window *w, input_command_id action);
    /*
     * The service step of a host pass. The owner applies its deferred model
     * changes here, such as the destruction of closed content. No frame, no
     * input event, and no content callback is on the stack. `now_ms` is the
     * frame time of the pass. The frames that follow in the same pass use
     * the same time.
     *
     * A modal step runs while the platform blocks the loop, as during a
     * macOS live resize. The platform then refuses to create or destroy a
     * surface. A modal step must not open or close a window.
     */
    void (*service)(void *ud, uint64_t now_ms, bool modal);
    /* Called when a window wake deadline fires. Returning true skips a full
     * window redraw (e.g. when only an overlay cursor toggled visibility). */
    bool (*wake)(void *ud, struct window *w, uint64_t now_ms);
    void *ud;
};

struct window_host {
    struct yasl_loop *loop;
    const struct window_config *config;
    struct window_host_hooks hooks;
    uint64_t service;
    uint64_t wake;
    uint64_t wake_at_ms;
    /* The frame time of the running host pass, or 0 outside a pass. */
    uint64_t frame_ms;
    /* The inspector server runs and captures each window scene. */
    bool debug_capture;

    struct window **windows;
    int count;
    int cap;
    struct window *primary;
    struct window *closing;
    int holds;
    bool destroying;
    /* The keymap that each window copies. The host loads input.conf into it
     * once, before its first window and at each reload. */
    struct input_keymap keymap;
    bool keymap_loaded;
};

/* Prepares `host` for its first window. The host keeps the `config`
 * pointer, so `config` must outlive the host. It copies `hooks`. Call it
 * once, before window_create(). */
void window_host_init(struct window_host *host, struct yasl_loop *loop,
                      const struct window_config *config,
                      const struct window_host_hooks *hooks);
/* Asks the close hook whether `w` may close. Returns true when the host
 * allows the close. */
bool window_host_request_close(struct window *w);
/* Asks the quit request hook whether the app may quit. Returns true when the
 * host allows the quit. */
bool window_host_request_quit(struct window_host *host);
void window_host_destroy(struct window_host *host);
void window_host_reap(struct window_host *host);
void window_host_schedule(struct window_host *host);
bool window_host_command(struct window *w, input_command_id action);
void window_host_hold(struct window_host *host);
void window_host_release(struct window_host *host);
bool window_host_destroying(const struct window_host *host);
/* Reads input.conf once, and gives the same app bindings to every window of
 * `host` and to each window that opens later. The widget scope bindings stay.
 * A key that is down keeps the route of its press until its release. The
 * new bindings apply from the next press. The app calls this when it
 * reloads its configuration. */
void window_host_reload_input(struct window_host *host);

struct window_frame {
    struct yasl_surface *surface;
    uint64_t generation;
    struct framebuffer target;
};

struct window_geometry {
    struct vector2i size;
    uint32_t scale_120;
};

/* The system title bar over the top of a window that has a title bar
 * overlay, in device pixels from the top left of the window. All zero
 * without the overlay, in fullscreen, and on a platform without it. */
struct window_titlebar {
    /* The height of the band that the system title bar covers. */
    int height;
    /* The area of the system window buttons in the band. */
    struct rect2i controls;
};

typedef void (*window_frame_composer)(struct window *w,
                                      struct window_frame *frame,
                                      void *ud);
void window_set_background(struct window *w, uint32_t bg);
uint32_t window_background(const struct window *w);

struct window_spec {
    const char *title;
    void *graphics_device;
    void *(*acquire_device)(void *ud);
    void *device_ud;
    window_frame_composer compose;
    void *compose_ud;
    struct vector2i size;
    bool windowed;
    /* Asks the platform to extend the content under a transparent system
     * title bar. The window then draws its own title bar: see
     * window_titlebar(). The size includes the title bar. */
    bool titlebar_overlay;
};

struct window *window_open(struct window_host *host, const struct window_spec *spec);
struct window *window_create(struct window_host *host, const struct window_spec *spec);
bool window_has_gpu(const struct window *w);
void window_compose_frame(struct window *w, struct window_frame *frame);
/*
 * Presents the canvas of `frame`, or records the result of a drawable frame.
 * `damage` lists the canvas pixels that changed. NULL, full damage, or an
 * empty list presents the whole canvas. An overflowed list presents its
 * bounds. `presented` reports the GPU result of a drawable frame: true when
 * the GPU submitted the frame, not when a display showed it. Returns true
 * when the surface took the frame. Only a display report proves that a frame
 * was shown.
 */
bool window_present(struct window *w, struct window_frame *frame,
                    const struct damage_list *damage, bool presented);
bool window_native_add(struct window *w, struct yasl_surface *s);
/* Marks one native surface as due in the next host pass. */
void window_native_request_draw(struct window *w, struct yasl_surface *s);
void window_native_remove(struct window *w, struct yasl_surface *s);
void window_native_draw(struct window *w, struct yasl_surface *s);
void window_close(struct window *w);

struct window_drag_target {
    void (*drag)(struct window *w, enum yasl_drop_action ev, struct vector2i pos,
                 void *ud);
    void (*drop_hover)(struct window *w, bool over, void *ud);
};
void window_set_drag_target(struct window *w, const struct window_drag_target *t,
                            void *ud);

/* The frame time. Inside a host pass, this is the time of the pass. Outside
 * a pass, it is the current time. */
uint64_t window_frame_time_ms(struct window *w);
uint32_t window_scale_120(const struct window *w);
/* Returns the scale factor of `w`, where a scale of 120 is 1.0. */
double window_scale(const struct window *w);
#define WINDOW_DEFAULT_COLS 132
#define WINDOW_DEFAULT_ROWS 42
struct vector2i window_default_size(const struct window_config *config);
void window_redraw(struct window *w);
void window_set_title(struct window *w, const char *title);
const char *window_title(const struct window *w);
/* Sends the queued input events to their surfaces. The host calls this once
 * for each window in each pass. */
void window_drain_input(struct window *w);
bool window_inject_input(struct window *w, const struct input_event *ev);

struct window_pointer {
    struct vector2i pos;
    bool inside;
};

struct window_host *window_host(const struct window *w);
struct yasl_surface *window_surface(const struct window *w);
uint32_t window_double_click_ms(const struct window *w);
struct frame_stats *window_stats(struct window *w);
bool window_closed(const struct window *w);
struct vector2i window_size(const struct window *w);
bool window_suspended(const struct window *w);
uint32_t window_frame_count(const struct window *w);
struct window_pointer window_pointer(const struct window *w);
void window_repaint_all(struct window *w);
bool window_take_repaint(struct window *w);
const struct input_keyboard_state *window_keyboard(const struct window *w);
/* Returns the keymap of window `w`. The window copies every scope of its host
 * keymap at creation, and window_host_reload_input() replaces its app scope. */
const struct input_keymap *window_keymap(const struct window *w);
void window_copy(struct window *w, enum yasl_selection sel, const char *text);
void window_paste(struct window *w, enum yasl_selection sel);
int window_modifiers(const struct window *w);
struct window_logical_point {
    double x, y;
};
struct window_logical_point window_logical(const struct window *w,
                                           struct vector2i device);
bool window_place_under_pointer(struct window *w, struct window_logical_point at);

/* Returns the system title bar of `w`. Read it during layout: the window
 * lays out its UI again and calls the resize hook of the mounted UI when the
 * title bar changes. */
struct window_titlebar window_titlebar(const struct window *w);
/*
 * Sets the parts of `w` that act as its title bar, in device pixels from the
 * top left of the window. A primary press there moves the window, and a
 * double click zooms or minimizes it as the system setting asks. These
 * presses reach the UI as no event. The window copies `areas`. A list equal
 * to the current one changes nothing, so the UI can call this after each
 * layout. Returns false on a memory shortage, or when the platform has no
 * title bar areas; the window then keeps the list for
 * window_titlebar_areas().
 */
bool window_set_titlebar_areas(struct window *w, const struct rect2i *areas, int count);
/* Returns the title bar areas of `w` and stores their count in `count`. Call
 * it to check the areas, for example in a test. The array stays valid until
 * the next window_set_titlebar_areas() call. */
const struct rect2i *window_titlebar_areas(const struct window *w, int *count);
bool window_drag_carry(struct window *w, struct window_logical_point at);

const char *window_ime_preedit(const struct window *w);
int window_ime_preedit_begin(const struct window *w);
int window_ime_preedit_end(const struct window *w);

/*
 * The cursor as a native layer above the window content. The window shows
 * and hides it at each blink, and draws no frame for that. Only some
 * platforms have it: see window_cursor_overlay_supported().
 *
 * - `rect` is in canvas pixels. `pixels` holds rect.width * rect.height
 *   0xAARRGGBB values, or it is NULL for a rect of `color`.
 * - `key` names `pixels`. The window keeps the last pixels when the key is
 *   the same. Key 0 never matches: the window takes the pixels again.
 * - window_set_cursor_overlay() returns true when the layer takes the
 *   cursor, also while the cursor is hidden. The frame must then draw no
 *   cursor of its own.
 */
struct window_cursor_overlay {
    bool active;
    bool visible;
    struct rect2i rect;
    uint32_t color;
    const uint32_t *pixels;
    uint64_t key;
};
bool window_cursor_overlay_supported(const struct window *w);
bool window_set_cursor_overlay(struct window *w, const struct window_cursor_overlay *ov);
bool window_cursor_overlay_visible(struct window *w, bool visible);
bool window_cursor_overlay_active(const struct window *w);
bool window_has_popup(const struct window *w);
void window_wake_at(struct window *w, uint64_t at_ms);

#endif
