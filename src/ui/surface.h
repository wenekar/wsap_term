#ifndef WSAP_UI_SURFACE_H
#define WSAP_UI_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/css.h"
#include "ui/event.h"
#include "ui/style_context.h"
#include "ui/widget.h"
#include "util/geom.h"

struct ui_scene;

struct ui_surface_host {
    void (*request_frame)(void *userdata);
    void (*copy_text)(void *userdata, const char *text);
    void (*paste_text)(void *userdata);
    void (*open_url)(void *userdata, const char *target);
    uint32_t (*double_click_interval_ms)(void *userdata);
};

/* A function that ui_surface_post queues. The surface passes itself and the
 * user data that the poster gave. */
typedef void (*ui_post_fn)(struct ui_surface *surface, void *userdata);

struct ui_surface_post {
    ui_post_fn fn;
    void *userdata;
};

/* What a handler may do while the surface dispatches, ticks, or builds.
 *
 * A handler can detach or destroy any widget, itself included. It must not
 * free the storage of a widget or a surface while the surface is busy: the
 * dispatch still reads the surface, the tree, and the stored targets after the
 * handler returns.
 *
 * An owner that must free storage calls ui_surface_post. The function runs
 * after the outermost busy section ends, so no code uses the storage then.
 * ui_surface_destroy returns false for a busy surface, and asserts in a debug
 * build. A busy owner posts its own free instead.
 *
 * The caller of an entry point can still read the surface after it returns, so
 * a posted function must not free the storage of the surface it runs on. It
 * may destroy that surface, and it may free any other storage.
 *
 * State that a handler can change stays readable while the surface is busy.
 * A stored target is thus safe to test, but not to use: any use follows a
 * ui_widget_stale test.
 */

/* One level of the modal focus stack. `modal` took the keyboard focus, and
 * `focus` held it before, or is NULL when nothing had it. */
struct ui_surface_focus_frame {
    struct ui_widget *modal;
    struct ui_widget *focus;
};

struct ui_surface {
    struct {
        struct ui_surface_host host;
        void *host_userdata;
        struct ui_widget *root;
        /* Ordered from the bottom up. Each overlay sits above the root. The
         * list grows as the surface takes overlays. */
        struct ui_widget **overlays;
        size_t overlay_count;
        size_t overlay_cap;
        struct ui_widget *focused;
        struct ui_widget *captured;
        struct ui_widget *hovered;
        struct vector2i pointer;
        /* The topmost modal widget. It masks the widgets below it. */
        struct ui_widget *modal;
        /* The modals that took the focus, from the bottom up. A modal closes
         * out of order, so this stack is not a property of one widget. */
        struct ui_surface_focus_frame *focus_stack;
        size_t focus_count;
        size_t focus_cap;
        struct ui_widget *default_focus;
        struct ui_widget *paste_target, *context_target;
        struct vector2i viewport;
        struct ui_style_context style;
        uint64_t deadline;
        /* The next step of a CSS transition, or 0. */
        uint64_t anim_deadline;
        /* The time of the last tick. A transition starts at this time. */
        uint64_t now_ms;
        uint64_t generation;
        struct ui_surface_post *post;
        size_t post_count;
        size_t post_cap;
        bool key_text_kept;
        bool frame_requested;
        bool busy;
    } impl;
};

void ui_surface_init(struct ui_surface *surface,
                     const struct ui_surface_host *host, void *host_userdata);
/* Releases the root, the overlays, and the styles of `surface`. Returns false
 * when the surface is busy, and asserts in a debug build. */
bool ui_surface_destroy(struct ui_surface *surface);
/* Queues `fn` to run after the outermost busy section of `surface` ends, or
 * runs it now when the surface is idle. A function that runs then may post
 * again. Returns false only when memory is short. */
bool ui_surface_post(struct ui_surface *surface, ui_post_fn fn, void *userdata);

/* True while the surface dispatches, ticks, or builds, and reads its stored
 * targets. Use ui_surface_post to free storage then. */
bool ui_surface_busy(const struct ui_surface *surface);

bool ui_surface_set_root(struct ui_surface *surface, struct ui_widget *root);
struct ui_widget *ui_surface_root(const struct ui_surface *surface);

