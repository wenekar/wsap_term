#ifndef WINDOW_INTERNAL_H
#define WINDOW_INTERNAL_H

/* The window's state and the helpers its files share. Only the window's
 * files include this header. The window's one client is the UI client,
 * so its state is part of the window. */

#include <stddef.h>
#include <stdint.h>

#include "yasl/yasl.h"
#include "input/input.h"
#include "paint/framebuffer.h"
#include "ui/damage.h"
#include "ui/surface.h"
#include "ui/widget_impl.h"
#include "ui/widgets/menu.h"
#include "util/damage.h"
#include "ui/yasl/stats.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

#define YASL_SCALE_UNIT 120

/* The most native surfaces one window holds: the main one and its menu
 * popups. */
#define WINDOW_NATIVE_MAX 16

/* What the window keeps for one native surface: its render history and
 * the damage of the frame being drawn. A Metal surface also keeps its
 * layer and the CPU pixels that render falls back to. */
struct window_native {
    struct yasl_surface *surface;
    struct framebuffer_history history;
    struct damage_list damage;
    uint64_t generation;
    void *metal_layer;
    uint32_t *pixels;
    struct vector2i pixels_size;
    /* The last draw or present failed. The next failure logs nothing. */
    bool failing;
};

/* Injected input. The window drains it before native input. It grows
 * instead of dropping: a command may queue far more than the platform
 * queue holds. */
struct window_fifo {
    struct input_event *ev;
    size_t head;
    size_t len;
    size_t cap;
};

struct window_accessibility;
struct window_debug;

/* A menu level in a native popup: a UI surface of its own, whose root
 * holds the level inside the shadow margin. */
struct window_ui_menu_popup {
    struct yasl_surface *surface;
    struct ui_surface ui;
    struct ui_widget root;
    struct rect2i anchor;
};

/* The UI client's state. It lives inside the window. Zero means the
 * window has no UI mount. */
struct window_ui {
    /* Set by window_ui_mount. */
    bool mounted;
    struct window_ui_spec spec;
    const struct css_sheet *sheet;
    uint32_t background;
    /* The settings' font size at the surface's scale: the chrome's font. */
    struct font_instance *ui_font;
    /* Typing hid the pointer until it moves. */
    bool pointer_hidden;
    /* The preceding key replaced its associated text. */
    bool replace_key_text;
    /* UI inspector state; NULL until the inspector or overlay is used. */
    struct window_debug *debug;

    /* The window's root: the content root, then the inline menu levels. */
    struct ui_widget root;
    struct ui_surface screen;
    struct window_accessibility *accessibility;

    /* One context menu at a time. The first menu_popups levels show in
     * native popups; menu_layer shows the rest over the content. */
    struct ui_menu menu;
    struct ui_menu_layer menu_layer;
    struct window_ui_menu_popup menu_popup[UI_MENU_MAX_LEVELS];
    int menu_popups;
    /* The first depth shown inline: a popup failed there. */
    int menu_inline_from;
    struct vector2i menu_at;
    uint32_t menu_shown;

    struct ui_scene *scene_scratch;
    /* The scopes of the last UI scene drawn in the main surface. */
    struct ui_damage_tracker ui_damage;

    /* The suspended state the UI client last passed to its content. */
    bool suspended;
};

struct window {
    struct window_host *host;
    /* Owned. The title the surface shows; NULL shows the app name. */
    char *title;
    /* Where drags reach the window. NULL takes none. */
    const struct window_drag_target *drag_target;
    void *drag_ud;
    struct yasl_surface *surface;
    struct window_native natives[WINDOW_NATIVE_MAX];
    int native_count;
    window_frame_composer compose;
    void *compose_ud;
    /* Native pointer state, kept by the input drain. */
    struct window_pointer pointer;
    struct input_keyboard_state keyboard;
    /* Native input after shortcut routing, drained before the UI. */
    struct input_queue input;
    struct input_shortcut_state sc_state;
    /* The last event was a key press with text. Committed text that
     * follows belongs to that key. */
    bool key_text_pending;
    /* Pointer button held down, from press to release. yasl reports the
     * button only on press and release. */
    bool button_held;
    enum pointer_button held_button;
    /* The system double-click interval. Zero until the window gains focus. */
    uint32_t double_click_ms;
    struct window_fifo inject;
    struct window_ui ui;

