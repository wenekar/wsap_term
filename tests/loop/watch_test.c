#define _GNU_SOURCE

#include "loop/watch/watch.h"

#include "yasl/yasl.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct capture {
    int count;
    char last[PATH_MAX];
    unsigned bits;
    bool remove_on_first;
    struct fs_watch *watch;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void on_change(void *userdata, const char *path, unsigned events)
{
    struct capture *capture = userdata;
    capture->count++;
    snprintf(capture->last, sizeof(capture->last), "%s", path);
    capture->bits = events;
    if (capture->remove_on_first && capture->count == 1)
        fs_watch_remove(capture->watch);
}

static bool step_until(struct capture *capture, yasl_loop *loop, int target, int timeout_ms)
{
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (capture->count < target && now_ms() < deadline)
        yasl_loop_step(loop, 5);
    return capture->count >= target;
}

int main(void)
{
    char directory[] = "/tmp/wsap-watch-XXXXXX";
    assert(mkdtemp(directory));
    char file[PATH_MAX];
    assert(snprintf(file, sizeof(file), "%s/file.txt", directory) > 0);
    int fd = open(file, O_CREAT | O_WRONLY | O_APPEND, 0600);
    assert(fd >= 0);

    yasl_loop *loop = yasl_loop_create();
    assert(loop);

    struct capture file_events = {0};
    struct fs_watch *watch = fs_watch_add(loop, file, on_change, &file_events);
    assert(watch);
    assert(write(fd, "a", 1) == 1);
    assert(step_until(&file_events, loop, 1, 5000));
    assert(file_events.count == 1);
    assert(file_events.bits & FS_WATCH_MODIFIED);
    assert(strcmp(file_events.last, file) == 0);

    /* A burst of writes that waits in the kernel coalesces into one event. */
    int before = file_events.count;
    for (int i = 0; i < 32; i++)
        assert(write(fd, "x", 1) == 1);
    assert(step_until(&file_events, loop, before + 1, 5000));
    assert(file_events.count == before + 1);
    assert(file_events.bits & FS_WATCH_MODIFIED);
    for (int i = 0; i < 10; i++)
        yasl_loop_step(loop, 5);
    assert(file_events.count == before + 1);

    fs_watch_remove(watch);
    assert(write(fd, "y", 1) == 1);
    for (int i = 0; i < 10; i++)
        yasl_loop_step(loop, 5);
    assert(file_events.count == before + 1);

    char missing[PATH_MAX];
    assert(snprintf(missing, sizeof(missing), "%s/missing.txt", directory) > 0);
    errno = 0;
    assert(fs_watch_add(loop, missing, on_change, &file_events) == NULL);
    assert(errno == ENOENT);

    struct capture directory_events = {0};
    struct fs_watch *directory_watch = fs_watch_add(loop, directory, on_change, &directory_events);
    assert(directory_watch);
    char created[PATH_MAX];
    assert(snprintf(created, sizeof(created), "%s/new.txt", directory) > 0);
    int created_fd = open(created, O_CREAT | O_WRONLY, 0600);
    assert(created_fd >= 0);
    close(created_fd);
    assert(step_until(&directory_events, loop, 1, 5000));
    assert(directory_events.bits & FS_WATCH_CREATED);
    assert(strcmp(directory_events.last, created) == 0);
    fs_watch_remove(directory_watch);

    /* A callback can remove its own watch. The watch is freed, so a later
     * change must not reach it. */
    struct capture one_shot = {.remove_on_first = true};
    one_shot.watch = fs_watch_add(loop, file, on_change, &one_shot);
    assert(one_shot.watch);
    assert(write(fd, "z", 1) == 1);
    assert(step_until(&one_shot, loop, 1, 5000));
    assert(write(fd, "w", 1) == 1);
    for (int i = 0; i < 20; i++)
        yasl_loop_step(loop, 5);
    assert(one_shot.count == 1);

    close(fd);
    yasl_loop_destroy(loop);
    unlink(created);
    unlink(file);
    rmdir(directory);
    puts("watch-test: ok");
    return 0;
}
