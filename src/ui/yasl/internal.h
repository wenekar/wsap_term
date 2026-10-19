#ifndef WINDOW_INTERNAL_H
#define WINDOW_INTERNAL_H

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

#define WINDOW_NATIVE_MAX 16

/*
 * One native surface of a window.
 *
 * The canvas of a surface persists between frames, so the renderer paints
 * only what changed. The window marks a frame fresh when it cannot vouch
 * for the canvas:
 * - `vouched` is true when the canvas holds the last frame that the
 *   renderer drew with `history`, and the window presented that frame.
 *   Each frame clears it before it composes. window_present() sets it again
 *   when the present succeeds. Zero: the first frame is fresh.
 * - `canvas_size` and `canvas_scale_120` are the size and the scale of the
 *   last frame. A change makes the next frame fresh. A size change also
 *   covers a reallocation of the canvas, because only a size change
 *   reallocates it.
 * - window_canvas_forget() clears `vouched` on events that can change the
 *   canvas behind the window.
 * - A frame with a drawable is always fresh, until the Metal backend
 *   (milestone 6) keeps the previous frame for a drawable.
 */
struct window_native {
    struct yasl_surface *surface;
    struct framebuffer_history history;
    uint64_t generation;
    void *metal_layer;
    uint32_t *pixels;
    struct vector2i pixels_size;
    struct vector2i canvas_size;
    uint32_t canvas_scale_120;
    bool vouched;
    bool failing;
};

struct window_fifo {
    struct input_event *ev;
    size_t head;
    size_t len;
    size_t cap;
};

struct window_accessibility;
struct window_debug;

/*
 * One native menu popup. `damage` tracks the scenes of the popup surface,
 * as `window_ui.ui_damage` does for the main surface. So a popup frame
 * where no scope changed presents nothing.
 */
struct window_ui_menu_popup {
    struct yasl_surface *surface;
    struct ui_surface ui;
    struct ui_widget root;
    struct rect2i anchor;
    struct ui_damage_tracker damage;
};

struct window_ui {
    bool mounted;
    struct window_ui_spec spec;
    const struct css_sheet *sheet;
    uint32_t background;
    struct font_instance *ui_font;
    bool pointer_hidden;
    bool replace_key_text;
    struct window_debug *debug;

    struct ui_widget root;
    struct ui_surface screen;
    struct window_accessibility *accessibility;

    struct ui_menu menu;
    struct ui_menu_layer menu_layer;
    struct window_ui_menu_popup menu_popup[UI_MENU_MAX_LEVELS];
    int menu_popups;
    int menu_inline_from;
    struct vector2i menu_at;
    uint32_t menu_shown;

    struct ui_scene *scene_scratch;
    struct ui_damage_tracker ui_damage;

    bool suspended;
};

struct window {
    struct window_host *host;
    char *title;
    const struct window_drag_target *drag_target;
    void *drag_ud;
    struct yasl_surface *surface;
    struct window_native natives[WINDOW_NATIVE_MAX];
    int native_count;
    window_frame_composer compose;
    void *compose_ud;
    struct window_pointer pointer;
    struct input_keyboard_state keyboard;
    struct input_queue input;
    struct input_shortcut_state sc_state;
    bool key_text_pending;
    bool button_held;
    enum pointer_button held_button;
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
    bool needs_draw;
    uint64_t wake_ms;
    bool drawing;
    uint32_t scale_120;
    int modifiers;
    uint32_t bg;
    bool has_bg;
};

/* Makes the next frame of each native surface of `w` fresh. */
void window_canvas_forget(struct window *w);
bool window_host_add(struct window_host *host, struct window *w);
void window_wake_at(struct window *w, uint64_t at_ms);
void window_host_remove(struct window_host *host, struct window *w);
void window_destroy(struct window *w);
void window_host_dispatch_event(struct window_host *host, const struct yasl_event *ev);
bool window_inject_pop(struct window *w, struct input_event *ev);
void window_host_report_empty(struct window_host *host, struct window *last);

extern struct window_host *window_ui_host_last;

static inline struct window_ui *ui_of(const struct window *w)
{
    return (struct window_ui *)&w->ui;
}

static inline bool window_mounted(const struct window *w)
{
    return w->ui.mounted;
}

enum window_input_result {
    WINDOW_INPUT_UNHANDLED = 0,
    WINDOW_INPUT_HANDLED,
    WINDOW_INPUT_REPLACE_TEXT,
};
void window_ui_route(struct window *w, struct input_event *ev);
void window_ui_configure(struct window *w, struct window_geometry g);
void window_ui_state_changed(struct window *w);
void window_ui_closing(struct window *w);
void window_ui_destroy(struct window *w);
void window_ui_restyle(struct window *w);
struct ui_scene *window_ui_scene_scratch(struct window *w);
void window_menu_relayout(struct window *w);
struct ui_surface *window_ui_surface_of(struct window *w,
                                        const struct yasl_surface *s);
struct ui_surface *window_ui_surface_at(struct window *w, int popup);
void window_ui_root_init(struct window *w);
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
