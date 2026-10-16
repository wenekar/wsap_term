#ifndef UTIL_FS_H
#define UTIL_FS_H

#include <stdio.h>

/* Creates a file for writing. Makes its parent directories first. Prints
 * an error on stderr and returns NULL on failure. */
FILE *fs_create(const char *path);

#endif
