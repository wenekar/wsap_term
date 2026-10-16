#ifndef APP_CONFIG_SETTINGS_H
#define APP_CONFIG_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

struct app_config;
struct look;
struct window;
struct window_host;

/* The app owns the values. The form copies these hooks. */
struct settings_host {
    const struct app_config *config;
    const char *config_path;
    struct look *look;
    bool (*apply)(void *ud, const struct app_config *draft);
    bool (*save)(void *ud, const struct app_config *draft,
                 const char *const *keys, size_t count);
    void *ud;
};

/* Shows the settings form on w, which has no UI yet. False on failure;
 * then the caller closes w. */
bool settings_mount(struct window *w, const struct settings_host *host);
/* The open settings window, or NULL. */
struct window *settings_find(struct window_host *host);

#endif
