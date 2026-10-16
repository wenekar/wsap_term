#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/path.h"
#include "util/proc.h"

bool xdg_config_path(char *out, size_t sz, const char *file)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *suffix = "";
    if (!base || base[0] != '/') {
        base = proc_home_dir();
        if (!base || !base[0])
            return false;
        suffix = "/.config";
    }
    int n = snprintf(out, sz, "%s%s/%s", base, suffix, file);
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
