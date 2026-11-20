#ifndef LOOP_WATCH_H
#define LOOP_WATCH_H

struct yasl_loop;

/*
 * A watch of one file or one directory. The events of a path that are ready
 * in the same loop step coalesce into one callback with the union of the
 * bits below. A directory watch reports its entries by their full path.
 */
struct fs_watch;

enum fs_watch_event {
    FS_WATCH_CREATED = 1u << 0,
    FS_WATCH_MODIFIED = 1u << 1,
    FS_WATCH_DELETED = 1u << 2,
    FS_WATCH_MOVED_FROM = 1u << 3,
    FS_WATCH_MOVED_TO = 1u << 4,
};

/* Runs on the loop's thread. `path` is valid until the callback returns. */
typedef void (*fs_watch_fn)(void *context, const char *path, unsigned events);

/*
 * Watches `path`, a file or a directory, and delivers its changes on the
 * loop's thread. The watch is not recursive.
 * Returns NULL with errno on failure. ENOENT means that `path` is missing.
 * The watch belongs to the loop's thread, and it must be removed before the
 * loop is destroyed.
 */
struct fs_watch *fs_watch_add(struct yasl_loop *loop, const char *path, fs_watch_fn callback,
                              void *context);

/*
 * Stops the watch and frees it. You can call it from a callback, also for a
 * different watch. A NULL watch does nothing.
 */
void fs_watch_remove(struct fs_watch *watch);

#endif
