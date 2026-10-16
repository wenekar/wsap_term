#ifndef TERMINAL_TERMINAL_H
#define TERMINAL_TERMINAL_H

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>
#include "util/geom.h"
#include "ui/yasl/window.h"

struct session;
struct tab;
struct window;
struct window_host;
struct font_instance;
struct wsap_vtcore_snapshot;
struct ui_image;
struct ui_scene;

typedef bool (*terminal_preview_capture_fn)(void *ud, void **state,
                                             struct font_instance *font,
                                             struct wsap_vtcore *term, int width,
                                             struct ui_image *image);
typedef void (*terminal_preview_release_fn)(void *state);
typedef bool (*terminal_scene_paint_fn)(void *ud, struct font_instance *font,
                                        uint32_t *pixels,
                                        struct vector2i size,
                                        const struct ui_scene *scene);

/* Terminal data that the app adds to a render frame. */
struct terminal_frame_source {
    struct wsap_vtcore_snapshot *snapshot;
    struct font_instance *font;
    struct rect2i rect;
    int origin_y;
    bool cursor_visible;
};

/* What a terminal reads from the app's config. The app owns it and
 * changes it in place. Every terminal window keeps the pointer it was
 * opened with. */
struct terminal_config {
    /* Owned by the app. */
    char *command;
    int scrollback;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_blink;
    bool visual_bell;
};

/* App callbacks for the terminal theme picker. */
struct terminal_theme_picker {
    int (*count)(void *ud);
    const char *(*label)(void *ud, int index);
    bool (*selected)(void *ud, int index);
    int (*previewed)(void *ud);
    bool (*preview)(void *ud, int index);
    void (*commit)(void *ud);
    void (*cancel)(void *ud);
    int (*rescan)(void *ud);
    void *ud;
};

/* What a terminal shows on a window, and how it asks for more windows. */
struct terminal_spec {
    const struct terminal_config *config;
    const struct wsap_vtcore_theme *theme;
    /* The theme picker of the context menu. Zero hides the picker. */
    struct terminal_theme_picker theme_picker;
    /* The tab to show. NULL starts a new tab in cwd. */
    struct tab *tab;
    /* Where a new tab starts. NULL starts where the shell starts. */
    const char *cwd;
    /* Starts no tab. For tests that add their own. */
    bool empty;
    /* Opens a window that shows tab, or a new tab in cwd, at size (zero
     * takes the settings size). NULL when it cannot; then tab is
     * untouched. Tear-off and New Window use it. NULL: no new windows. */
    struct window *(*open_window)(void *ud, struct tab *tab, const char *cwd,
                                  struct vector2i size);
    terminal_preview_capture_fn preview_capture;
    terminal_preview_release_fn preview_release;
    void *preview_ud;
    terminal_scene_paint_fn scene_paint;
    void *scene_paint_ud;
    void *ud;
};
/* Shows a terminal on w, which has no UI yet. A terminal never exists
 * without a tab unless spec->empty. False on failure: then the caller
 * closes w, and spec->tab is untouched. */
bool terminal_mount(struct window *w, const struct terminal_spec *spec);
/* True when w shows a terminal. */
bool terminal_is(const struct window *w);
/* Updates the terminal snapshot, IME caret, and closing-tab state. Does
 * nothing on a window that shows no terminal. */
void terminal_prepare(struct window *w, uint64_t now_ms);
/* Returns the active tab's stable data for the current app frame. False
 * on a window that shows no terminal. */
bool terminal_frame_source(struct window *w, uint64_t now_ms,
                           struct terminal_frame_source *out);
/* Updates the palette for this terminal and its tabs. */
void terminal_set_theme(struct window *w, const struct wsap_vtcore_theme *theme);

/* A new tab in the directory of the active one. */
bool terminal_new_tab(struct window *w);
/* Asks for a window the size of from, with a new tab in the directory
 * of from's active tab. */
struct window *terminal_new_window(struct window *from);
void terminal_close_tab(struct window *w, int index);
void terminal_select_tab(struct window *w, int index);
void terminal_reorder_tab(struct window *w, int from, int to);
struct tab *terminal_remove_tab(struct window *w, int index);
bool terminal_attach(struct window *w, struct tab *t, int slot);
/* Moves tab index of w into a new window of its own. NULL if w has only
 * that tab or the window cannot open; w keeps the tab then. */
struct window *terminal_detach_tab(struct window *w, int index);

/* Any window can ask these. A window that shows no terminal has no tabs:
 * 0 tabs, active index -1, no session, and no tab bar. */
int terminal_tab_count(const struct window *w);
int terminal_active_index(const struct window *w);
struct session *terminal_active_session(const struct window *w);
int terminal_tabbar_height(const struct window *w);

#endif
