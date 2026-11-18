#ifndef EDITOR_LARGEFILE_H
#define EDITOR_LARGEFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A file above this size opens in large-file mode. */
#define EDITOR_LARGEFILE_THRESHOLD ((uint64_t)50 * 1024 * 1024)

/* A line of a window into the file, in bytes. */
struct editor_largefile_line {
    uint64_t start;
    uint64_t length;
};

/* A read-only mapping of a file. Large-file mode reads through the mapping and
 * does no parse work over the whole file. */
struct editor_largefile;

/* Maps the file at `path`. Returns NULL with errno set. */
struct editor_largefile *editor_largefile_open(const char *path);
void editor_largefile_close(struct editor_largefile *file);

const char *editor_largefile_bytes(const struct editor_largefile *file);
uint64_t editor_largefile_size(const struct editor_largefile *file);

/* True when the file is larger than `threshold`. The default threshold is
 * `EDITOR_LARGEFILE_THRESHOLD`. */
bool editor_largefile_exceeds(const struct editor_largefile *file, uint64_t threshold);
/* True when the file is larger than the editor can hold as a list of lines.
 * Large-file mode turns off word wrap and the whole-file parse work. */
bool editor_largefile_mode(const struct editor_largefile *file);

/* Looks at the first `scan` bytes. True when the bytes hold a NUL, or when the
 * text is not UTF-8. `scan` of 0 reads the first 8192 bytes. */
bool editor_largefile_binary(const struct editor_largefile *file, size_t scan);

/* Finds the line that holds `offset` without a line index. Returns false past
 * the end of the file. */
bool editor_largefile_locate(const struct editor_largefile *file, uint64_t offset,
                             struct editor_largefile_line *line);

/* Counts the lines in `length` bytes from `offset`. A CRLF pair is one break. */
uint64_t editor_largefile_count_lines(const struct editor_largefile *file,
                                      uint64_t offset, uint64_t length);

/* Slices one line into a window of `width` bytes. `column` is the first byte of
 * the line to return. The result holds `*count` slices of at most `width` bytes
 * each. It returns the byte column of the next slice, which is past the end of
 * the line when the window holds all of it. `slices` and `count` are
 * optional. */
uint64_t editor_largefile_slice_line(const struct editor_largefile *file,
                                     const struct editor_largefile_line *line,
                                     uint64_t column, uint64_t width,
                                     struct editor_largefile_line *slices,
                                     size_t capacity, size_t *count);

#endif
