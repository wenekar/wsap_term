#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "editor/file.h"

static void write_file(const char *path, const char *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static mode_t file_mode(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return st.st_mode & 07777;
}

static ino_t file_inode(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return st.st_ino;
}

static int directory_entries(const char *path)
{
    DIR *directory = opendir(path);
    assert(directory);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            count++;
    }
    closedir(directory);
    return count;
}

static void test_read_and_save(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/note.txt", directory);
    const char bytes[] = "hello\0world";
    struct editor_file_state state;
    assert(editor_file_save(path, bytes, sizeof(bytes) - 1, &state));
    assert(state.present);
    assert(state.size == sizeof(bytes) - 1);
    assert(editor_file_check(&state, path) == EDITOR_FILE_UNCHANGED);

    size_t length = 0;
    char *read = editor_file_read(path, &length);
    assert(read);
    assert(length == sizeof(bytes) - 1);
    assert(memcmp(read, bytes, length) == 0);
    assert(read[length] == '\0');
    free(read);
    unlink(path);
}

static void test_mode_and_rename(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/mode.txt", directory);
    write_file(path, "old", 3);
    assert(chmod(path, 0640) == 0);
    ino_t before = file_inode(path);

    struct editor_file_state state;
    assert(editor_file_save(path, "new text", 8, &state));
    assert(file_mode(path) == 0640);
    assert(file_inode(path) != before);
    assert(editor_file_check(&state, path) == EDITOR_FILE_UNCHANGED);

    size_t length = 0;
    char *read = editor_file_read(path, &length);
    assert(read);
    assert(length == 8);
    assert(memcmp(read, "new text", 8) == 0);
    free(read);
    /* No temporary file stays behind. */
    assert(directory_entries(directory) == 1);
    unlink(path);
}

static void test_new_file_mode(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/new.txt", directory);
    mode_t mask = umask(0022);
    struct editor_file_state state;
    assert(editor_file_save(path, "x", 1, &state));
    umask(mask);
    assert(file_mode(path) == 0644);
    unlink(path);
}

static void test_missing_file(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/absent.txt", directory);
    errno = 0;
    assert(editor_file_read(path, NULL) == NULL);
    assert(errno == ENOENT);

    struct editor_file_state state = {0};
    assert(editor_file_check(&state, path) == EDITOR_FILE_UNCHANGED);
    write_file(path, "x", 1);
    assert(editor_file_check(&state, path) == EDITOR_FILE_CHANGED);

    struct editor_file_state present;
    assert(editor_file_state_get(path, &present));
    unlink(path);
    assert(editor_file_check(&present, path) == EDITOR_FILE_MISSING);

    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/no/such/dir/file", directory);
    assert(!editor_file_save(missing, "x", 1, NULL));
}

static void test_external_change(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/external.txt", directory);
    struct editor_file_state state;
    assert(editor_file_save(path, "first", 5, &state));
    assert(editor_file_check(&state, path) == EDITOR_FILE_UNCHANGED);

    write_file(path, "second", 6);
    assert(editor_file_check(&state, path) == EDITOR_FILE_CHANGED);

    assert(editor_file_save(path, "third", 5, &state));
    assert(editor_file_check(&state, path) == EDITOR_FILE_UNCHANGED);
    /* A same-size change with a later timestamp is a change too. */
    write_file(path, "other", 5);
    struct timespec times[2] = {
        { .tv_sec = state.mtime_ns / 1000000000 + 5, .tv_nsec = 0 },
        { .tv_sec = state.mtime_ns / 1000000000 + 5, .tv_nsec = 0 },
    };
    assert(utimensat(AT_FDCWD, path, times, 0) == 0);
    assert(editor_file_check(&state, path) == EDITOR_FILE_CHANGED);
    unlink(path);
}

static void test_hash(void)
{
    assert(editor_file_hash("", 0) == editor_file_hash(NULL, 0));
    assert(editor_file_hash("abc", 3) != editor_file_hash("abd", 3));
    assert(editor_file_hash("abc", 3) == editor_file_hash("abc", 3));
    assert(editor_file_hash("abc", 2) != editor_file_hash("abc", 3));
}

int main(void)
{
    char directory[] = "/tmp/editor-file-XXXXXX";
    assert(mkdtemp(directory));
    test_read_and_save(directory);
    test_mode_and_rename(directory);
    test_new_file_mode(directory);
    test_missing_file(directory);
    test_external_change(directory);
    test_hash();
    assert(rmdir(directory) == 0);
    printf("PASS: editor file tests\n");
    return 0;
}
