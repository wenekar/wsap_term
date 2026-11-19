#define _GNU_SOURCE

#include "util/app_identity.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "util/path.h"

static const struct app_identity *current;

/* Copies "<base>/<rel>" into `out`. */
static bool join(const char *base, const char *rel, char *out, size_t sz)
{
    if (!base || !base[0])
        return false;
    int n = snprintf(out, sz, "%s/%s", base, rel);
    return n > 0 && n < (int)sz;
}

/*
 * Fills `out` with the directory of the image of this process. Returns false
 * when the system does not give the path or it does not fit.
 */
static bool exe_dir(char *out, size_t sz)
{
    char path[APP_IDENTITY_PATH_MAX];
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    path[n] = '\0';
#elif defined(__APPLE__)
    uint32_t len = (uint32_t)sizeof(path);
    if (_NSGetExecutablePath(path, &len) != 0 || len > sizeof(path))
        return false;
#else
    (void)out;
    (void)sz;
    return false;
#endif
    char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return false;
    *slash = '\0';
    int w = snprintf(out, sz, "%s", path);
    return w > 0 && (size_t)w < sz;
}

/*
 * Fills `out` with "<prefix>/share/<app_id>", found from the directory of the
 * executable of this process. Returns false when that directory is not there,
 * for example in a development tree or in the test runner.
 */
static bool installed_data_dir(const char *app_id, char *out, size_t sz)
{
    char dir[APP_IDENTITY_PATH_MAX];
    if (!exe_dir(dir, sizeof(dir)))
        return false;
    char candidate[APP_IDENTITY_PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/../share/%s", dir, app_id);
    if (n <= 0 || (size_t)n >= sizeof(candidate))
        return false;
    char resolved[APP_IDENTITY_PATH_MAX];
    if (!realpath(candidate, resolved))
        return false;
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    n = snprintf(out, sz, "%s", resolved);
    return n > 0 && (size_t)n < sz;
}

bool app_identity_init(struct app_identity *out, const char *app_id)
{
    if (!out || !app_id || !app_id[0])
        return false;
    *out = (struct app_identity){0};
    if (strlen(app_id) >= sizeof(out->id))
        return false;
    snprintf(out->id, sizeof(out->id), "%s", app_id);
    if (!xdg_config_home(out->config_dir, sizeof(out->config_dir)) ||
        !xdg_cache_home(out->cache_dir, sizeof(out->cache_dir)))
        return false;
    size_t len = strlen(out->config_dir);
    if (snprintf(out->config_dir + len, sizeof(out->config_dir) - len, "/%s",
                 app_id) >= (int)(sizeof(out->config_dir) - len))
        return false;
    len = strlen(out->cache_dir);
    if (snprintf(out->cache_dir + len, sizeof(out->cache_dir) - len, "/%s",
                 app_id) >= (int)(sizeof(out->cache_dir) - len))
        return false;
    /* The installed data directory comes from the executable path, so a
     * relocated install finds its own themes and fonts. WSAP_DATADIR is the
     * prefix of this build and is only the fallback of a development tree. */
    if (!installed_data_dir(app_id, out->data_dir, sizeof(out->data_dir))) {
#ifdef WSAP_DATADIR
        if (!join(WSAP_DATADIR, app_id, out->data_dir, sizeof(out->data_dir)))
            return false;
#endif
    }
    return true;
}

bool app_identity_config_path(const struct app_identity *identity, char *out,
                              size_t sz, const char *rel)
{
    return identity && join(identity->config_dir, rel, out, sz);
}

bool app_identity_cache_path(const struct app_identity *identity, char *out,
                             size_t sz, const char *rel)
{
    return identity && join(identity->cache_dir, rel, out, sz);
}

bool app_identity_data_path(const struct app_identity *identity, char *out,
                            size_t sz, const char *rel)
{
    return identity && join(identity->data_dir, rel, out, sz);
}

void app_identity_set_current(const struct app_identity *identity)
{
    current = identity;
}

const struct app_identity *app_identity_current(void)
{
    return current;
}
