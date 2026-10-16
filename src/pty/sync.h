#ifndef PTY_SYNC_H
#define PTY_SYNC_H

#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#define PTY_SYNC_ENV "WSAP_SYNC"
#define PTY_SYNC_LEAVE_PIPE 'p'
#define PTY_SYNC_LEAVE_TTY 't'

#if defined(MSG_NOSIGNAL)
#define PTY_SYNC_SEND_FLAGS MSG_NOSIGNAL
#else
#define PTY_SYNC_SEND_FLAGS 0
#endif

static inline socklen_t pty_sync_addr(struct sockaddr_un *addr,
                                      const char *name)
{
    size_t n = strlen(name);
    if (!n || n + 1 > sizeof(addr->sun_path))
        return 0;
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
#if defined(__linux__)
    memcpy(addr->sun_path + 1, name, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
#else
    memcpy(addr->sun_path, name, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
#endif
}

static inline int pty_sync_socket(void)
{
#if defined(__linux__)
    return socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
#else
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

#endif
