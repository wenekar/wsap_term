#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "pty/preload/shim.h"

#ifdef __APPLE__
#define FP_REAL(ret, name, params, args, fail) \
    static ret fp_real_##name params { return name args; }
#else
#define FP_REAL(ret, name, params, args, fail) \
    static ret fp_real_##name params \
    { \
        static ret (*fn) params; \
        if (!fn) \
            fn = (ret (*) params)dlsym(RTLD_NEXT, #name); \
        return fn ? fn args : (fail); \
    }
#endif

FP_REAL(int, isatty, (int fd), (fd), 0)
FP_REAL(int, tcgetattr, (int fd, struct termios *t), (fd, t), -1)
FP_REAL(int, tcsetattr, (int fd, int actions, const struct termios *t), (fd, actions, t), -1)
FP_REAL(int, ioctl, (int fd, unsigned long req, void *arg), (fd, req, arg), -1)
FP_REAL(int, tcdrain, (int fd), (fd), -1)
FP_REAL(int, tcflush, (int fd, int queue), (fd, queue), -1)
FP_REAL(int, tcflow, (int fd, int action), (fd, action), -1)
FP_REAL(int, tcsendbreak, (int fd, int duration), (fd, duration), -1)
FP_REAL(pid_t, tcgetpgrp, (int fd), (fd), (pid_t)-1)
FP_REAL(int, tcsetpgrp, (int fd, pid_t pgrp), (fd, pgrp), -1)
FP_REAL(char *, ttyname, (int fd), (fd), NULL)
FP_REAL(int, ttyname_r, (int fd, char *buf, size_t n), (fd, buf, n), ENOTTY)

#define FP_WRAP_FD(ret, name, params, args) \
    ret FP_SHIM(name) params \
    { \
        fd = fp_terminal_fd(fd); \
        if (fd < 0) return (ret)-1; \
        return fp_real_##name args; \
    }

static unsigned long long fast_pipe_dev, fast_pipe_ino;
static int fast_pipe_init = 0;

int fp_is_fast_pipe(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return 0;
    if (!S_ISFIFO(st.st_mode) && !S_ISSOCK(st.st_mode))
        return 0;

    if (!fast_pipe_init) {
        const char *env = getenv("WSAP_PIPE_ID");
        if (!env || sscanf(env, "%llu:%llu", &fast_pipe_dev, &fast_pipe_ino) != 2)
            fast_pipe_dev = fast_pipe_ino = 0;
        fast_pipe_init = 1;
    }

    return fast_pipe_ino && (unsigned long long)st.st_ino == fast_pipe_ino &&
           (unsigned long long)st.st_dev == fast_pipe_dev;
}

static int cached_fd = -1;
static dev_t cached_dev;
static ino_t cached_ino;

static int open_tty(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !fp_real_isatty(fd)) {
        close(fd);
        return -1;
    }
    cached_fd = fd;
    cached_dev = st.st_dev;
    cached_ino = st.st_ino;
    return fd;
}

int fp_tty_fd(void)
{
    if (cached_fd >= 0) {
        struct stat st;
        if (fstat(cached_fd, &st) == 0 && st.st_dev == cached_dev &&
            st.st_ino == cached_ino)
            return cached_fd;
        cached_fd = -1;
    }

    const char *path = getenv("WSAP_PTY_PATH");
    if (path && path[0] && open_tty(path) >= 0)
        return cached_fd;
    if (open_tty("/dev/tty") >= 0)
        return cached_fd;
    if (fp_real_isatty(0))
        return 0;
    return -1;
}

static int fp_terminal_fd(int fd)
{
    if (!fp_is_fast_pipe(fd))
        return fd;
    int tty = fp_tty_fd();
    if (tty < 0)
        errno = ENOTTY;
    return tty;
}

