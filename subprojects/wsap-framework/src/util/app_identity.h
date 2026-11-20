#ifndef UTIL_APP_IDENTITY_H
#define UTIL_APP_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#define APP_IDENTITY_PATH_MAX 4096

/*
 * The identity of one application: its app ID, the directories that it reads
 * and writes, and the directory of its installed data. The application builds
 * one of these at startup and passes it to the modules that need a name or a
 * path.
 *
 * An all-zero identity is valid and names nothing. A module that receives a
 * zero identity cannot read a config path.
 */
struct app_identity {
    char id[128];
    char config_dir[APP_IDENTITY_PATH_MAX];
    char cache_dir[APP_IDENTITY_PATH_MAX];
    char data_dir[APP_IDENTITY_PATH_MAX];
};

/*
 * Fills `out` for `app_id` and zeroes it first. The config and cache
 * directories end with the app ID. The data directory is the installed data of
 * the app, and it is empty when neither the executable nor the build prefix
 * provides it. Returns false for a NULL or empty `app_id`, an app ID or a
 * directory that does not fit, or a config or cache directory that the system
 * does not give.
 */
bool app_identity_init(struct app_identity *out, const char *app_id);

/*
 * Builds the path "<config_dir>/<rel>" in `out`. Returns false when the
 * identity has no config directory or the path does not fit.
 */
bool app_identity_config_path(const struct app_identity *identity, char *out,
                              size_t sz, const char *rel);

/*
 * Builds the path "<cache_dir>/<rel>" in `out`. Returns false when the
 * identity has no cache directory or the path does not fit.
 */
bool app_identity_cache_path(const struct app_identity *identity, char *out,
                             size_t sz, const char *rel);

/*
 * Builds the path "<data_dir>/<rel>" in `out`. Returns false when the
 * identity has no data directory or the path does not fit.
 */
bool app_identity_data_path(const struct app_identity *identity, char *out,
                            size_t sz, const char *rel);

/*
 * Stores the identity of this process. The application calls this at startup,
 * before a module reads the identity. It borrows `identity` and does not copy
 * it. A later call replaces the pointer, so the last identity set must stay
 * valid for the life of the process.
 */
void app_identity_set_current(const struct app_identity *identity);

/*
 * Returns the identity of this process. It is NULL before the application
 * sets it.
 */
const struct app_identity *app_identity_current(void);

#endif