    struct frame_stats stats;
    uint32_t frame;

    struct window *next_closing;
    bool closed;
    bool suspended;
    bool repaint;

    char *ime_preedit;
    int ime_begin;
    int ime_end;
    bool focused;
    /* Asked for a frame; the host service draws it. */
    bool needs_draw;
    /* When the window wants its next frame, in ms of yasl_time_ns(), or 0.
     * Set it with window_wake_at(). */
    uint64_t wake_ms;
    /* Inside window_native_draw. */
    bool drawing;
    uint32_t scale_120;
    int modifiers;
};

/* Window internals the window files share. */
bool window_host_add(struct window_host *host, struct window *w);
/* Asks for a frame at at_ms, in ms of yasl_time_ns(); 0 cancels. The host
 * keeps one timer for every window. It finds the windows when it fires, so a
 * window needs no cleanup when it closes. */
void window_wake_at(struct window *w, uint64_t at_ms);
void window_host_remove(struct window_host *host, struct window *w);
void window_destroy(struct window *w);
void window_host_dispatch_event(struct window_host *host, const struct yasl_event *ev);
/* Takes the next injected event. False when none waits. */
bool window_inject_pop(struct window *w, struct input_event *ev);
/* Tells the app when the host has no windows and no holds. A host being
 * destroyed closes its windows on purpose and reports nothing. */
void window_host_report_empty(struct window_host *host, struct window *last);

/* The inspector starts from the latest mounted UI host. */
extern struct window_host *window_ui_host_last;

/* The UI state of w, without a mount check. Only code that runs under the
 * UI client calls it. */
static inline struct window_ui *ui_of(const struct window *w)
{
    return (struct window_ui *)&w->ui;
}

static inline bool window_mounted(const struct window *w)
{
    return w->ui.mounted;
}

/* What the route did with a key or text: its replace flag comes from it. */
enum window_input_result {
    WINDOW_INPUT_UNHANDLED = 0,
    WINDOW_INPUT_HANDLED,
    /* A handled key also replaces its associated text. */
    WINDOW_INPUT_REPLACE_TEXT,
};
/* The UI client's entry points. The window calls them only when it is
 * mounted. Keys, pointer, and text reach the UI through window_ui_route. */
void window_ui_route(struct window *w, struct input_event *ev);
void window_ui_configure(struct window *w, struct window_geometry g);
/* The window's suspended state changed. */
void window_ui_state_changed(struct window *w);
/* The window is closing; its surface goes next. */
void window_ui_closing(struct window *w);
/* At reap, before the surface goes and its timers with it. */
void window_ui_destroy(struct window *w);
void window_ui_restyle(struct window *w);
/* A scene to build into, reset; valid until the next call. */
struct ui_scene *window_ui_scene_scratch(struct window *w);
/* Measures and places the open menu popups again, after the window's
 * scale or style changed. */
void window_menu_relayout(struct window *w);
/* The UI surface of a native surface of w: the main one or a popup. */
struct ui_surface *window_ui_surface_of(struct window *w,
                                        const struct yasl_surface *s);
/* The UI surface at a popup number of input: 0 is the main one. */
struct ui_surface *window_ui_surface_at(struct window *w, int popup);
void window_ui_root_init(struct window *w);
/* After a frame: wake the window at the earliest tick deadline of its UI
 * surfaces. */
void window_ui_arm(struct window *w, uint64_t now_ms);
void window_ui_root_destroy(struct window *w);
void window_accessibility_create(struct window *w);
void window_accessibility_stop(struct window *w);
void window_accessibility_destroy(struct window *w);
void window_accessibility_publish(struct window *w);
void window_accessibility_menu_popup_open(struct window *w, int depth);
void window_accessibility_menu_popup_close(struct window *w, int depth);
extern const struct ui_surface_host window_ui_surface_host;
void window_menu_destroy(struct window *w);

#endif
