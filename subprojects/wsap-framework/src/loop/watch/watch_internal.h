#ifndef LOOP_WATCH_INTERNAL_H
#define LOOP_WATCH_INTERNAL_H

#include "loop/watch/watch.h"

#include <stdbool.h>
#include <stddef.h>

struct fs_watch_change {
    char *path;
    unsigned events;
};

struct fs_watch {
    struct yasl_loop *loop;
    fs_watch_fn callback;
    void *context;
    /* Backend state, private to the platform file. */
    void *platform;
    bool removed;
    bool delivering;
    struct fs_watch_change *changes;
    size_t change_count;
    size_t change_capacity;
};

/*
 * Records a raw change of `path` for the current loop step. The common layer
 * coalesces the changes until fs_watch_flush.
 */
void fs_watch_report(struct fs_watch *watch, const char *path, unsigned events);

/*
 * Delivers the coalesced changes on the loop's thread. A callback can remove
 * the watch. The flush then frees the watch, so the backend must not touch it
 * after this call.
 */
void fs_watch_flush(struct fs_watch *watch);

/* True once a callback removed the watch. */
bool fs_watch_removed(const struct fs_watch *watch);

/*
 * Backend hooks. A backend reports through fs_watch_report and calls
 * fs_watch_flush at the end of its loop callback. The inotify backend uses one
 * descriptor. An FSEvents or kqueue backend can use more descriptors or a
 * timer, because the common layer does not see them.
 */
int fs_watch_platform_open(struct fs_watch *watch);
int fs_watch_platform_add(struct fs_watch *watch, const char *path, bool directory);
void fs_watch_platform_close(struct fs_watch *watch);

#endif
