#define _GNU_SOURCE

#include "loop/watch/watch_internal.h"

#include "yasl/yasl.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

struct inotify_backend {
    int fd;
    int watch_id;
    int wd;
    char *path;
};

static unsigned event_bits(uint32_t mask)
{
    unsigned events = 0;
    if (mask & IN_CREATE)
        events |= FS_WATCH_CREATED;
    if (mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB))
        events |= FS_WATCH_MODIFIED;
    if (mask & (IN_DELETE | IN_DELETE_SELF))
        events |= FS_WATCH_DELETED;
    if (mask & (IN_MOVED_FROM | IN_MOVE_SELF))
        events |= FS_WATCH_MOVED_FROM;
    if (mask & IN_MOVED_TO)
        events |= FS_WATCH_MOVED_TO;
    return events;
}

/* Reads all queued events, then delivers the coalesced changes. The flush can
 * free the watch, so nothing below it touches the watch. */
static void on_ready(void *userdata, int fd, short revents)
{
    struct fs_watch *watch = userdata;
    struct inotify_backend *backend = watch->platform;
    (void)revents;
    char buffer[65536];
    char path[PATH_MAX];
    for (;;) {
        ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length <= 0)
            break;
        for (char *cursor = buffer; cursor < buffer + length;) {
            const struct inotify_event *event = (const struct inotify_event *)cursor;
            cursor += sizeof(*event) + event->len;
            const char *name = event->len ? event->name : "";
            int written = name[0] ? snprintf(path, sizeof(path), "%s/%s", backend->path, name)
                                  : snprintf(path, sizeof(path), "%s", backend->path);
            if (written < 0 || (size_t)written >= sizeof(path))
                continue;
            fs_watch_report(watch, path, event_bits(event->mask));
        }
    }
    fs_watch_flush(watch);
}

int fs_watch_platform_open(struct fs_watch *watch)
{
    struct inotify_backend *backend = calloc(1, sizeof(*backend));
    if (!backend)
        return -1;
    backend->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (backend->fd < 0) {
        free(backend);
        return -1;
    }
    backend->watch_id = yasl_loop_add_fd(watch->loop, backend->fd, POLLIN, on_ready, watch);
    if (backend->watch_id <= 0) {
        int error = errno;
        close(backend->fd);
        free(backend);
        errno = error;
        return -1;
    }
    watch->platform = backend;
    return 0;
}

int fs_watch_platform_add(struct fs_watch *watch, const char *path, bool directory)
{
    struct inotify_backend *backend = watch->platform;
    uint32_t mask = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF |
                    IN_MOVED_FROM | IN_MOVED_TO;
    if (directory)
        mask |= IN_CREATE | IN_DELETE;
    backend->wd = inotify_add_watch(backend->fd, path, mask);
    if (backend->wd < 0)
        return -1;
    backend->path = strdup(path);
    if (!backend->path) {
        int error = errno;
        inotify_rm_watch(backend->fd, backend->wd);
        backend->wd = -1;
        errno = error;
        return -1;
    }
    return 0;
}

void fs_watch_platform_close(struct fs_watch *watch)
{
    struct inotify_backend *backend = watch->platform;
    if (!backend)
        return;
    yasl_loop_del_fd(watch->loop, backend->watch_id);
    if (backend->wd >= 0)
        inotify_rm_watch(backend->fd, backend->wd);
    close(backend->fd);
    free(backend->path);
    free(backend);
    watch->platform = NULL;
}
