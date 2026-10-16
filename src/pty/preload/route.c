#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "pty/preload/shim.h"
#include "pty/sync.h"

/* Raw-mode routing. A program that clears ICANON on the terminal is
 * interactive and needs neither the pipe's throughput nor its lack of
 * output processing, so its stdout and stderr move to the terminal. They
 * move back when ICANON returns. Each move first passes the barrier in
 * pty/sync.h, so output written before the move stays ahead of output
 * written after it. When the barrier fails, the output stays where it is:
 * a lost switch costs speed or line endings, never order. */

#define FP_SYNC_TIMEOUT_MS 200

static int routed_fd[2] = { -1, -1 };
static int routed;
static dev_t tty_rdev;
static int tty_rdev_known;

static int is_terminal_dev(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
        return 0;
    if (!tty_rdev_known) {
        const char *path = getenv("WSAP_PTY_PATH");
        struct stat pst;
        if (!path || !path[0] || stat(path, &pst) != 0)
            return 0;
        tty_rdev = pst.st_rdev;
        tty_rdev_known = 1;
    }
    return st.st_rdev == tty_rdev;
}

static int sync_barrier(char req)
{
    const char *name = getenv(PTY_SYNC_ENV);
    struct sockaddr_un addr;
    socklen_t len = name ? pty_sync_addr(&addr, name) : 0;
    int fd = len ? pty_sync_socket() : -1;
    if (fd < 0)
        return -1;
    char ack = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ok = connect(fd, (const struct sockaddr *)&addr, len) == 0 &&
             send(fd, &req, 1, PTY_SYNC_SEND_FLAGS) == 1 &&
             poll(&pfd, 1, FP_SYNC_TIMEOUT_MS) == 1 &&
             recv(fd, &ack, 1, 0) == 1 && ack == req;
    close(fd);
    return ok ? 0 : -1;
}

void fp_stdio_restore(int saved[2])
{
    int error = errno;
    for (int i = 0; i < 2; i++) {
        if (saved[i] < 0)
            continue;
        dup2(saved[i], i + STDOUT_FILENO);
        close(saved[i]);
        saved[i] = -1;
    }
    errno = error;
}

int fp_stdio_to_tty(int saved[2], int sync)
{
    saved[0] = saved[1] = -1;
    const int on_pipe[2] = { fp_is_fast_pipe(STDOUT_FILENO),
                             fp_is_fast_pipe(STDERR_FILENO) };
    if (!on_pipe[0] && !on_pipe[1])
        return 0;
    int tty = fp_tty_fd();
    if (tty < 0) {
        errno = ENOTTY;
        return -1;
    }
    if (sync && sync_barrier(PTY_SYNC_LEAVE_PIPE) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        int fd = i + STDOUT_FILENO;
        if (on_pipe[i] && ((saved[i] = fcntl(fd, F_DUPFD_CLOEXEC, 10)) < 0 ||
                           dup2(tty, fd) < 0)) {
            fp_stdio_restore(saved);
            return -1;
        }
    }
    return 1;
}

/* Only a descriptor still on the terminal goes back. If the program
 * redirected it meanwhile, its copy of the pipe is dropped. */
static void route_to_pipe(void)
{
    for (int i = 0; i < 2; i++) {
        if (routed_fd[i] >= 0 && !is_terminal_dev(i + STDOUT_FILENO)) {
            close(routed_fd[i]);
            routed_fd[i] = -1;
        }
    }
    if ((routed_fd[0] >= 0 || routed_fd[1] >= 0) &&
        sync_barrier(PTY_SYNC_LEAVE_TTY) < 0)
        return;
    fp_stdio_restore(routed_fd);
    routed = 0;
}

void fp_termios_changed(int fd, const struct termios *t)
{
    if (!t)
        return;
    int raw = !(t->c_lflag & ICANON);
    if (raw == routed)
        return;
    const char *opt = getenv("WSAP_TTY_ROUTE");
    if (opt && !strcmp(opt, "0"))
        return;
    int error = errno;
    if (fp_is_fast_pipe(fd) || is_terminal_dev(fd)) {
        if (!raw)
            route_to_pipe();
        else if (fp_stdio_to_tty(routed_fd, 1) > 0)
            routed = 1;
    }
    errno = error;
}
