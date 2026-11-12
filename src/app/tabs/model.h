#ifndef APP_TABS_MODEL_H
#define APP_TABS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/yasl/window.h"
#include "util/geom.h"

struct terminal;

/* The ID of one app tab. Zero means no tab. A model gives each ID only
 * once. */
typedef uint32_t app_tab_id;

/* One tab. A live tab has content and no retire time. A closing tab keeps
 * its content and its label until the app retires it. */
struct app_tab {
    app_tab_id id;
    struct terminal *content;
    char *label;
    uint64_t retire_at_ms;
    /* The window that holds the tab, or NULL. The model borrows it. */
    struct window *window;
};

/* Opens an app window. A nonzero tab is a tab that no window holds. The
 * window holds it only on success. */
typedef struct window *(*app_tabs_open_window_fn)(void *ud, app_tab_id tab,
                                                  const char *cwd,
                                                  struct vector2i size);

/* The tab that a native drag moves. A zero value means no drag. */
struct app_tab_flight {
    bool running;
    /* The dragged tab, or 0. A carry window can hold it during the drag. */
    app_tab_id tab;
    bool taken;
    struct window *over;
    struct vector2i size;
    struct window_logical_point grab;
    struct window_host *host;
    app_tabs_open_window_fn open_window;
    void *ud;
};

/* The tab table of the app. It owns each terminal for its whole life.
 * Windows hold tab IDs and borrow the content widgets. A zero value is an
 * empty table. */
struct app_tab_model {
    /* The tabs in ID order. */
    struct app_tab *tabs;
    int count;
    int cap;
    app_tab_id last_id;
    struct app_tab_flight flight;
};

/*
 * Adds terminal `content` to `model` and assigns a new tab ID.
 * Destroys `content` and returns 0 on allocation failure.
 */
app_tab_id app_tab_model_add(struct app_tab_model *model,
                             struct terminal *content);
/* Finds the tab associated with `id` in `model`. Returns NULL if not found. */
struct app_tab *app_tab_model_get(const struct app_tab_model *model,
                                  app_tab_id id);
/* Finds the tab containing terminal `content` in `model`. Returns NULL if not found. */
struct app_tab *app_tab_model_find(const struct app_tab_model *model,
                                   const struct terminal *content);
/* Removes tab `id` from `model` and frees its terminal content and label. */
void app_tab_model_destroy(struct app_tab_model *model, app_tab_id id);
/* Destroys all tabs in `model` and frees model storage. */
void app_tab_model_fini(struct app_tab_model *model);

#endif
