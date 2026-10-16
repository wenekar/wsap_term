#define _GNU_SOURCE

#include "pty/internal.h"
#include "pty/sync.h"
#include "platform/platform.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* WSAP_FAST_PIPE=1 forces the pipe on and WSAP_FAST_PIPE=0 forces it
 * off. Any other value keeps the platform default. */
static bool pipe_enabled(void)
{
    const char *env = getenv("WSAP_FAST_PIPE");
    if (env && env[0] && !env[1] && (env[0] == '0' || env[0] == '1'))
        return env[0] == '1';
    return PLATFORM_FASTPIPE_DEFAULT;
}

int pty_pipe_open(int fds[2])
{
    fds[0] = fds[1] = -1;
    if (!pipe_enabled())
        return 0;
    return platform_fastpipe_create(fds, PTY_PIPE_SIZE);
}

int pty_sync_listen(char *name, size_t n)
{
    static _Atomic unsigned seq;
#if defined(__linux__)
    snprintf(name, n, "wsap_term.sync.%ld.%u", (long)getpid(),
             atomic_fetch_add(&seq, 1));
#else
    snprintf(name, n, "/tmp/wsap_sync.%ld.%u", (long)getpid(),
             atomic_fetch_add(&seq, 1));
    unlink(name);
#endif
    struct sockaddr_un addr;
    socklen_t len = pty_sync_addr(&addr, name);
    int fd = len ? pty_sync_socket() : -1;
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0 ||
        bind(fd, (const struct sockaddr *)&addr, len) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
#if !defined(__linux__)
        unlink(name);
#endif
        return -1;
    }
    return fd;
}
