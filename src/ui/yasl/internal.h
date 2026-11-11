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

struct window_native {
    struct yasl_surface *surface;
    struct framebuffer_history history;
    uint64_t generation;
    void *metal_layer;
    uint32_t *pixels;
    /* The allocated words of `pixels`. A resize reuses them while they fit. */
    size_t pixels_cap;
    /* The presentations of this surface, as the display reports them. It is
     * NULL for a surface without a Metal layer, which reports no feedback. */
    struct present_watch *watch;
    /* A draw of this surface is due in the next host pass. Each native surface
     * has its own intent, so a popup refresh draws only that popup. */
    bool needs_draw;
    /* The frame of the last draw went to a drawable, and the GPU or the surface
     * submitted it. A drawable frame that did not submit is asked for again. */
    bool frame_drawable;
    bool frame_submitted;
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

struct window_ui_menu_popup {
    struct yasl_surface *surface;
    struct ui_surface ui;
    struct ui_widget root;
    struct rect2i anchor;
    /* Set when the popup closed while its surface was busy. The popup
     * is destroyed as soon as the surface is idle. */
    bool closing;
};

struct window_ui {
    struct window_ui_spec spec;
    const struct css_sheet *sheet;
    struct font_instance *ui_font;
    struct window_debug *debug;
    struct window_accessibility *accessibility;
    struct ui_scene *scene_scratch;

    struct ui_widget root;
    struct ui_surface screen;

    struct ui_menu menu;
    struct ui_menu_layer menu_layer;
    struct window_ui_menu_popup menu_popup[UI_MENU_MAX_LEVELS];
    struct ui_damage_tracker ui_damage;

    struct vector2i menu_at;
    uint32_t background;
    int menu_popups;
    int menu_inline_from;
    uint32_t menu_shown;

    bool mounted;
    bool pointer_hidden;
    bool replace_key_text;
    bool suspended;
};

struct window {
    struct window_host *host;
    char *title;
    const struct window_drag_target *drag_target;
    void *drag_ud;
    struct yasl_surface *surface;
    window_frame_composer compose;
    void *compose_ud;
    struct window *next_closing;
    char *ime_preedit;
    uint64_t wake_ms;

    struct window_native natives[WINDOW_NATIVE_MAX];
    struct input_keyboard_state keyboard;
    struct input_queue input;
    struct input_shortcut_state sc_state;
    struct yasl_event native_key;
    struct window_fifo inject;
    struct window_ui ui;
    struct frame_stats stats;

    struct window_pointer pointer;
    uint32_t bg;

    int native_count;
    enum pointer_button held_button;
    uint32_t double_click_ms;
    uint32_t frame;
    int ime_begin;
    int ime_end;
    uint32_t scale_120;
    int modifiers;

    bool key_text_pending;
    bool button_held;
    bool closed;
    bool suspended;
    bool repaint;
    bool focused;
    /* The configuration permits unpaced presentation: draw at once. */
    bool unpaced;
    bool has_bg;
};

bool window_host_add(struct window_host *host, struct window *w);
void window_wake_at(struct window *w, uint64_t at_ms);
void window_host_remove(struct window_host *host, struct window *w);
void window_destroy(struct window *w);
void window_host_dispatch_event(struct window_host *host, const struct yasl_event *ev);
bool window_inject_pop(struct window *w, struct input_event *ev);
void window_host_report_empty(struct window_host *host, struct window *last);
/* Runs the service step of the owner. See `window_host_hooks.service`. */
void window_host_run_service(struct window_host *host, uint64_t now_ms,
                             bool modal);

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
void window_menu_reap(struct window *w);
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
