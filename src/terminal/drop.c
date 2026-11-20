#define _GNU_SOURCE

#include "util/text_limit.h"
#include "terminal/drop.h"

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

/* The largest image that a drop saves. */
#define DROP_IMAGE_MAX ((size_t)64 * 1024 * 1024)
/* The number of names to try when a name is already taken. */
#define DROP_NAME_TRIES 8
/* Room for a file name: "drop-", a pid, a counter, an extension, and NUL. */
#define DROP_NAME_MAX 80

/* The paths that terminal_save_dropped_image wrote. The exit handler removes
 * them. */
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

/* Returns true when `mime` names an image type with a subtype. */
bool terminal_mime_is_image(const char *mime)
{
    return mime && !strncasecmp(mime, "image/", 6) && mime[6];
}

/* Returns the file extension for `mime`, or "img" for a type that is not
 * listed. */
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

/* Opens the entry `name` of the open directory `parent` as a directory. It
 * creates the directory with mode 0700 when it is missing. Returns the new
 * descriptor. Returns -1 with errno set when the entry is not a directory
 * that only the current user can use. A symbolic link is refused. */
static int open_private_dir(int parent, const char *name)
{
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
        return -1;
    int fd = openat(parent, name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) != 0) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

/* Opens the drops directory and returns its descriptor. The directory lies
 * under the runtime directory, or under /tmp when the runtime directory is
 * unset or unusable. Each level is opened from the level above it, so no
 * step follows a path that a later step could swap. Stores the directory
 * path in *prefix. The caller frees the path. Returns -1 on failure. */
static int open_drops_dir(char **prefix)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *root = runtime;
    const char *level = "wsap_term";
    char fallback[64];
    int parent = runtime && runtime[0] == '/'
        ? open(runtime, O_DIRECTORY | O_CLOEXEC) : -1;
    if (parent < 0) {
        root = "/tmp";
        snprintf(fallback, sizeof fallback, "wsap_term-%lu",
                 (unsigned long)getuid());
        level = fallback;
        parent = open(root, O_DIRECTORY | O_CLOEXEC);
    }
    if (parent < 0)
        return -1;
    int base = open_private_dir(parent, level);
    close(parent);
    if (base < 0)
        return -1;
    int drops = open_private_dir(base, "drops");
    close(base);
    if (drops < 0)
        return -1;
    char *dir = path_join(root, level);
    *prefix = dir ? path_join(dir, "drops") : NULL;
    free(dir);
    if (!*prefix) {
        close(drops);
        return -1;
    }
    return drops;
}

/* Creates a new file in the open directory `dir`. Its name ends in `ext`,
 * and the name is written to `name`. Returns the open descriptor, or -1 with
 * errno set. */
static int create_file(int dir, const char *ext, char name[DROP_NAME_MAX])
{
    for (unsigned i = 0; i < DROP_NAME_TRIES; i++) {
        snprintf(name, DROP_NAME_MAX, "drop-%ld-%lu.%s", (long)getpid(),
                 next_name++, ext);
        int fd = openat(dir, name,
                        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                        0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
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

/* Adds a copy of `path` to the list that terminal_drop_cleanup removes.
 * Returns false when memory runs out. */
static bool remember(const char *path)
{
    char *copy = strdup(path);
    if (!copy)
        return false;
    if (saved_count == saved_cap) {
        size_t cap = saved_cap ? saved_cap * 2 : 8;
        char **grown = realloc(saved, cap * sizeof *grown);
        if (!grown) {
            free(copy);
            return false;
        }
        saved = grown;
        saved_cap = cap;
    }
    saved[saved_count++] = copy;
    return true;
}

static void terminal_drop_cleanup(void)
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

char *terminal_save_dropped_image(const char *mime, const char *data,
                                  size_t len)
{
    if (!data || !len || len > DROP_IMAGE_MAX)
        return NULL;
    static bool cleanup_registered;
    if (!cleanup_registered) {
        /* The paths belong to the shell, so they stay until the exit. */
        atexit(terminal_drop_cleanup);
        cleanup_registered = true;
    }
    char *prefix = NULL;
    int dir = open_drops_dir(&prefix);
    if (dir < 0)
        return NULL;
    char name[DROP_NAME_MAX];
    char *path = NULL;
    int fd = create_file(dir, extension_of(mime), name);
    if (fd >= 0) {
        bool ok = write_all(fd, data, len);
        if (close(fd) != 0)
            ok = false;
        if (ok)
            path = path_join(prefix, name);
        if (!path || !remember(path)) {
            fprintf(stderr, "drop: cannot name the saved image\n");
            unlinkat(dir, name, 0);
            free(path);
            path = NULL;
        }
    }
    close(dir);
    free(prefix);
    return path;
}

char *terminal_quote_path(const char *path)
{
    if (!path || !*path) return NULL;
    size_t len = strlen(path), quotes = 0;
    if (len > INPUT_TEXT_MAX / 4) return NULL;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)path[i] < 0x20 || path[i] == 0x7f) return NULL;
        if (path[i] == '\'') quotes++;
    }
    if (len + quotes * 3 + 3 > INPUT_TEXT_MAX) return NULL;
    char *out = malloc(len + quotes * 3 + 3);
    if (!out) return NULL;
    size_t n = 0;
    out[n++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = path[i];
        }
    }
    out[n++] = '\'';
    out[n] = 0;
    return out;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *terminal_quote_uri_list(const char *data, size_t len)
{
    if (!data || len > INPUT_TEXT_MAX) return NULL;
    char *out = calloc(len * 4 + 1, 1);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t start = 0; start < len;) {
        size_t end = start;
        while (end < len && data[end] != '\r' && data[end] != '\n') end++;
        size_t path = start + 7;
        if (end >= path && !memcmp(data + start, "file://", 7)) {
            if (end >= path + 9 && !memcmp(data + path, "localhost", 9)) path += 9;
            if (path < end && data[path] == '/') {
                char *decoded = malloc(end - path + 1);
                if (!decoded) break;
                size_t n = 0;
                for (size_t i = path; i < end; i++) {
                    unsigned char c = (unsigned char)data[i];
                    if (c == '%' && i + 2 < end) {
                        int hi = hex(data[i + 1]), lo = hex(data[i + 2]);
                        if (hi >= 0 && lo >= 0) { c = (unsigned char)(hi * 16 + lo); i += 2; }
                    }
                    decoded[n++] = (char)c;
                }
                decoded[n] = 0;
                if (strlen(decoded) == n) {
                    char *quoted = terminal_quote_path(decoded);
                    if (quoted) {
                        size_t qlen = strlen(quoted);
                        if (used) out[used++] = ' ';
                        memcpy(out + used, quoted, qlen);
                        used += qlen;
                        free(quoted);
                    }
                }
                free(decoded);
            }
        }
        start = end;
        while (start < len && (data[start] == '\r' || data[start] == '\n')) start++;
    }
    if (!used) { free(out); return NULL; }
    return out;
}

char *terminal_join_uri_lines(const char *data, size_t len)
{
    if (!data || len > INPUT_TEXT_MAX) return NULL;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t used = 0;
    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        const char *line_end = p;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        while (line_end > line &&
               (line_end[-1] == ' ' || line_end[-1] == '\t'))
            line_end--;
        if (line_end == line || *line == '#')
            continue;
        if (used)
            out[used++] = ' ';
        size_t size = (size_t)(line_end - line);
        memcpy(out + used, line, size);
        used += size;
    }
    out[used] = 0;
    if (!used) { free(out); return NULL; }
    return out;
}

