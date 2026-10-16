#ifndef UTIL_PATH_H
#define UTIL_PATH_H

#include <stdbool.h>
#include <stddef.h>

bool xdg_config_path(char *out, size_t sz, const char *file);

bool mkdir_parents(const char *path);

#endif
