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
#ifdef __APPLE__
#include <libproc.h>
#endif

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

const char *proc_default_shell(void)
{
    const char *shell = getenv("SHELL");
    if (shell && shell[0])
        return shell;
    const struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && pw->pw_shell[0])
        return pw->pw_shell;
    return "/bin/sh";
}

bool proc_read_pid_cwd_buf(pid_t pid, char *out, size_t sz)
{
    if (pid <= 0 || !out || sz == 0)
        return false;
#ifdef __APPLE__
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) > 0 &&
        vpi.pvi_cdir.vip_path[0] &&
        access(vpi.pvi_cdir.vip_path, X_OK) == 0) {
        size_t len = strlen(vpi.pvi_cdir.vip_path);
        if (len >= sz)
            return false;
        memcpy(out, vpi.pvi_cdir.vip_path, len + 1);
        return true;
    }
#else
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/cwd", (int)pid);
    /* readlink truncates silently; a cut path may name a parent directory. */
    ssize_t len = readlink(proc_path, out, sz);
    if (len > 0 && (size_t)len < sz) {
        out[len] = '\0';
        if (access(out, X_OK) == 0)
            return true;
    }
#endif
    return false;
}
