#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/config.h"
#include "util/str.h"

bool config_parse_file(const char *path, config_kv_fn fn, void *ctx)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return false;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) {
        int err = errno;
        close(fd);
        errno = err;
        return false;
    }
    char buf[1024];
    int line = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line++;
        if (!strchr(buf, '\n') && !feof(f)) {
            bool overlong = false;
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n')
                overlong = true;
            if (overlong)
                continue;
        }
        char *p = str_trim(buf);
        if (!*p || *p == '#')
            continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            fn(ctx, path, line, p, NULL);
            continue;
        }
        *eq = 0;
        fn(ctx, path, line, str_trim(p), str_trim(eq + 1));
    }
    fclose(f);
    return true;
}
