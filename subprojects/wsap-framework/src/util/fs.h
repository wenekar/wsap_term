#ifndef UTIL_FS_H
#define UTIL_FS_H

#include <stdio.h>

/* Creates `path` and its parent directories. `tag` is the log prefix. */
FILE *fs_create(const char *tag, const char *path);

#endif
