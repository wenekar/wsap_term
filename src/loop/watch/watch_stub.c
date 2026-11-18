#include "loop/watch/watch_internal.h"

#include <errno.h>

/* The FSEvents (macOS) and kqueue (BSD) backends go here. Both deliver the
 * same event bits through fs_watch_report and fs_watch_flush. */

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
