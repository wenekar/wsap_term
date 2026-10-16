#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "pty/internal.h"
#include "platform/platform.h"
#include "util/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static void route_fds(int slave_fd, int out_fd)
{
    int in = fcntl(slave_fd, F_DUPFD, 3);
    int out = fcntl(out_fd, F_DUPFD, 3);
    if (in < 0 || out < 0 || dup2(in, 0) < 0 || dup2(out, 1) < 0 ||
        dup2(out, 2) < 0)
        _exit(126);
    platform_close_fds_from(3);
}

void pty_exec_child(int master_fd, int slave_fd, const int pipe_fds[2],
                    const struct pty_plan *plan)
{
    proc_child_unblock_signals();
    close(master_fd);
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);

    if (setsid() < 0)
        _exit(126);
    ioctl(slave_fd, TIOCSCTTY, 0);
    tcsetpgrp(slave_fd, getpgrp());

    route_fds(slave_fd, pipe_fds[1] >= 0 ? pipe_fds[1] : slave_fd);

    if ((!plan->cwd || chdir(plan->cwd) != 0) && plan->cwd_fallback)
        (void)!chdir(plan->cwd_fallback);

    char *const *envp = plan->env.vars;
    execve(plan->path, plan->argv, envp);
    if (errno == ENOEXEC)
        execve("/bin/sh", plan->sh_argv, envp);
    execve("/bin/sh", (char *const[]){ "sh", NULL }, envp);
    _exit(127);
}
