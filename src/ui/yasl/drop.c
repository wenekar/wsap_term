#define _GNU_SOURCE

#include "ui/yasl/drop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DROP_IMAGE_MAX ((size_t)64 * 1024 * 1024)
/* The number of names to try when a name is already taken. */
#define DROP_NAME_TRIES 8

/* The paths that this process wrote. drop_cleanup removes them. */
static char **saved;
static size_t saved_count;
static size_t saved_cap;
/* The number that the next file name uses. */
static unsigned long next_name;

static const struct {
    const char *mime;
    const char *ext;
} extensions[] = {
    { "image/png", "png" },
    { "image/jpeg", "jpg" },
    { "image/webp", "webp" },
    { "image/gif", "gif" },
    { "image/bmp", "bmp" },
    { "image/tiff", "tiff" },
    { "image/svg+xml", "svg" },
};

/* Returns the file extension for `mime`, or "img" for a type that is not listed. */
static const char *extension_of(const char *mime)
{
    if (!mime)
        return "img";
    for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; i++)
        if (!strcasecmp(mime, extensions[i].mime))
            return extensions[i].ext;
    return "img";
}

/* Returns a new string that holds "a/b". Returns NULL when memory runs out. */
static char *path_join(const char *a, const char *b)
{
    size_t size = strlen(a) + strlen(b) + 2;
    char *out = malloc(size);
    if (out)
        snprintf(out, size, "%s/%s", a, b);
    return out;
}

/* Returns true when `path` is a real directory. Only the current user can use it. */
static bool private_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 077) == 0;
}

/* Creates `path` with mode 0700 when it is missing. Returns true when `path`
 * is then a private directory. */
static bool ensure_private_dir(const char *path)
{
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return false;
    return private_dir(path);
}

/* Returns true when `dir` is an absolute path to a directory. */
static bool runtime_usable(const char *dir)
{
    struct stat st;
    return dir && dir[0] == '/' && stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Returns the drops directory, created when it is missing. Returns NULL when
 * a directory on the way is not private. The caller frees the string. */
static char *drops_dir(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char *base = NULL;
    if (runtime_usable(runtime)) {
        base = path_join(runtime, "wsap_term");
    } else {
        char fallback[64];
        snprintf(fallback, sizeof fallback, "/tmp/wsap_term-%lu", (unsigned long)getuid());
        base = strdup(fallback);
    }
    char *drops = base ? path_join(base, "drops") : NULL;
    bool ok = drops && ensure_private_dir(base) && ensure_private_dir(drops);
    free(base);
    if (!ok) {
        free(drops);
        return NULL;
    }
    return drops;
}

/* Creates a new file in `dir` that ends in `ext`. Stores its path in *path.
 * Returns the open file descriptor, or -1 with errno set. */
static int create_file(const char *dir, const char *ext, char **path)
{
    size_t room = strlen(dir) + strlen(ext) + 64;
    for (unsigned i = 0; i < DROP_NAME_TRIES; i++) {
        char *name = malloc(room);
        if (!name)
            return -1;
        snprintf(name, room, "%s/drop-%ld-%lu.%s", dir, (long)getpid(), next_name++, ext);
        int fd = open(name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            *path = name;
            return fd;
        }
        int err = errno;
        free(name);
        if (err != EEXIST) {
            errno = err;
            return -1;
        }
    }
    errno = EEXIST;
    return -1;
}

/* Writes all `len` bytes to `fd`. Returns false on an error. */
static bool write_all(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* Adds `path` to the list that drop_cleanup removes. The list takes ownership
 * of `path` on success. Returns false when memory runs out. */
static bool remember(char *path)
{
    if (saved_count == saved_cap) {
        size_t cap = saved_cap ? saved_cap * 2 : 8;
        char **grown = realloc(saved, cap * sizeof *grown);
        if (!grown) {
            errno = ENOMEM;
            return false;
        }
        saved = grown;
        saved_cap = cap;
    }
    saved[saved_count++] = path;
    return true;
}

const char *drop_save_image(const char *mime, const char *data, size_t len)
{
    if (!data || !len || len > DROP_IMAGE_MAX) {
        fprintf(stderr, "drop: refused an image of %zu bytes\n", len);
        return NULL;
    }
    char *dir = drops_dir();
    if (!dir) {
        fprintf(stderr, "drop: no private directory for dropped files\n");
        return NULL;
    }
    char *path = NULL;
    int fd = create_file(dir, extension_of(mime), &path);
    free(dir);
    if (fd < 0) {
        fprintf(stderr, "drop: cannot create a file: %s\n", strerror(errno));
        return NULL;
    }
    bool ok = write_all(fd, data, len);
    if (close(fd) != 0)
        ok = false;
    if (ok && !remember(path))
        ok = false;
    if (!ok) {
        fprintf(stderr, "drop: cannot write %s: %s\n", path, strerror(errno));
        unlink(path);
        free(path);
        return NULL;
    }
    return path;
}

void drop_cleanup(void)
{
    for (size_t i = 0; i < saved_count; i++) {
        unlink(saved[i]);
        free(saved[i]);
    }
    free(saved);
    saved = NULL;
    saved_count = 0;
    saved_cap = 0;
}
