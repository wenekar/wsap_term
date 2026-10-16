#ifndef WINDOW_UI_H
#define WINDOW_UI_H

/* The window's UI client: the widget tree, the menus, the accessibility
 * tree, and the inspector that a window shows. Content reaches its
 * window's UI only through these functions. */

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "paint/damage.h"
#include "ui/css.h"
#include "ui/widgets/menu.h"
#include "util/geom.h"

struct font_instance;
struct json_writer;
struct ui_surface;
struct ui_scene;
struct ui_widget;
struct window;
struct window_ui;
struct window_frame;

/* UI data that the app adds to its render frame. It stays valid until
 * the app finishes drawing this frame. */
struct window_ui_frame {
    struct ui_scene *scene;
    struct ui_damage damage;
    struct font_instance *font;
    uint32_t background;
    bool repaint;
    bool gpu_off;
    /* The window is suspended: paint the background only. */
    bool suspended;
};

/* What a UI mount shows, and the hooks its content keeps. Every hook is
 * optional. The mount borrows root. */
struct window_ui_spec {
    /* For the inspector. */
    const char *name;
    /* Identity, so code can tell its own windows apart. */
    const void *kind;
    struct ui_widget *root;
    /* Points for UI text. Zero uses the terminal font size setting. */
    int ui_font_size;
    /* The style, scale, or config changed. */
    void (*restyle)(struct window *w);
    void (*resize)(struct window *w, struct vector2i size);
    void (*suspended)(struct window *w, bool suspended);
    /* Content state for the inspector's "windows" and "state" replies. */
    void (*debug_state)(struct window *w, struct json_writer *j);
    /* Frees the content. The window's timers are cancelled already. */
    void (*destroy)(struct window *w);
    void *content;
};

/* Shows spec on w and makes the mount w's client. False on failure;
 * then nothing is mounted and the caller still owns the content. */
bool window_ui_mount(struct window *w, const struct window_ui_spec *spec);
/* The kind w was mounted with, or NULL when w has no UI mount. */
const void *window_ui_kind(const struct window *w);
/* The UI state of w, opaque, or NULL when w has no UI mount. */
struct window_ui *window_ui_of(const struct window *w);

/* Ticks the main UI and opens any popup before content prepares. */
void window_ui_prepare_frame(struct window *w, uint64_t now_ms);
/* Builds the UI layer for the active main or popup surface. */
bool window_ui_build_frame(struct window *w, struct yasl_surface *surface,
                           uint64_t now_ms, struct window_ui_frame *out);
/* Reports the rendered frame and arms the next UI timer. */
void window_ui_frame_done(struct window *w, struct window_frame *frame,
                          uint64_t now_ms);

/* The surface that takes keys: the top menu popup, else the main one. */
struct ui_surface *window_ui_key_surface(struct window *w);
/* The DPI scale and the UI font's cell, for widgets that size by them. */
struct css_scale_context window_ui_scale(const struct window *w);
/* Reapplies scale and styles, and redraws. */
void window_ui_restyle(struct window *w);
/* Sets the current app appearance for this window. The sheet is borrowed. */
void window_ui_set_style(struct window *w, const struct css_sheet *sheet,
                         uint32_t background);
/* Reapplies scale and styles, lays out again, and redraws. */
void window_ui_relayout(struct window *w);
/* Hides the pointer until it moves, as typing does. */
void window_ui_hide_pointer(struct window *w);
/* Shows the shape the widget under the pointer asks for. */
void window_ui_update_cursor(struct window *w);

/* Opens a context menu at `at`, in native popups when the surface has
 * them. fn gets the menu's actions; submenu supplies submenu items. */
bool window_menu_open(struct window *w, const ui_menu_item *items, int n,
                      struct vector2i at, ui_action_fn fn,
                      ui_menu_submenu_fn submenu, void *userdata);
void window_menu_close(struct window *w);
/* Opens and closes the native popups of the open menu levels. Runs
 * after input and at the start of a frame, never inside a dispatch. */
void window_menu_sync(struct window *w);
bool window_menu_is_open(const struct window *w);

/* The window's main UI surface. */
struct ui_surface *window_ui_surface(struct window *w);
/* The widget that holds the content, under the menu layer. */
struct ui_widget *window_ui_root(struct window *w);
/* The UI font at the window's scale. */
struct font_instance *window_ui_font(const struct window *w);
/* The content the window's UI shows, or NULL. */
void *window_ui_content(const struct window *w);

#endif
