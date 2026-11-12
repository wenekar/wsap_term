#ifndef APP_TABS_TABS_H
#define APP_TABS_TABS_H

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>
#include "app/tabs/model.h"
#include "input/input.h"
#include "terminal/terminal.h"
#include "util/geom.h"

struct app_tabs;
struct window;
struct font_instance;
struct ui_image;
struct ui_scene;

/* The time that a closing tab keeps its bar entry. The app retires the
 * entry and its content after this time. */
#define APP_TABS_CLOSE_MS 200

typedef bool (*app_tabs_preview_capture_fn)(void *ud, void **state,
                                            struct font_instance *font,
                                            struct wsap_vtcore *term, int width,
                                            struct ui_image *image);
typedef void (*app_tabs_preview_release_fn)(void *state);
typedef bool (*app_tabs_scene_paint_fn)(void *ud, struct font_instance *font,
                                        uint32_t *pixels,
                                        struct vector2i size,
                                        const struct ui_scene *scene);

struct app_tabs_spec {
    const struct terminal_config *config;
    const struct wsap_vtcore_theme *theme;
    struct terminal_theme_picker theme_picker;
    const char *cwd;
    /* The tab table of the app. It owns every terminal. The window holds
     * tab IDs only. */
    struct app_tab_model *model;
    /* A tab of the model that no window holds. The window holds it only on
     * success. */
    app_tab_id tab;
    app_tabs_open_window_fn open_window;
    app_tabs_preview_capture_fn preview_capture;
    app_tabs_preview_release_fn preview_release;
    void *preview_ud;
    app_tabs_scene_paint_fn scene_paint;
    void *scene_paint_ud;
    void *ud;
    /* When true, initializes tab model without creating an initial tab. */
    bool empty;
};

bool app_tabs_mount(struct window *window, const struct app_tabs_spec *spec);
bool app_tabs_is(const struct window *window);

struct terminal *app_tabs_active(const struct window *window);
/* Returns the count of live active tabs. */
int app_tabs_count(const struct window *window);
int app_tabs_active_index(const struct window *window);
/* Returns the reserved tab bar height in pixels. */
int app_tabs_bar_height(const struct window *window);

bool app_tabs_new(struct window *window);
bool app_tabs_new_window(struct window *window);
void app_tabs_close(struct window *window, int index);
void app_tabs_select(struct window *window, int index);
void app_tabs_reorder(struct window *window, int from, int to);

/*
 * Moves the tab at `index` from window `from` to window `to` at `slot`.
 * Appends to the end when `slot` is negative.
 * Returns true on success, or false on transfer failure.
 */
bool app_tabs_move(struct window *from, int index, struct window *to, int slot);
/*
 * Detaches the tab at `index` from `window` into a newly created window.
 * Returns the new window pointer, or NULL on failure.
 */
struct window *app_tabs_detach(struct window *window, int index);

void app_tabs_set_theme(struct window *window,
                        const struct wsap_vtcore_theme *theme);
/* Executes tab management shortcuts for `window`. */
void app_tabs_shortcut(struct window *window, enum input_shortcut sc);
/*
 * Retires expired closing tabs in `model` relative to `now_ms`.
 * Destroys associated terminal content and releases bar entries.
 */
void app_tabs_service(struct app_tab_model *model, uint64_t now_ms);
/* Updates UI layout and content mount geometry for the active tab in `window`. */
void app_tabs_relayout(struct window *window);

#endif
