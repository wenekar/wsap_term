#ifndef UTIL_CONFIG_H
#define UTIL_CONFIG_H

#include <stdbool.h>

typedef void (*config_kv_fn)(void *ctx, const char *path, int line,
                             const char *key, const char *val);

bool config_parse_file(const char *path, config_kv_fn fn, void *ctx);

#endif
