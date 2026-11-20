#define _GNU_SOURCE

#include "pty/internal.h"
#include "pty/sync.h"
#include "platform/platform.h"

#include <sys/socket.h>

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool pipe_enabled(void)
{
    const char *env = getenv("WSAP_FAST_PIPE");
    if (env && env[0] && !env[1] && (env[0] == '0' || env[0] == '1'))
        return env[0] == '1';
    return PTY_FASTPIPE_DEFAULT;
}

/* A pipe that the child and this process share with a large buffer. */
static int pty_fastpipe_create(int fds[2], int buf_size)
{
#if defined(__APPLE__)
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
        return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    return 0;
#else
    if (pipe2(fds, O_CLOEXEC) < 0)
        return -1;
    fcntl(fds[0], F_SETPIPE_SZ, buf_size);
    return 0;
#endif
}

int pty_pipe_open(int fds[2])
{
    fds[0] = fds[1] = -1;
    if (!pipe_enabled())
        return 0;
    return pty_fastpipe_create(fds, PTY_PIPE_SIZE);
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
