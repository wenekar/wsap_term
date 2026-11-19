#ifndef UTIL_PATH_H
#define UTIL_PATH_H

#include <stdbool.h>
#include <stddef.h>

/* The XDG base directories of this user, without a trailing file name. */
bool xdg_config_home(char *out, size_t sz);
bool xdg_cache_home(char *out, size_t sz);

bool xdg_config_path(char *out, size_t sz, const char *file);

bool mkdir_parents(const char *path);

#endif
