#define _GNU_SOURCE

#include "util/fs.h"

#include <errno.h>
#include <string.h>

#include "util/path.h"

FILE *fs_create(const char *tag, const char *path)
{
    if (!mkdir_parents(path)) {
        fprintf(stderr, "[%s] cannot create %s: %s\n", tag, path,
                strerror(errno));
        return NULL;
    }
    FILE *f = fopen(path, "w");
    if (!f)
        fprintf(stderr, "[%s] cannot create %s: %s\n", tag, path,
                strerror(errno));
    return f;
}
