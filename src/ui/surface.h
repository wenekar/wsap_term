#ifndef WSAP_UI_SURFACE_H
#define WSAP_UI_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/css.h"
#include "ui/event.h"
#include "ui/style_context.h"
#include "ui/widget.h"
#include "util/geom.h"

struct ui_scene;
struct ui_layout;
struct ui_dialog_frame;

/* request_frame queues a frame on the host loop. It must not draw inside
 * the call. */
struct ui_surface_host {
    void (*request_frame)(void *userdata);
    /* Optional host services. Text and targets are borrowed for the call. */
    void (*copy_text)(void *userdata, const char *text);
    /* Deliver clipboard text later as UI_EVENT_PASTE on this surface. */
    void (*paste_text)(void *userdata);
    void (*open_url)(void *userdata, const char *target);
    /* 0 disables double-click detection. Missing callback uses 400 ms. */
    uint32_t (*double_click_interval_ms)(void *userdata);
};

#define UI_DIALOG_MAX_BUTTONS 4
#define UI_SURFACE_SCRATCH_CAP 16384

struct ui_dialog_button {
    const char *text;
    int action_id;
};

/* Data-driven description for a transient modal dialog. Text is copied
 * during ui_dialog_show(). The callback userdata stays borrowed until the
 * dialog closes. Extra buttons above UI_DIALOG_MAX_BUTTONS are ignored. */
struct ui_dialog_desc {
    const char *title;
    const char *message;
    struct ui_dialog_button buttons[UI_DIALOG_MAX_BUTTONS];
    int button_count;
    ui_action_fn on_action;
    void *userdata;
};

/*
 * A surface shows one widget tree in one native surface. It owns focus,
 * pointer capture, hover, style, and frame requests for the tree.
 *
 * An all-zero surface is valid: it has no host, the default theme, and
 * scale 1.
 *
 * The frame: the host calls ui_surface_take_frame_request(), then
 * ui_surface_tick(), then ui_surface_build_scene(), and renders the scene.
 * After the frame it arms one timer at ui_surface_next_deadline() and
 * requests a frame when the timer fires.
 */
struct ui_surface {
    struct {
        struct ui_surface_host host;
        void *host_userdata;
        struct ui_widget *root;
        struct ui_widget *focused;
        struct ui_widget *captured;
        struct ui_widget *hovered;
        struct vector2i pointer;
        struct ui_widget *modal;
        struct ui_widget *restore_focus;
        struct ui_widget *default_focus;
        struct ui_widget *paste_target, *context_target;
        bool key_text_kept;
        struct vector2i viewport;
        struct ui_style_context style;
        struct ui_layout *layout;
        uint64_t deadline;
        bool frame_requested;
        bool busy;
        struct ui_widget *dialog;
        uint64_t dialog_generation;
        struct ui_dialog_frame *dialog_storage;
        struct ui_dialog_frame *retired_dialogs;
        size_t scratch_used;
        uint8_t scratch_buf[UI_SURFACE_SCRATCH_CAP];
    } impl;
};

/* Call once for fresh storage. Destroy before initializing it again.
 * Copies host. Borrows host_userdata until ui_surface_destroy(). */
void ui_surface_init(struct ui_surface *surface,
                     const struct ui_surface_host *host, void *host_userdata);
/* Removes the root from the surface and frees the surface's own memory.
 * The root's storage stays with the caller. Call outside UI callbacks. */
void ui_surface_destroy(struct ui_surface *surface);

/* The root must have no parent. Replacing it removes the old root.
 * Returns false when the root is invalid. */
bool ui_surface_set_root(struct ui_surface *surface, struct ui_widget *root);
struct ui_widget *ui_surface_root(const struct ui_surface *surface);

/* Borrows sheet until the next set_sheet call or surface destruction. */
void ui_surface_set_sheet(struct ui_surface *surface,
                          const struct css_sheet *sheet);
/* px, em, and ch values resolve with this scale. A change restyles and
 * measures the tree again. */
void ui_surface_set_scale(struct ui_surface *surface,
                          struct css_scale_context scale);
struct css_scale_context ui_surface_scale(const struct ui_surface *surface);
void ui_surface_resize(struct ui_surface *surface, struct vector2i viewport);
struct vector2i ui_surface_viewport(const struct ui_surface *surface);

