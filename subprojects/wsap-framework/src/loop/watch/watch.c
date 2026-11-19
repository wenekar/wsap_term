#define _GNU_SOURCE

#include "loop/watch/watch_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void free_changes(struct fs_watch *watch)
{
    for (size_t i = 0; i < watch->change_count; i++)
        free(watch->changes[i].path);
    watch->change_count = 0;
}

static void free_watch(struct fs_watch *watch)
{
    free_changes(watch);
    free(watch->changes);
    free(watch);
}

void fs_watch_report(struct fs_watch *watch, const char *path, unsigned events)
{
    if (!events)
        return;
    for (size_t i = 0; i < watch->change_count; i++) {
        if (strcmp(watch->changes[i].path, path) == 0) {
            watch->changes[i].events |= events;
            return;
        }
    }
    if (watch->change_count == watch->change_capacity) {
        size_t capacity = watch->change_capacity ? watch->change_capacity * 2 : 8;
        struct fs_watch_change *grown = realloc(watch->changes, capacity * sizeof(*grown));
        if (!grown)
            return;
        watch->changes = grown;
        watch->change_capacity = capacity;
    }
    char *copy = strdup(path);
    if (!copy)
        return;
    watch->changes[watch->change_count++] = (struct fs_watch_change){.path = copy, .events = events};
}

void fs_watch_flush(struct fs_watch *watch)
{
    watch->delivering = true;
    size_t i = 0;
    for (; i < watch->change_count; i++) {
        char *path = watch->changes[i].path;
        unsigned events = watch->changes[i].events;
        watch->changes[i] = (struct fs_watch_change){0};
        if (!watch->removed)
            watch->callback(watch->context, path, events);
        free(path);
        if (watch->removed)
            break;
    }
    for (; i < watch->change_count; i++)
        free(watch->changes[i].path);
    watch->change_count = 0;
    watch->delivering = false;
    if (watch->removed)
        free_watch(watch);
}

bool fs_watch_removed(const struct fs_watch *watch)
{
    return watch->removed;
}

struct fs_watch *fs_watch_add(struct yasl_loop *loop, const char *path, fs_watch_fn callback,
                              void *context)
{
    if (!loop || !path || !path[0] || !callback) {
        errno = EINVAL;
        return NULL;
    }
    struct stat info;
    if (stat(path, &info) < 0)
        return NULL;
    struct fs_watch *watch = calloc(1, sizeof(*watch));
    if (!watch)
        return NULL;
    watch->loop = loop;
    watch->callback = callback;
    watch->context = context;
    int error = 0;
    if (fs_watch_platform_open(watch) < 0)
        error = errno;
    else if (fs_watch_platform_add(watch, path, S_ISDIR(info.st_mode)) < 0)
        error = errno;
    if (error) {
        fs_watch_remove(watch);
        errno = error;
        return NULL;
    }
    return watch;
}

void fs_watch_remove(struct fs_watch *watch)
{
    if (!watch || watch->removed)
        return;
    watch->removed = true;
    fs_watch_platform_close(watch);
    if (!watch->delivering)
        free_watch(watch);
}
