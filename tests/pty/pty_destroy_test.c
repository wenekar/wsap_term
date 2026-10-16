#define _GNU_SOURCE
#include "pty/pty.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DESTROY_TIMEOUT_SEC 10

static void test_zero_lifecycle(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int master, slave;
        struct winsize size = { .ws_row = 24, .ws_col = 80 };
        assert(openpty(&master, &slave, NULL, NULL, &size) == 0);
        assert(dup2(slave, STDIN_FILENO) == STDIN_FILENO);
        struct pty p = {0};
        pty_destroy(&p);
        pty_destroy(&p);
        assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);
        assert(pty_master_fd(&p) == -1 && pty_pipe_fd(&p) == -1);
        pty_resize(&p, (struct vector2i){ .x = 120, .y = 40 });
        assert(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0);
        assert(size.ws_row == 24 && size.ws_col == 80);
        close(master);
        close(slave);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    test_zero_lifecycle();
    int master_fd = -1, slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
        perror("openpty");
        return 1;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(master_fd);
        kill(getpid(), SIGSTOP);
        setsid();
        ioctl(slave_fd, TIOCSCTTY, 0);
        for (;;)
            pause();
        _exit(0);
    }

    close(slave_fd);

    struct pty p = {
        .master_handle = (unsigned)master_fd + 1,
        .pid = pid,
    };

    for (int i = 0; i < 100; i++) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (r == pid && WIFSTOPPED(status))
            break;
        usleep(10000);
    }

    alarm(DESTROY_TIMEOUT_SEC);

    pid_t waker = fork();
    assert(waker >= 0);
    if (waker == 0) {
        usleep(200000);
        kill(pid, SIGCONT);
        _exit(0);
    }

    pty_destroy(&p);

    alarm(0);
    kill(waker, SIGKILL);
    waitpid(waker, NULL, 0);

    assert(p.pid == 0);
    assert(pty_master_fd(&p) == -1);

    /* The child is reaped in the background; kill(pid, 0) succeeds on
     * a zombie, so ESRCH means it was reaped, not only that it exited. */
    bool reaped = false;
    for (int i = 0; i < 500 && !reaped; i++) {
        reaped = kill(pid, 0) < 0 && errno == ESRCH;
        if (!reaped)
            usleep(10000);
    }
    assert(reaped);

    puts("PASS: pty_destroy reaps child stopped before setsid");
    return 0;
}