static int is_tty_ioctl(unsigned long request)
{
    switch (request) {
#ifdef TCGETS
    case TCGETS:
#endif
#ifdef TCSETS
    case TCSETS:
#endif
#ifdef TCSETSW
    case TCSETSW:
#endif
#ifdef TCSETSF
    case TCSETSF:
#endif
    case TIOCGWINSZ:
    case TIOCSWINSZ:
#ifdef TIOCOUTQ
    case TIOCOUTQ:
#endif
    case TIOCGPGRP:
    case TIOCSPGRP:
    case TIOCEXCL:
#ifdef TIOCSBRK
    case TIOCSBRK:
#endif
#ifdef TIOCCBRK
    case TIOCCBRK:
#endif
#ifdef TIOCGLCKTRMIOS
    case TIOCGLCKTRMIOS:
#endif
#ifdef TIOCSLCKTRMIOS
    case TIOCSLCKTRMIOS:
#endif
        return 1;
    default:
        return 0;
    }
}

int FP_SHIM(isatty)(int fd)
{
    if (fp_is_fast_pipe(fd))
        return 1;

    return fp_real_isatty(fd);
}

FP_WRAP_FD(int, tcgetattr, (int fd, struct termios *termios_p), (fd, termios_p))

int FP_SHIM(tcsetattr)(int fd, int optional_actions,
                      const struct termios *termios_p)
{
    int tty = fp_terminal_fd(fd);
    if (tty < 0)
        return -1;
    int rc = fp_real_tcsetattr(tty, optional_actions, termios_p);
    if (rc == 0)
        fp_termios_changed(fd, termios_p);
    return rc;
}

FP_WRAP_FD(int, tcdrain, (int fd), (fd))
FP_WRAP_FD(int, tcflush, (int fd, int queue_selector), (fd, queue_selector))
FP_WRAP_FD(int, tcflow, (int fd, int action), (fd, action))
FP_WRAP_FD(int, tcsendbreak, (int fd, int duration), (fd, duration))
FP_WRAP_FD(pid_t, tcgetpgrp, (int fd), (fd))
FP_WRAP_FD(int, tcsetpgrp, (int fd, pid_t pgrp), (fd, pgrp))

static int fp_pty_name_r(int pty, char *buf, size_t buflen)
{
#ifndef __APPLE__
    if (ptsname_r(pty, buf, buflen) == 0)
        return 0;
    return fp_real_ttyname_r(pty, buf, buflen);
#else
    const char *slave = ptsname(pty);
    if (!slave)
        return errno ? errno : ENOTTY;
    size_t len = strlen(slave) + 1;
    if (len > buflen)
        return ERANGE;
    memcpy(buf, slave, len);
    return 0;
#endif
}

static int fp_tty_fd_for_fast_pipe(int fd)
{
    if (!fp_is_fast_pipe(fd))
        return -2;
    return fp_tty_fd();
}

char *FP_SHIM(ttyname)(int fd)
{
    int pty = fp_tty_fd_for_fast_pipe(fd);
    if (pty == -2)
        return fp_real_ttyname(fd);
    if (pty < 0) {
        errno = ENOTTY;
        return NULL;
    }
    static char name[256];
    int error = fp_pty_name_r(pty, name, sizeof(name));
    if (error) {
        errno = error;
        return NULL;
    }
    return name;
}

int FP_SHIM(ttyname_r)(int fd, char *buf, size_t buflen)
{
    int pty = fp_tty_fd_for_fast_pipe(fd);
    if (pty == -2)
        return fp_real_ttyname_r(fd, buf, buflen);
    if (pty < 0)
        return ENOTTY;
    return fp_pty_name_r(pty, buf, buflen);
}

int FP_SHIM(ioctl)(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    int target = fd;
    if (is_tty_ioctl(request)) {
        target = fp_terminal_fd(fd);
        if (target < 0) return -1;
    }

    int rc = fp_real_ioctl(target, request, arg);
#if defined(TCSETS) && defined(TCSETSW) && defined(TCSETSF)
    if (rc == 0 && (request == TCSETS || request == TCSETSW ||
                    request == TCSETSF))
        fp_termios_changed(fd, arg);
#endif
    return rc;
}
