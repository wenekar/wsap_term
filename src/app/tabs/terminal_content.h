#ifndef APP_TABS_TERMINAL_CONTENT_H
#define APP_TABS_TERMINAL_CONTENT_H

#include "app/tabs/model.h"
#include "terminal/terminal.h"

struct font_instance;
struct ui_image;
struct wsap_vtcore;

/* Renders the preview thumbnail of a terminal. `ud` belongs to the app. */
typedef bool (*app_tabs_preview_capture_fn)(void *ud, void **state,
                                            struct font_instance *font,
                                            struct wsap_vtcore *term, int width,
                                            struct ui_image *image);

/* The terminal settings that the app passes to the tab content adapter. The
 * adapter copies the structure. */
struct app_terminal_content_spec {
    const struct terminal_config *config;
    const struct wsap_vtcore_theme *theme;
    struct terminal_theme_picker theme_picker;
    app_tabs_preview_capture_fn preview_capture;
    void *preview_ud;
};

/* The content calls of a terminal tab. */
extern const struct app_tab_content_ops app_terminal_content_ops;

/*
 * Creates a terminal content. The tab model owns the result. The content sends
 * its host reports through `model`. `font_size` is the size of the active tab,
 * or 0. Returns NULL on failure.
 */
void *app_terminal_content_create(struct window *window,
                                  struct app_tab_model *model, const char *cwd,
                                  struct vector2i content_size, int font_size,
                                  const struct app_terminal_content_spec *spec);

#endif
