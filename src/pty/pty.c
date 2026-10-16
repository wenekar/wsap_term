#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "pty/cmdline.h"
#include "pty/internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define PTY_HANGUP_GRACE_MS 2000
#define PTY_REAP_POLL_MS 10

static struct winsize winsize_of(struct vector2i grid)
{
    return (struct winsize){
        .ws_row = (unsigned short)grid.y,
        .ws_col = (unsigned short)grid.x,
    };
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int pty_spawn(struct pty *p, const struct pty_spec *spec)
{
    *p = (struct pty){0};

    struct pty_plan plan;
    if (pty_plan_init(&plan, spec) < 0) {
        pty_plan_free(&plan);
        return -1;
    }

    struct winsize ws = winsize_of(spec->grid);
    int master_fd = -1, slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        perror("openpty");
        pty_plan_free(&plan);
        return -1;
    }
    fcntl(master_fd, F_SETFD, FD_CLOEXEC);
    if (slave_fd > STDERR_FILENO)
        fcntl(slave_fd, F_SETFD, FD_CLOEXEC);

    int fast_pipe[2] = { -1, -1 };
    int sync_fd = -1;
    char sync_name[64] = { 0 };
    if (!pty_argv_needs_tty(plan.argv) && pty_pipe_open(fast_pipe) < 0)
        goto fail;
    if (fast_pipe[1] >= 0)
        sync_fd = pty_sync_listen(sync_name, sizeof(sync_name));
    if (pty_plan_env(&plan, spec, slave_fd, fast_pipe[1],
                     sync_fd >= 0 ? sync_name : NULL) < 0)
        goto fail;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        goto fail;
    }
    if (pid == 0)
        pty_exec_child(master_fd, slave_fd, fast_pipe, &plan);

    pty_plan_free(&plan);
    close(slave_fd);
    if (fast_pipe[1] >= 0)
        close(fast_pipe[1]);
    set_nonblock(master_fd);
    if (fast_pipe[0] >= 0)
        set_nonblock(fast_pipe[0]);

    p->master_handle = (unsigned)master_fd + 1;
    p->pipe_read_handle = (unsigned)(fast_pipe[0] + 1);
    p->sync_handle = (unsigned)(sync_fd + 1);
    p->pid = pid;
    return 0;

fail:
    if (sync_fd >= 0) close(sync_fd);
    if (fast_pipe[0] >= 0) close(fast_pipe[0]);
    if (fast_pipe[1] >= 0) close(fast_pipe[1]);
    close(master_fd);
    close(slave_fd);
    pty_plan_free(&plan);
    return -1;
}

static void signal_child(pid_t pid, int sig)
{
    kill(-pid, sig);
    kill(pid, sig);
}

static void kill_and_reap(pid_t pid)
{
    signal_child(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static void *reap_after_grace(void *arg)
{
    pid_t pid = (pid_t)(intptr_t)arg;
    for (int waited = 0; waited < PTY_HANGUP_GRACE_MS;
         waited += PTY_REAP_POLL_MS) {
        pid_t r = waitpid(pid, NULL, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR))
            return NULL;
        usleep(PTY_REAP_POLL_MS * 1000);
    }
    kill_and_reap(pid);
    return NULL;
}

void pty_destroy(struct pty *p)
{
    if (!p)
        return;
    int master = pty_master_fd(p);
    int pipe_fd = pty_pipe_fd(p);
    int sync_fd = pty_sync_fd(p);
    pid_t pid = p->pid;
    if (pid > 0) {
        signal_child(pid, SIGHUP);
        signal_child(pid, SIGCONT);
    }
    if (master >= 0) close(master);
    if (pipe_fd >= 0) close(pipe_fd);
    if (sync_fd >= 0) close(sync_fd);
    if (pid > 0 && waitpid(pid, NULL, WNOHANG) != pid) {
        pthread_t reaper;
        if (pthread_create(&reaper, NULL, reap_after_grace,
                           (void *)(intptr_t)pid) == 0)
            pthread_detach(reaper);
        else
            kill_and_reap(pid);
    }
    *p = (struct pty){0};
}

void pty_resize(struct pty *p, struct vector2i grid)
{
    int fd = pty_master_fd(p);
    if (fd < 0)
        return;
    struct winsize ws = winsize_of(grid);
    ioctl(fd, TIOCSWINSZ, &ws);
}
