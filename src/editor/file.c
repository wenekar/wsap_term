#define _GNU_SOURCE

#include "editor/file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define EDITOR_PATH_MAX 4096

uint64_t editor_file_hash(const char *bytes, size_t length)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char)bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static int64_t stat_mtime(const struct stat *st)
{
#ifdef __APPLE__
    struct timespec mtime = st->st_mtimespec;
#else
    struct timespec mtime = st->st_mtim;
#endif
    return (int64_t)mtime.tv_sec * 1000000000 + mtime.tv_nsec;
}

bool editor_file_state_get(const char *path, struct editor_file_state *state)
{
    if (!path || !state)
        return false;
    struct stat st;
    if (stat(path, &st) < 0)
        return false;
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return false;
    }
    *state = (struct editor_file_state){
        .present = true,
        .device = (uint64_t)st.st_dev,
        .inode = (uint64_t)st.st_ino,
        .size = (uint64_t)st.st_size,
        .mtime_ns = stat_mtime(&st),
    };
    return true;
}

bool editor_file_state_equal(const struct editor_file_state *a,
                             const struct editor_file_state *b)
{
    if (!a || !b)
        return false;
    if (a->present != b->present)
        return false;
    if (!a->present)
        return true;
    return a->device == b->device && a->inode == b->inode && a->size == b->size &&
           a->mtime_ns == b->mtime_ns;
}

enum editor_file_result editor_file_check(const struct editor_file_state *state,
                                          const char *path)
{
    if (!state || !path)
        return EDITOR_FILE_MISSING;
    struct editor_file_state current;
    if (!editor_file_state_get(path, &current)) {
        if (state->present)
            return EDITOR_FILE_MISSING;
        return EDITOR_FILE_UNCHANGED;
    }
    return editor_file_state_equal(state, &current) ? EDITOR_FILE_UNCHANGED
                                                    : EDITOR_FILE_CHANGED;
}

char *editor_file_read(const char *path, size_t *length)
{
    if (!path)
        return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        int error = errno ? errno : EINVAL;
        close(fd);
        errno = error;
        return NULL;
    }
    uint64_t size = (uint64_t)st.st_size;
    if (size >= SIZE_MAX) {
        close(fd);
        errno = EFBIG;
        return NULL;
    }
    char *bytes = malloc((size_t)size + 1);
    if (!bytes) {
        close(fd);
        return NULL;
    }
    size_t done = 0;
    while (done < (size_t)size) {
        ssize_t got = read(fd, bytes + done, (size_t)size - done);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            int error = errno;
            free(bytes);
            close(fd);
            errno = error;
            return NULL;
        }
        if (got == 0)
            break;
        done += (size_t)got;
    }
    close(fd);
    bytes[done] = '\0';
    if (length)
        *length = done;
    return bytes;
}

static bool write_all(int fd, const char *bytes, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t wrote = write(fd, bytes + done, length - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += (size_t)wrote;
    }
    return true;
}

/* Copies the name of the directory that holds `path` into `out`. */
static bool directory_of(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, size, ".");
        return true;
    }
    size_t length = (size_t)(slash - path);
    if (length == 0)
        length = 1;
    if (length >= size)
        return false;
    memcpy(out, path, length);
    out[length] = '\0';
    return true;
}

/* Flushes the directory entry of a rename to disk. A file system that cannot
 * flush a directory keeps the rename in its cache. */
static void sync_directory(const char *path)
{
    char directory[EDITOR_PATH_MAX];
    if (!directory_of(path, directory, sizeof(directory)))
        return;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    fsync(fd);
    close(fd);
}

static mode_t target_mode(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return st.st_mode & 07777;
    mode_t mask = umask(0);
    umask(mask);
    return (mode_t)(0666 & ~mask);
}

bool editor_file_save(const char *path, const char *bytes, size_t length,
                      struct editor_file_state *state)
{
    if (!path || !bytes)
        return false;
    char temp[EDITOR_PATH_MAX];
    int used = snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", path);
    if (used < 0 || (size_t)used >= sizeof(temp)) {
        errno = ENAMETOOLONG;
        return false;
    }
    int fd = mkstemp(temp);
    if (fd < 0)
        return false;
    if (!write_all(fd, bytes, length) || fchmod(fd, target_mode(path)) < 0 ||
        fsync(fd) < 0) {
        int error = errno ? errno : EIO;
        close(fd);
        unlink(temp);
        errno = error;
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int error = errno;
        close(fd);
        unlink(temp);
        errno = error;
        return false;
    }
    if (close(fd) < 0) {
        int error = errno;
        unlink(temp);
        errno = error;
        return false;
    }
    if (rename(temp, path) < 0) {
        int error = errno;
        unlink(temp);
        errno = error;
        return false;
    }
    sync_directory(path);
    if (state) {
        *state = (struct editor_file_state){
            .present = true,
            .device = (uint64_t)st.st_dev,
            .inode = (uint64_t)st.st_ino,
            .size = (uint64_t)st.st_size,
            .mtime_ns = stat_mtime(&st),
        };
    }
    return true;
}
