#ifndef APP_H
#define APP_H

#include <stdbool.h>

#include "app/config/config.h"
#include "app/stats.h"
#include "app/tabs/model.h"
#include "ui/yasl/window.h"

struct look;
struct renderer;

struct app {
    struct app_config config;
    /* The terminal palette of the active theme, over the configured one. */
    struct wsap_vtcore_theme palette;
    char *config_path;
    struct yasl_loop *loop;
    struct renderer *renderer;
    struct look *look;
    struct window_host host;
    /* The tab table. It owns every terminal of the app. Windows hold tab
     * IDs. */
    struct app_tab_model tabs;
    struct app_stats stats;
    char **argv;
    uint32_t *cursor_px;
    size_t cursor_px_cap;
    uint64_t gpu_idle_timer;
    uint64_t last_gpu_draw_ms;
    int argc;
    bool render_started;
};

struct app *app_create(int argc, char **argv, int *status);
/* Like app_create(), but it creates the GPU device right after it reads the
 * configuration. The driver load sets the peak RSS of the startup. At this
 * point the font system and the window are not resident yet. Use it only
 * when a window opens next. app_create() leaves the device to the first
 * window. */
struct app *app_create_gpu_first(int argc, char **argv, int *status);
void app_destroy(struct app *a);
int app_run(struct app *a);
/* Opens a terminal window. A nonzero tab is a tab of the app that no window
 * holds. The window holds it only on success. A zero tab starts a new
 * terminal in `cwd`. */
struct window *app_open_terminal(struct app *a, app_tab_id tab,
                                 const char *cwd, struct vector2i size);

bool app_apply_config(struct app *a, const struct app_config *draft);
bool app_save_config(struct app *a, const struct app_config *draft,
                     const char *const *keys, size_t count);

bool app_watch_signals(struct app *a);

#endif
