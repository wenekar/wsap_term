#ifndef APP_TABS_MODEL_H
#define APP_TABS_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/input.h"
#include "ui/yasl/window.h"
#include "util/geom.h"

struct font_instance;
struct ui_image;
struct ui_widget;
struct wsap_vtcore;
struct wsap_vtcore_theme;

/* The ID of one app tab. Zero means no tab. A model gives each ID only
 * once. */
typedef uint32_t app_tab_id;

/* The calls that a tab content provides. The tab code gets no window and no
 * session from the content; it uses these calls. The tab model owns the
 * content.
 *
 * The calls are optional, and a content leaves a call NULL when it has no
 * meaning for it. A zero table with a `destroy` call is thus a valid
 * content. */
struct app_tab_content_ops {
    /* Frees the content. Required. */
    void (*destroy)(void *content);
    /* Binds the content to `window`. A NULL `window` means that the content
     * is in transfer. */
    void (*set_window)(void *content, struct window *window);
    /* The root widget of the content, or NULL. */
    struct ui_widget *(*widget)(void *content);
    /* The widget that takes the default keyboard focus, or NULL. */
    struct ui_widget *(*input)(void *content);
    /* The title of the content, or NULL. */
    const char *(*title)(const void *content);
    /* The background color of the content. */
    uint32_t (*background)(const void *content);
    /* The font size of the content in points, or 0. A new tab takes the
     * size of the active tab. */
    int (*font_size)(const void *content);
    /* True when the content ended. An ended tab cannot bind to a window. */
    bool (*exited)(const void *content);
    /* Resizes the content to its laid out widget. */
    void (*resize)(void *content);
    /* Applies the window style to the content. */
    void (*restyle)(void *content);
    void (*set_theme)(void *content, const struct wsap_vtcore_theme *theme);
    /* Tells the content that its window gained or lost the focus. */
    void (*focus)(void *content, bool focused);
    /* Tells the content that the UI removed it from the display. */
    void (*unmount)(void *content);
    void (*paste)(void *content, const char *text, size_t len);
    /* Pastes the file list of a drop event. */
    void (*drop_files)(void *content, const char *data, size_t len);
    /* Shows or hides the file drop highlight of the content. */
    void (*drop_hover)(void *content, bool over);
    /* Runs the content command for `sc`. Returns true when the content size
     * may have changed. */
    bool (*shortcut)(void *content, input_command_id sc);
    /* Scrolls the content to its end. */
    void (*scroll_bottom)(void *content);
    /* Writes the working directory of the content to `out`. Returns false
     * when it has none. */
    bool (*cwd)(void *content, char *out, size_t size);
    /*
     * Renders the preview thumbnail of the content. It fits in `max_size`
     * device pixels. `state` belongs to the preview. Returns false on
     * failure. Zero `image->pixels` on success means: show the title only.
     */
    bool (*thumbnail)(void *content, struct vector2i max_size, void **state,
                      struct ui_image *image);
};

/* One tab. A live tab has content and no retire time. A closing tab keeps
 * its content and its label until the app retires it. */
struct app_tab {
    app_tab_id id;
    void *content;
    const struct app_tab_content_ops *ops;
    char *label;
    uint64_t retire_at_ms;
    /* The window that holds the tab, or NULL. The model borrows it. */
    struct window *window;
};

/* The title of the content of `tab`, or NULL. */
static inline const char *app_tab_title(const struct app_tab *tab)
{
    return tab && tab->content && tab->ops && tab->ops->title
        ? tab->ops->title(tab->content) : NULL;
}

/* True when the content of `tab` ended. A tab with no content did not end. */
static inline bool app_tab_exited(const struct app_tab *tab)
{
    return tab && tab->content && tab->ops && tab->ops->exited
        ? tab->ops->exited(tab->content) : false;
}

/* Opens an app window. A nonzero tab is a tab that no window holds. The
 * window holds it only on success. */
typedef struct window *(*app_tabs_open_window_fn)(void *ud, app_tab_id tab,
                                                  const char *cwd,
                                                  struct vector2i size);

/* The tab that a native drag moves. A zero value means no drag. */
struct app_tab_flight {
    struct window *over;
    struct window_logical_point grab;
    struct window_host *host;
    app_tabs_open_window_fn open_window;
    void *ud;
    struct vector2i size;
    /* The dragged tab, or 0. A carry window can hold it during the drag. */
    app_tab_id tab;
    bool running;
    bool taken;
};

/* The tab table of the app. It owns each content for its whole life.
 * Windows hold tab IDs and borrow the content widgets. A zero value is an
 * empty table. */
struct app_tab_model {
    /* The tabs in ID order. */
    struct app_tab *tabs;
    int count;
    int cap;
    struct app_tab_flight flight;
    app_tab_id last_id;
};

/*
 * Adds `content` to `model` and assigns a new tab ID. `ops` gives the calls
 * of the content. Destroys `content` and returns 0 on failure.
 */
app_tab_id app_tab_model_add(struct app_tab_model *model, void *content,
                             const struct app_tab_content_ops *ops);
/* Returns the tab with `id`, or NULL. */
struct app_tab *app_tab_model_get(const struct app_tab_model *model,
                                  app_tab_id id);
/* Returns the tab that holds `content`, or NULL. */
struct app_tab *app_tab_model_find(const struct app_tab_model *model,
                                   const void *content);
/* Removes tab `id` from `model` and frees its content and label. */
void app_tab_model_destroy(struct app_tab_model *model, app_tab_id id);
/* Destroys all tabs in `model` and frees model storage. */
void app_tab_model_fini(struct app_tab_model *model);

#endif
