#ifndef UTIL_PATH_H
#define UTIL_PATH_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The XDG config and cache directories of this user, without a trailing file
 * name. A variable that is unset or not an absolute path falls back to the home
 * directory. Returns false when the directory is not available or does not fit
 * `out`.
 */
bool xdg_config_home(char *out, size_t sz);
bool xdg_cache_home(char *out, size_t sz);

bool xdg_config_path(char *out, size_t sz, const char *file);

bool mkdir_parents(const char *path);

#endif
