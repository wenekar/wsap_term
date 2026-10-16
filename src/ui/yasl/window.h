#ifndef WINDOW_WINDOW_H
#define WINDOW_WINDOW_H

#include <stdbool.h>

#include "input/input.h"
#include "yasl/yasl.h"
#include "paint/framebuffer.h"
#include "util/damage.h"
#include "util/geom.h"

struct frame_stats;
struct json_writer;
struct window;
struct window_host;

/* What every window reads from the app's config. The app owns it and
 * changes it in place. Windows receive a pointer to it. */
struct window_config {
    struct vector2i size;
    bool fullscreen;
    bool tearing;
    /* Owned by the app. */
    char *font_family;
    int font_size;
    int menu_hover_delay_ms;
};

/* What the app gives its windows. Every callback is optional. */
struct window_host_hooks {
    /* The host has no windows and no holds. The last window is closing,
     * or the last hold is released. A closing window still has its
     * surface and its stats; the host already counts it as gone. NULL
     * for a release. */
    void (*empty)(void *ud, struct window *last);
    /* The user asked the app to quit. */
    void (*quit)(void *ud);
    /* The user activated the app while it has no windows. */
    void (*reopen)(void *ud);
    /* A shortcut the app may run, such as opening the settings. True when
     * the app ran it. */
    bool (*command)(void *ud, struct window *w, enum input_shortcut action);
    void *ud;
};

/* The windows of one process and what they share: the loop and config.
 * Windows reach nothing else. */
struct window_host {
    struct yasl_loop *loop;
    const struct window_config *config;
    struct window_host_hooks hooks;
    /* The armed service timer, or 0. See window_host_schedule(). */
    uint64_t service;
    /* The armed wake timer, or 0, and the time it fires. See window_wake_at(). */
    uint64_t wake;
    uint64_t wake_at_ms;

    struct window **windows;
    int count;
    int cap;
    struct window *primary;
    /* Closed windows. They stay valid until the next loop iteration. */
    struct window *closing;
    /* Holds keep the host from reporting empty. See window_host_hold(). */
    int holds;
    /* Set when window_host_destroy starts. Holders then end their work. */
    bool destroying;
};

/* Borrows every argument; each must outlive the host. */
void window_host_init(struct window_host *host, struct yasl_loop *loop,
                      const struct window_config *config,
                      const struct window_host_hooks *hooks);
/* Closes every window. Holders see window_host_destroying() first, and
 * a drag in flight ends before the display closes. */
void window_host_destroy(struct window_host *host);
/* Frees the closed windows. The host service does it, so a window closed
 * inside any callback stays valid until that callback and every caller
 * above it return. */
void window_host_reap(struct window_host *host);
/* Arms the host service on the loop: it reaps closed windows, drains input
 * and draws the windows that asked for a frame. Everything that makes such
 * work calls this, so the loop never waits while work is pending. */
void window_host_schedule(struct window_host *host);
/* Asks the app to run a shortcut for w. False when the app does not run it. */
bool window_host_command(struct window *w, enum input_shortcut action);
/* A hold keeps the host from reporting empty while content needs it, such
 * as a tab in flight. Each hold has one release. The host reports empty
 * when it has no windows and no holds. */
void window_host_hold(struct window_host *host);
void window_host_release(struct window_host *host);
/* True once window_host_destroy has started. Holders then free their
 * state without opening windows. */
bool window_host_destroying(const struct window_host *host);

/* One frame of a window. target is what the composer draws into; surface is
 * the native surface it presents on, which is a popup for a menu level.
 * generation is the draw identity yasl assigned, so the frame can be
 * presented. */
struct window_frame {
    struct yasl_surface *surface;
    uint64_t generation;
    struct framebuffer target;
};

/* A window's size and scale, in device pixels and 120ths of a scale. */
struct window_geometry {
    struct vector2i size;
    uint32_t scale_120;
};

typedef void (*window_frame_composer)(struct window *w,
                                      struct window_frame *frame,
                                      void *ud);
void window_set_background(struct window *w, uint32_t bg);

struct window_spec {
    const char *title;
    void *graphics_device;
    window_frame_composer compose;
    void *compose_ud;
    /* Zero on an axis takes the settings size. */
    struct vector2i size;
    /* Open this window normally, even when terminal windows use fullscreen. */
    bool windowed;
};

/* Opens a window with its surface and input, registered with the host's
 * loop. On failure the caller keeps its content. */
