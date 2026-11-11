#ifndef APP_TABS_TABS_H
#define APP_TABS_TABS_H

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>
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
    /* A content to attach to the window. The mount takes ownership only on
     * success. */
    struct terminal *content;
    /* Open no tab. The caller then adds the first tab. */
    bool empty;
    struct window *(*open_window)(void *ud, struct terminal *content,
                                  const char *cwd, struct vector2i size);
    app_tabs_preview_capture_fn preview_capture;
    app_tabs_preview_release_fn preview_release;
    void *preview_ud;
    app_tabs_scene_paint_fn scene_paint;
    void *scene_paint_ud;
    void *ud;
};

bool app_tabs_mount(struct window *window, const struct app_tabs_spec *spec);
bool app_tabs_is(const struct window *window);

struct terminal *app_tabs_active(const struct window *window);
/* The count of the live tabs. A closing tab is not live. */
int app_tabs_count(const struct window *window);
int app_tabs_active_index(const struct window *window);
/* The reserved bar height. Closing entries keep this space until their
 * presentation ends. */
int app_tabs_bar_height(const struct window *window);

bool app_tabs_new(struct window *window);
bool app_tabs_new_window(struct window *window);
void app_tabs_close(struct window *window, int index);
void app_tabs_select(struct window *window, int index);
void app_tabs_reorder(struct window *window, int from, int to);

/* Takes the content out of the window. The caller then owns it. */
struct terminal *app_tabs_take(struct window *window, int index);
/* Adds a content that the caller owns. The window owns it on success. */
bool app_tabs_attach(struct window *window, struct terminal *content, int slot);
/* Moves a tab to a new window. The source keeps the content on failure. */
struct window *app_tabs_detach(struct window *window, int index);

void app_tabs_set_theme(struct window *window,
                        const struct wsap_vtcore_theme *theme);
/* Runs the tab commands and leaves the other shortcuts to the content. */
void app_tabs_shortcut(struct window *window, enum input_shortcut sc);
/* Applies the pending tab changes before the UI layout. */
void app_tabs_prepare(struct window *window, uint64_t now_ms);
/* Applies the pending tab changes, lays the UI out, and commits the
 * geometry of the active content. */
void app_tabs_relayout(struct window *window);

#endif