/*
 * Keys, text, paste, drops, and shortcuts go to the focused widget. Without
 * focus they go to the active modal, then to the default focus, then to
 * the root. Key capture runs first on the path to that target. Then the
 * event bubbles from the target up. An unhandled Tab without Alt, Ctrl, or
 * Super moves focus; Shift+Tab moves it back.
 *
 * Pointer events go to the capturing widget, or else to the top visible
 * hit region, then bubble to its ancestors. They never fall through to a
 * covered sibling. A visible modal takes every pointer event that misses
 * its children. A primary press on a path with no focusable widget clears
 * focus unless a modal is active. A press whose target keeps press focus
 * leaves focus alone.
 *
 * True when a widget handled the event. A handled key also used its text:
 * the host drops the text event that follows it, unless a key handler
 * called ui_surface_keep_key_text().
 */
bool ui_surface_dispatch(struct ui_surface *surface,
                         const struct ui_event *event);

/* The widget that takes keys when nothing has focus and no modal is open.
 * It need not be attached yet. A widget that is not attached, hidden, or
 * retired falls back to the root. Focus does not change. */
void ui_surface_set_default_focus(struct ui_surface *surface,
                                  struct ui_widget *widget);
/* A key handler calls this when it handled the key, but the text that
 * comes with the key must still reach the UI. The flag resets at each
 * dispatch. */
void ui_surface_keep_key_text(struct ui_surface *surface);
bool ui_surface_key_text_kept(const struct ui_surface *surface);

void ui_surface_focus(struct ui_surface *surface, struct ui_widget *widget);
struct ui_widget *ui_surface_focused(const struct ui_surface *surface);
/* The widget that holds pointer capture, or NULL. */
struct ui_widget *ui_surface_captured(const struct ui_surface *surface);
struct ui_widget *ui_surface_context_target(const struct ui_surface *surface);
/* Borrows target while it remains attached to this surface. NULL clears it. */
void ui_surface_set_context_target(struct ui_surface *surface, struct ui_widget *target);
bool ui_surface_focus_next(struct ui_surface *surface);
bool ui_surface_focus_prev(struct ui_surface *surface);
/* Ends any pointer capture. The widget that held it gets
 * UI_EVENT_CAPTURE_LOST. Call it when the native surface loses the
 * pointer or the focus. */
void ui_surface_cancel_capture(struct ui_surface *surface);
/* True while a visible modal subtree, such as a palette, is open. */
bool ui_surface_has_modal(const struct ui_surface *surface);

/* Allocates zeroed memory from the surface scratch arena. Returns NULL
 * when size exceeds remaining capacity. */
void *ui_surface_scratch_alloc(struct ui_surface *surface, size_t size);
/* Invalidates all pointers from earlier scratch allocations. */
void ui_surface_scratch_reset(struct ui_surface *surface);

/* Shows a transient modal dialog. It wires modal capture, action routing,
 * and centered layout. */
struct ui_widget *ui_dialog_show(struct ui_surface *surface,
                                 const struct ui_dialog_desc *desc);

/* Closes and cleans up any active dialog on the surface. */
void ui_dialog_close(struct ui_surface *surface);
/* The pointer shape that the widget under the pointer asks for. */
enum ui_cursor ui_surface_cursor(const struct ui_surface *surface);

/* Advances every visible widget's animations and timers. True when a
 * widget needs a frame now. A future deadline is available below. */
bool ui_surface_tick(struct ui_surface *surface, uint64_t now_ms);
/* The earliest tick time a widget asked for at the last tick, or 0. */
uint64_t ui_surface_next_deadline(const struct ui_surface *surface);
/* False when the host does not supply the service. */
bool ui_surface_copy_text(struct ui_surface *surface, const char *text);
bool ui_surface_paste_text(struct ui_surface *surface);
bool ui_surface_open_url(struct ui_surface *surface, const char *target);
uint32_t ui_surface_double_click_interval_ms(
    const struct ui_surface *surface);
void ui_surface_request_frame(struct ui_surface *surface);
bool ui_surface_frame_pending(const struct ui_surface *surface);
/* The host takes the request when it starts a frame. Requests made during
 * the frame stay pending for the next one. */
bool ui_surface_take_frame_request(struct ui_surface *surface);

/* Brings content, style, size, and position up to date. Dispatch and
 * scene building do this; a host calls it only to read layout. */
void ui_surface_prepare(struct ui_surface *surface);
/* Prepares, then builds the scene. False when the scene is not valid. */
bool ui_surface_build_scene(struct ui_surface *surface,
                            struct ui_scene *scene);

#endif