struct window *window_open(struct window_host *host, const struct window_spec *spec);
/* The same without a native surface, for tests. */
struct window *window_create(struct window_host *host, const struct window_spec *spec);
/* Calls the app's frame composer for a window or one of its popups. */
void window_compose_frame(struct window *w, struct window_frame *frame);
/* Presents the active frame. False when the frame is not active. */
bool window_present(struct window *w, struct window_frame *frame,
                    const struct damage_list *damage);
/* Native surfaces of a window: its main surface and menu popups. Add one
 * before its first draw and remove it before the surface is destroyed. */
bool window_native_add(struct window *w, struct yasl_surface *s);
void window_native_remove(struct window *w, struct yasl_surface *s);
/* Draws a native frame of w: the main surface or a menu popup. */
void window_native_draw(struct window *w, struct yasl_surface *s);
/* Marks the window closed; the host frees it later. */
void window_close(struct window *w);

/* Where a window takes drags. drag gets each yasl drag event over w with
 * its pointer position. drop_hover says a drag from another program is
 * over w, or has left it. Every callback is optional. */
struct window_drag_target {
    void (*drag)(struct window *w, enum yasl_drop_action ev, struct vector2i pos,
                 void *ud);
    void (*drop_hover)(struct window *w, bool over, void *ud);
};
void window_set_drag_target(struct window *w, const struct window_drag_target *t,
                            void *ud);

/* The frame clock's now, in milliseconds. */
uint64_t window_frame_time_ms(struct window *w);
/* The surface's scale in 120ths. */
uint32_t window_scale_120(const struct window *w);
/* The default window size in cells, used when the config size is zero. */
#define WINDOW_DEFAULT_COLS 132
#define WINDOW_DEFAULT_ROWS 42
/* The config's window size, with a zero axis derived from the cell size. */
struct vector2i window_default_size(const struct window_config *config);
/* Asks for a frame when the window has a surface and is open: at once, or,
 * when asked while the window draws (an animation's next frame), once the
 * system shows the frame being drawn. */
void window_redraw(struct window *w);
/* Stores a copy of title and shows it. NULL shows the app name. */
void window_set_title(struct window *w, const char *title);
/* The stored title, or NULL. */
const char *window_title(const struct window *w);
void window_drain_input(struct window *w);
/* Queues ev as if the platform sent it. The window owns ev's payload
 * from here. False on allocation failure. */
bool window_inject_input(struct window *w, const struct input_event *ev);

struct window_pointer {
    struct vector2i pos;
    bool inside;
};

struct window_host *window_host(const struct window *w);
struct yasl_surface *window_surface(const struct window *w);
/* The system double-click interval in milliseconds, read on focus gain. */
uint32_t window_double_click_ms(const struct window *w);
struct frame_stats *window_stats(struct window *w);
bool window_closed(const struct window *w);
/* The surface size in pixels; zero without a surface. */
struct vector2i window_size(const struct window *w);
bool window_suspended(const struct window *w);
uint32_t window_frame_count(const struct window *w);
struct window_pointer window_pointer(const struct window *w);
/* Repaints every buffer in full on the next frames. */
void window_repaint_all(struct window *w);
/* Takes the full repaint request for the next frame. */
bool window_take_repaint(struct window *w);
/* Held keys, for encoding modifier sides. */
const struct input_keyboard_state *window_keyboard(const struct window *w);
/* Clipboard and selection, through the host's display. No-ops without one. */
void window_copy(struct window *w, enum yasl_selection sel, const char *text);
/* The text arrives later as a paste event in w's input queue. */
void window_paste(struct window *w, enum yasl_selection sel);
int window_modifiers(const struct window *w);
/* A point in logical units, as yasl takes it. It keeps the fraction. */
struct window_logical_point {
    double x, y;
};
/* Converts a device-pixel point of w to logical units. */
struct window_logical_point window_logical(const struct window *w,
                                           struct vector2i device);
/* Moves w, a new toplevel, so its logical point at is under the pointer. */
bool window_place_under_pointer(struct window *w, struct window_logical_point at);
/* Carries w, a toplevel in a drag, so its logical point at is under the pointer. */
bool window_drag_carry(struct window *w, struct window_logical_point at);

const char *window_ime_preedit(const struct window *w);
int window_ime_preedit_begin(const struct window *w);
int window_ime_preedit_end(const struct window *w);

#endif
