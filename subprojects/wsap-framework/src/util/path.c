#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/app_identity.h"
#include "util/path.h"
#include "util/proc.h"

/* Builds "<base>/<suffix>" for an XDG base directory variable. */
static bool xdg_home(char *out, size_t sz, const char *var, const char *suffix)
{
    const char *base = getenv(var);
    const char *tail = "";
    if (!base || base[0] != '/') {
        base = proc_home_dir();
        if (!base || !base[0])
            return false;
        tail = suffix;
    }
    int n = snprintf(out, sz, "%s%s", base, tail);
    return n > 0 && n < (int)sz;
}

bool xdg_config_home(char *out, size_t sz)
{
    return xdg_home(out, sz, "XDG_CONFIG_HOME", "/.config");
}

bool xdg_cache_home(char *out, size_t sz)
{
    return xdg_home(out, sz, "XDG_CACHE_HOME", "/.cache");
}

bool xdg_config_path(char *out, size_t sz, const char *file)
{
    char base[APP_IDENTITY_PATH_MAX];
    if (!xdg_config_home(base, sizeof(base)))
        return false;
    int n = snprintf(out, sz, "%s/%s", base, file);
    return n > 0 && n < (int)sz;
}

static bool mkdir_step(const char *dir)
{
    return mkdir(dir, 0700) == 0 || errno == EEXIST;
}

bool mkdir_parents(const char *path)
{
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s", path);
    if (n < 0 || n >= (int)sizeof(dir))
        return false;
    char *slash = strrchr(dir, '/');
    if (!slash)
        return true;
    *slash = 0;
    if (!dir[0])
        return true;
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            bool ok = mkdir_step(dir);
            *p = '/';
            if (!ok)
                return false;
        }
    }
    return mkdir_step(dir);
}
