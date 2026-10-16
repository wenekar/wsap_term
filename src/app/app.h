#ifndef APP_H
#define APP_H

#include <stdbool.h>

#include "app/config/config.h"
#include "app/stats.h"
#include "ui/yasl/window.h"

struct tab;
struct look;
struct renderer;

struct app {
    struct app_config config;
    char *config_path;
    struct yasl_loop *loop;
    struct renderer *renderer;
    struct look *look;
    struct window_host host;
    struct app_stats stats;
    char **argv;
    int argc;
    bool render_started;
};

struct app *app_create(int argc, char **argv, int *status);
void app_destroy(struct app *a);
int app_run(struct app *a);
struct window *app_open_terminal(struct app *a, struct tab *tab,
                                 const char *cwd, struct vector2i size);

bool app_apply_config(struct app *a, const struct app_config *draft);
bool app_save_config(struct app *a, const struct app_config *draft,
                     const char *const *keys, size_t count);

bool app_watch_signals(struct app *a);

#endif
