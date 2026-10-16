#ifndef APP_H
#define APP_H

#include <stdbool.h>

#include "app/config/config.h"
#include "app/stats.h"
#include "ui/yasl/window.h"

struct tab;
struct look;
struct renderer;

/* The composition root: the config, the loop, the look, and the windows,
 * wired together. Code below it sees only the window host. */
struct app {
    /* Owned. Windows keep pointers into it. */
    struct app_config config;
    /* Owned. The file the config loads from and saves to. NULL means no file. */
    char *config_path;
    /* Owned. Freed last, after the host. */
    struct yasl_loop *loop;
    /* Owned. Every window uses this renderer. */
    struct renderer *renderer;
    /* Owned. The host borrows it. */
    struct look *look;
    /* Owned. It owns the windows. */
    struct window_host host;
    /* Owned. It borrows the loop and the host. */
    struct app_stats stats;
    /* Owned. A copy of the arguments; a config reload reads them again. */
    char **argv;
    int argc;
    /* app_create() started the render system, so app_destroy() stops it. */
    bool render_started;
};

/* Loads the config from argv, the environment, and the config file.
 * Returns NULL on failure. Then *status is the exit status: 0 after
 * --help, otherwise the error code. */
struct app *app_create(int argc, char **argv, int *status);
void app_destroy(struct app *a);
/* Opens the first window and runs the loop. On macOS, the app stays open
 * after its last window closes. Returns the process exit status. */
int app_run(struct app *a);
/* Opens a terminal window that shows tab, or a new tab in cwd, at size
 * (zero takes the settings size). NULL when it cannot; tab is untouched. */
struct window *app_open_terminal(struct app *a, struct tab *tab,
                                 const char *cwd, struct vector2i size);

/* Applies a changed config. The app copies it. Windows see the change
 * through their restyle. False, with the config unchanged, on failure. */
bool app_apply_config(struct app *a, const struct app_config *draft);
/* Writes the keys that changed in draft to the config file. */
bool app_save_config(struct app *a, const struct app_config *draft,
                     const char *const *keys, size_t count);

/* Routes SIGINT, SIGTERM, SIGCHLD and SIGUSR1 through the loop. app_run()
 * does this itself; tests that spawn shells call it to reap them. */
bool app_watch_signals(struct app *a);

#endif
