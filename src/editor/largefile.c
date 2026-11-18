#define _GNU_SOURCE

#include "editor/largefile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "editor/encoding.h"

#define LARGEFILE_DEFAULT_SCAN 8192

struct editor_largefile {
    void *mapping;
    uint64_t size;
};

struct editor_largefile *editor_largefile_open(const char *path)
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
    struct editor_largefile *file = calloc(1, sizeof(*file));
    if (!file) {
        close(fd);
        return NULL;
    }
    /* A zero-length file has no mapping. An empty mapping is an error. */
    if (st.st_size > 0) {
        file->mapping = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (file->mapping == MAP_FAILED) {
            int error = errno;
            free(file);
            close(fd);
            errno = error;
            return NULL;
        }
    }
    file->size = (uint64_t)st.st_size;
    close(fd);
    return file;
}

void editor_largefile_close(struct editor_largefile *file)
{
    if (!file)
        return;
    if (file->mapping)
        munmap(file->mapping, (size_t)file->size);
    free(file);
}

const char *editor_largefile_bytes(const struct editor_largefile *file)
{
    if (!file || !file->mapping)
        return "";
    return file->mapping;
}

uint64_t editor_largefile_size(const struct editor_largefile *file)
{
    return file ? file->size : 0;
}

bool editor_largefile_exceeds(const struct editor_largefile *file, uint64_t threshold)
{
    return editor_largefile_size(file) > threshold;
}

bool editor_largefile_mode(const struct editor_largefile *file)
{
    return editor_largefile_exceeds(file, EDITOR_LARGEFILE_THRESHOLD);
}

bool editor_largefile_binary(const struct editor_largefile *file, size_t scan)
{
    if (!file)
        return false;
    if (scan == 0)
        scan = LARGEFILE_DEFAULT_SCAN;
    if (scan > file->size)
        scan = (size_t)file->size;
    const char *bytes = editor_largefile_bytes(file);
    if (memchr(bytes, '\0', scan))
        return true;
    return !editor_encoding_utf8_valid(bytes, scan);
}

/* True when the byte at `offset` ends a line. */
static bool is_break(char byte)
{
    return byte == '\n' || byte == '\r';
}

bool editor_largefile_locate(const struct editor_largefile *file, uint64_t offset,
                             struct editor_largefile_line *line)
{
    if (!file || offset >= file->size)
        return false;
    const char *bytes = editor_largefile_bytes(file);
    uint64_t start = offset;
    while (start > 0 && bytes[start - 1] != '\n')
        start--;
    uint64_t end = offset;
    while (end < file->size && bytes[end] != '\n')
        end++;
    while (end > start && is_break(bytes[end - 1]))
        end--;
    if (line)
        *line = (struct editor_largefile_line){.start = start, .length = end - start};
    return true;
}

uint64_t editor_largefile_count_lines(const struct editor_largefile *file,
                                      uint64_t offset, uint64_t length)
{
    if (!file || offset >= file->size)
        return 0;
    if (length > file->size - offset)
        length = file->size - offset;
    const char *bytes = editor_largefile_bytes(file);
    uint64_t count = 1;
    for (uint64_t i = 0; i < length; i++) {
        if (bytes[offset + i] == '\n')
            count++;
    }
    return count;
}

uint64_t editor_largefile_slice_line(const struct editor_largefile *file,
                                     const struct editor_largefile_line *line,
                                     uint64_t column, uint64_t width,
                                     struct editor_largefile_line *slices,
                                     size_t capacity, size_t *count)
{
    if (count)
        *count = 0;
    if (!file || !line || width == 0)
        return column;
    if (column > line->length)
        column = line->length;
    uint64_t remaining = line->length - column;
    uint64_t next = line->start + column;
    size_t used = 0;
    while (remaining > 0) {
        uint64_t take = remaining < width ? remaining : width;
        if (slices && used < capacity) {
            slices[used] = (struct editor_largefile_line){.start = next, .length = take};
            used++;
        }
        next += take;
        remaining -= take;
        /* One more slice needs room in the window. */
        if (slices && used == capacity && remaining > 0)
            break;
    }
    if (count)
        *count = used;
    return next;
}
