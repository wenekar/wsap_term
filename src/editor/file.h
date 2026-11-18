#ifndef EDITOR_FILE_H
#define EDITOR_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The state of a file on disk, as `stat` reports it. */
struct editor_file_state {
    /* A regular file was present. */
    bool present;
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    int64_t mtime_ns;
};

enum editor_file_result {
    EDITOR_FILE_UNCHANGED = 0,
    EDITOR_FILE_CHANGED,
    EDITOR_FILE_MISSING,
};

/* Reads the whole file. Returns a NUL-terminated buffer, or NULL with errno
 * set. `length` is optional. */
char *editor_file_read(const char *path, size_t *length);

/* Reads the state of the file. Returns false with errno set when the file is
 * missing or is not a regular file. */
bool editor_file_state_get(const char *path, struct editor_file_state *state);

bool editor_file_state_equal(const struct editor_file_state *a,
                             const struct editor_file_state *b);

/* Compares the file to a recorded state. */
enum editor_file_result editor_file_check(const struct editor_file_state *state,
                                          const char *path);

/* Writes the bytes to the file through a temporary file, a flush to disk, and
 * a rename. The mode of the file stays. `state` returns the new state. */
bool editor_file_save(const char *path, const char *bytes, size_t length,
                      struct editor_file_state *state);

/* Hashes the bytes. The journal uses the hash to identify the base text. */
uint64_t editor_file_hash(const char *bytes, size_t length);

#endif
