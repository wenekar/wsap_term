#define _GNU_SOURCE

#include "util/proc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Reads until the pipe reports the end of the stream. */
static size_t read_all(int fd, char *out, size_t capacity)
{
    size_t used = 0;
    for (;;) {
        struct pollfd polled = {.fd = fd, .events = POLLIN};
        int ready = poll(&polled, 1, 5000);
        assert(ready > 0);
        ssize_t length = read(fd, out + used, capacity - used - 1);
        if (length < 0) {
            assert(errno == EAGAIN || errno == EINTR);
            continue;
        }
        if (length == 0)
            break;
        used += (size_t)length;
        assert(used < capacity - 1);
    }
    out[used] = '\0';
    return used;
}

static void test_exit_code(void)
{
    char *argv[] = {"sh", "-c", "exit 7", NULL};
    struct proc_child child = {0};
    assert(proc_spawn(argv, &child) == 0);
    assert(child.pid > 0);
    int status = 0;
    assert(proc_wait(&child, &status) == 0);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 7);
    proc_child_close(&child);
}

static void test_killed(void)
{
    char *argv[] = {"sh", "-c", "kill -KILL $$", NULL};
    struct proc_child child = {0};
    assert(proc_spawn(argv, &child) == 0);
    int status = 0;
    assert(proc_wait(&child, &status) == 0);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGKILL);
    proc_child_close(&child);
}

static void test_pipe_close_early(void)
{
    char *argv[] = {"sh", "-c", "printf hello; printf oops >&2; exit 3", NULL};
    struct proc_child child = {0};
    assert(proc_spawn(argv, &child) == 0);
    char out[64];
    char err[64];
    assert(read_all(child.out_fd, out, sizeof(out)) == 5);
    assert(strcmp(out, "hello") == 0);
    assert(read_all(child.err_fd, err, sizeof(err)) == 4);
    assert(strcmp(err, "oops") == 0);
    int status = 0;
    assert(proc_wait(&child, &status) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    proc_child_close(&child);
}

static void test_bidirectional(void)
{
    char *argv[] = {"sh", "-c", "IFS= read -r line; printf '%s' \"$line\"", NULL};
    struct proc_child child = {0};
    assert(proc_spawn(argv, &child) == 0);
    assert(write(child.in_fd, "ping\n", 5) == 5);
    close(child.in_fd);
    child.in_fd = -1;
    char out[64];
    assert(read_all(child.out_fd, out, sizeof(out)) == 4);
    assert(strcmp(out, "ping") == 0);
    int status = 0;
    assert(proc_wait(&child, &status) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    proc_child_close(&child);
}

static void test_reap(void)
{
    char *argv[] = {"sh", "-c", "sleep 0.2", NULL};
    struct proc_child child = {0};
    assert(proc_spawn(argv, &child) == 0);
    int status = 0;
    assert(proc_reap(&child, &status) == 0);
    uint64_t deadline = now_ms() + 5000;
    int reaped = 0;
    while (!reaped && now_ms() < deadline) {
        reaped = proc_reap(&child, &status);
        assert(reaped >= 0);
        if (!reaped)
            usleep(1000);
    }
    assert(reaped == 1);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    proc_child_close(&child);

    struct proc_child none = {0};
    proc_child_close(&none);
    errno = 0;
    assert(proc_wait(&none, NULL) == -1);
    assert(errno == ECHILD);
}

int main(void)
{
    test_exit_code();
    test_killed();
    test_pipe_close_early();
    test_bidirectional();
    test_reap();
    puts("proc-test: ok");
    return 0;
}
