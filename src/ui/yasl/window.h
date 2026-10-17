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

struct window_config {
    struct vector2i size;
    bool fullscreen;
    bool tearing;
    char *font_family;
    int font_size;
    int menu_hover_delay_ms;
};

struct window_host_hooks {
    void (*empty)(void *ud, struct window *last);
    void (*quit)(void *ud);
    void (*reopen)(void *ud);
    bool (*command)(void *ud, struct window *w, enum input_shortcut action);
    void *ud;
};

struct window_host {
    struct yasl_loop *loop;
    const struct window_config *config;
    struct window_host_hooks hooks;
    uint64_t service;
    uint64_t wake;
    uint64_t wake_at_ms;

    struct window **windows;
    int count;
    int cap;
    struct window *primary;
    struct window *closing;
    int holds;
    bool destroying;
};

void window_host_init(struct window_host *host, struct yasl_loop *loop,
                      const struct window_config *config,
                      const struct window_host_hooks *hooks);
void window_host_destroy(struct window_host *host);
void window_host_reap(struct window_host *host);
void window_host_schedule(struct window_host *host);
bool window_host_command(struct window *w, enum input_shortcut action);
void window_host_hold(struct window_host *host);
void window_host_release(struct window_host *host);
bool window_host_destroying(const struct window_host *host);

struct window_frame {
    struct yasl_surface *surface;
    uint64_t generation;
    struct framebuffer target;
};

struct window_geometry {
    struct vector2i size;
    uint32_t scale_120;
};

typedef void (*window_frame_composer)(struct window *w,
                                      struct window_frame *frame,
                                      void *ud);
void window_set_background(struct window *w, uint32_t bg);
uint32_t window_background(const struct window *w);

struct window_spec {
    const char *title;
    void *graphics_device;
    window_frame_composer compose;
    void *compose_ud;
    struct vector2i size;
    bool windowed;
};

struct window *window_open(struct window_host *host, const struct window_spec *spec);
struct window *window_create(struct window_host *host, const struct window_spec *spec);
void window_compose_frame(struct window *w, struct window_frame *frame);
/*
 * Shows the canvas of `frame`. `damage` lists the canvas pixels that
 * changed. NULL, full damage, or an empty list presents the whole canvas.
 * An overflowed list presents its bounds. `presented` is the GPU result
 * for a drawable frame. Returns true when the frame is on screen.
 */
bool window_present(struct window *w, struct window_frame *frame,
                    const struct damage_list *damage, bool presented);
bool window_native_add(struct window *w, struct yasl_surface *s);
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

uint64_t window_frame_time_ms(struct window *w);
uint32_t window_scale_120(const struct window *w);
#define WINDOW_DEFAULT_COLS 132
#define WINDOW_DEFAULT_ROWS 42
struct vector2i window_default_size(const struct window_config *config);
void window_redraw(struct window *w);
void window_set_title(struct window *w, const char *title);
const char *window_title(const struct window *w);
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
void window_copy(struct window *w, enum yasl_selection sel, const char *text);
void window_paste(struct window *w, enum yasl_selection sel);
int window_modifiers(const struct window *w);
struct window_logical_point {
    double x, y;
};
struct window_logical_point window_logical(const struct window *w,
                                           struct vector2i device);
bool window_place_under_pointer(struct window *w, struct window_logical_point at);
bool window_drag_carry(struct window *w, struct window_logical_point at);

const char *window_ime_preedit(const struct window *w);
int window_ime_preedit_begin(const struct window *w);
int window_ime_preedit_end(const struct window *w);

#endif
