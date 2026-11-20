#include "loop/watch/watch_internal.h"

#include <errno.h>

/* The backend for a platform without a file watcher: every watch fails with
 * ENOTSUP. An FSEvents (macOS) or kqueue (BSD) backend replaces this file and
 * reports through fs_watch_report and fs_watch_flush. */

int fs_watch_platform_open(struct fs_watch *watch)
{
    (void)watch;
    errno = ENOTSUP;
    return -1;
}

int fs_watch_platform_add(struct fs_watch *watch, const char *path, bool directory)
{
    (void)watch;
    (void)path;
    (void)directory;
    errno = ENOTSUP;
    return -1;
}

void fs_watch_platform_close(struct fs_watch *watch)
{
    (void)watch;
}
