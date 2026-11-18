#define _GNU_SOURCE

#include "util/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

void proc_child_unblock_signals(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
}

void proc_spawn_detached(char *const argv[])
{
    pid_t child = fork();
    if (child == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            proc_child_unblock_signals();
            setsid();
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(grandchild < 0 ? 127 : 0);
    }
    if (child > 0)
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

int proc_nonblocking_pipe(int fds[2])
{
    if (pipe(fds) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
    }
    return 0;
}

void proc_eventfd_close(int rd_fd, int wr_fd)
{
    if (rd_fd >= 0)
        close(rd_fd);
    if (wr_fd >= 0 && wr_fd != rd_fd)
        close(wr_fd);
}

const char *proc_home_dir(void)
{
    const char *home = getenv("HOME");
    if (home && home[0])
        return home;
    const struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : NULL;
}