/* Adds `overlay` above the root, or above the overlays already there. The
 * surface places it with its ui_placement against the viewport, tests hits on
 * it before the root, and paints it after the root. The widget must be
 * detached. The list grows, so the surface takes any number of overlays.
 * Returns false only when memory is short. */
bool ui_surface_add_overlay(struct ui_surface *surface,
                            struct ui_widget *overlay);
/* Removes `overlay` from the list and detaches it from the surface. */
bool ui_surface_remove_overlay(struct ui_surface *surface,
                               struct ui_widget *overlay);
size_t ui_surface_overlay_count(const struct ui_surface *surface);
/* The overlay at `index`, counted from the bottom up. */
struct ui_widget *ui_surface_overlay_at(const struct ui_surface *surface,
                                        size_t index);
/* Returns the widget under `pos`: the topmost overlay that covers it, or else
 * a widget of the root tree. */
struct ui_widget *ui_surface_hit_test(struct ui_surface *surface,
                                      struct vector2i pos);

void ui_surface_set_sheet(struct ui_surface *surface,
                          const struct css_sheet *sheet);
/* Sets a sheet that applies above the surface sheet, such as a theme sheet. */
void ui_surface_set_overlay_sheet(struct ui_surface *surface,
                                  const struct css_sheet *sheet);
void ui_surface_set_scale(struct ui_surface *surface,
                          struct css_scale_context scale);
struct css_scale_context ui_surface_scale(const struct ui_surface *surface);
void ui_surface_resize(struct ui_surface *surface, struct vector2i viewport);
struct vector2i ui_surface_viewport(const struct ui_surface *surface);

bool ui_surface_dispatch(struct ui_surface *surface,
                         const struct ui_event *event);

/* Set the widget that takes keyboard events when nothing is focused. */
void ui_surface_set_default_focus(struct ui_surface *surface,
                                  struct ui_widget *widget);
void ui_surface_keep_key_text(struct ui_surface *surface);
bool ui_surface_key_text_kept(const struct ui_surface *surface);

/* Gives the keyboard focus to `widget`, or to no widget when it is NULL.
 * The previous widget gets FOCUS_OUT first. A widget that no longer belongs
 * to `surface` is ignored. */
void ui_surface_focus(struct ui_surface *surface, struct ui_widget *widget);
struct ui_widget *ui_surface_focused(const struct ui_surface *surface);
struct ui_widget *ui_surface_captured(const struct ui_surface *surface);
struct ui_widget *ui_surface_context_target(const struct ui_surface *surface);
void ui_surface_set_context_target(struct ui_surface *surface, struct ui_widget *target);
bool ui_surface_focus_next(struct ui_surface *surface);
bool ui_surface_focus_prev(struct ui_surface *surface);
void ui_surface_cancel_capture(struct ui_surface *surface);
bool ui_surface_has_modal(const struct ui_surface *surface);

enum ui_cursor ui_surface_cursor(const struct ui_surface *surface);

bool ui_surface_tick(struct ui_surface *surface, uint64_t now_ms);
uint64_t ui_surface_next_deadline(const struct ui_surface *surface);
/* Returns true when the host of `surface` can copy text. */
bool ui_surface_can_copy(const struct ui_surface *surface);
/* Returns true when the host of `surface` can paste text. */
bool ui_surface_can_paste(const struct ui_surface *surface);
bool ui_surface_copy_text(struct ui_surface *surface, const char *text);
bool ui_surface_paste_text(struct ui_surface *surface);
bool ui_surface_open_url(struct ui_surface *surface, const char *target);
uint32_t ui_surface_double_click_interval_ms(
    const struct ui_surface *surface);
/* The double-click interval, in ms, when no host reports one. */
#define UI_DOUBLE_CLICK_FALLBACK_MS 400
void ui_surface_request_frame(struct ui_surface *surface);
bool ui_surface_frame_pending(const struct ui_surface *surface);
bool ui_surface_take_frame_request(struct ui_surface *surface);

void ui_surface_prepare(struct ui_surface *surface);
bool ui_surface_build_scene(struct ui_surface *surface,
                            struct ui_scene *scene);

#endif
