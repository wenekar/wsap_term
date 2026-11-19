#define _GNU_SOURCE

#include "util/file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *util_read_file(const char *path, const char *what)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[%s] cannot open %s: %s\n",
                what, path, strerror(errno));
        return NULL;
    }
    struct stat st;
    FILE *f = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        f = fdopen(fd, "r");
    if (!f) {
        int err = errno;
        close(fd);
        errno = err ? err : EINVAL;
        fprintf(stderr, "[%s] cannot open %s: %s\n",
                what, path, strerror(errno));
        return NULL;
    }
    long sz = st.st_size;
    char *buf = NULL;
    if (sz > 0 && sz < 1024 * 1024 && (buf = malloc((size_t)sz + 1))) {
        size_t rd = fread(buf, 1, (size_t)sz, f);
        buf[rd] = '\0';
    }
    fclose(f);
    return buf;
}
