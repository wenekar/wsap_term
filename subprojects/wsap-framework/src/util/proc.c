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

static void close_pipe(int fds[2])
{
    for (int i = 0; i < 2; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* A child setup assumes that no pipe end is a standard descriptor, because
 * dup2(2) can then overwrite an end that it did not copy yet. */
static int raise_pipe_fds(int fds[2])
{
    for (int i = 0; i < 2; i++) {
        if (fds[i] > STDERR_FILENO)
            continue;
        int higher = fcntl(fds[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (higher < 0) {
            close_pipe(fds);
            return -1;
        }
        close(fds[i]);
        fds[i] = higher;
    }
    return 0;
}

static int spawn_pipe(int fds[2])
{
    if (pipe(fds) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK) < 0) {
            close_pipe(fds);
            return -1;
        }
    }
    return raise_pipe_fds(fds);
}

int proc_spawn(char *const argv[], struct proc_child *child)
{
    if (!child) {
        errno = EINVAL;
        return -1;
    }
    *child = (struct proc_child){0};
    if (!argv || !argv[0]) {
        errno = EINVAL;
        return -1;
    }

    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if (spawn_pipe(in) < 0)
        return -1;
    if (spawn_pipe(out) < 0) {
        close_pipe(in);
        return -1;
    }
    if (spawn_pipe(err) < 0) {
        close_pipe(in);
        close_pipe(out);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close_pipe(in);
        close_pipe(out);
        close_pipe(err);
        return -1;
    }
    if (pid == 0) {
        proc_child_unblock_signals();
        if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0 ||
            dup2(err[1], STDERR_FILENO) < 0)
            _exit(127);
        for (int i = 0; i < 2; i++) {
            close(in[i]);
            close(out[i]);
            close(err[i]);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    close(err[1]);
    child->pid = pid;
    child->in_fd = in[1];
    child->out_fd = out[0];
    child->err_fd = err[0];
    return 0;
}

int proc_wait(struct proc_child *child, int *status)
{
    if (!child || child->pid <= 0) {
        errno = ECHILD;
        return -1;
    }
    int st;
    pid_t pid;
    do {
        pid = waitpid(child->pid, &st, 0);
    } while (pid < 0 && errno == EINTR);
    if (pid < 0)
        return -1;
    if (status)
        *status = st;
    return 0;
}

int proc_reap(struct proc_child *child, int *status)
{
    if (!child || child->pid <= 0) {
        errno = ECHILD;
        return -1;
    }
    int st;
    pid_t pid;
    do {
        pid = waitpid(child->pid, &st, WNOHANG);
    } while (pid < 0 && errno == EINTR);
    if (pid < 0)
        return -1;
    if (pid == 0)
        return 0;
    if (status)
        *status = st;
    return 1;
}

void proc_child_close(struct proc_child *child)
{
    if (!child || child->pid <= 0)
        return;
    if (child->in_fd >= 0)
        close(child->in_fd);
    if (child->out_fd >= 0)
        close(child->out_fd);
    if (child->err_fd >= 0)
        close(child->err_fd);
    child->in_fd = -1;
    child->out_fd = -1;
    child->err_fd = -1;
}

const char *proc_home_dir(void)
{
    const char *home = getenv("HOME");
    if (home && home[0])
        return home;
    const struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : NULL;
}

