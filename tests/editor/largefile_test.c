#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "editor/largefile.h"

static char *make_directory(void)
{
    static char path[] = "/tmp/editor-largefile-XXXXXX";
    char *directory = mkdtemp(path);
    assert(directory);
    return directory;
}

static void write_file(const char *path, const char *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

/* A small file must not select large-file mode. The threshold is the contract,
 * not the actual size of the test file. */
static void test_threshold(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/small.txt", directory);
    write_file(path, "one\ntwo\n", 8);
    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(editor_largefile_size(file) == 8);
    assert(!editor_largefile_mode(file));
    assert(!editor_largefile_exceeds(file, EDITOR_LARGEFILE_THRESHOLD));
    assert(editor_largefile_exceeds(file, 7));
    /* An 8-byte file exceeds a threshold of 8 only when it is larger. */
    assert(!editor_largefile_exceeds(file, 8));
    editor_largefile_close(file);
}

/* A file above the threshold opens without a line index. `editor_largefile_mode`
 * is the flag that turns off word wrap and the parse work. */
static void test_large_file_mode(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/large.txt", directory);
    uint64_t size = EDITOR_LARGEFILE_THRESHOLD + 1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    char block[4096];
    memset(block, 'a', sizeof(block));
    block[sizeof(block) - 1] = '\n';
    uint64_t done = 0;
    while (done < size) {
        uint64_t take = size - done < sizeof(block) ? size - done : sizeof(block);
        assert(write(fd, block, (size_t)take) == (ssize_t)take);
        done += take;
    }
    assert(close(fd) == 0);

    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(editor_largefile_size(file) == size);
    assert(editor_largefile_mode(file));
    assert(editor_largefile_bytes(file) != NULL);
    /* The mapping holds the same bytes as the file. */
    assert(editor_largefile_bytes(file)[0] == 'a');
    assert(editor_largefile_count_lines(file, 0, size) == 12801);

    /* A locate uses no index. It scans to the line boundaries only. */
    struct editor_largefile_line line;
    assert(editor_largefile_locate(file, size - 1, &line));
    assert(line.start == size - 1);
    assert(line.length == 1);
    editor_largefile_close(file);
    assert(unlink(path) == 0);
}

/* A NUL byte marks a binary file. */
static void test_binary_nul(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/nul.bin", directory);
    const char bytes[] = "ELF\0\1\2";
    write_file(path, bytes, sizeof(bytes) - 1);
    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(editor_largefile_binary(file, 0));
    editor_largefile_close(file);
}

/* A byte that is not valid UTF-8 marks a binary file. */
static void test_binary_invalid_utf8(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/broken.bin", directory);
    const char bytes[] = "text\xFFmore";
    write_file(path, bytes, sizeof(bytes) - 1);
    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(editor_largefile_binary(file, 0));
    editor_largefile_close(file);
}

static void test_text_is_not_binary(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/text.txt", directory);
    const char bytes[] = "caf\xC3\xA9\n";
    write_file(path, bytes, sizeof(bytes) - 1);
    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(!editor_largefile_binary(file, 0));
    editor_largefile_close(file);
}

/* One line of 1000 bytes shows in three slices of 400 bytes. */
static void test_slice_long_line(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/longline.txt", directory);
    char *bytes = malloc(1001);
    assert(bytes);
    memset(bytes, 'x', 1000);
    bytes[1000] = '\n';
    write_file(path, bytes, 1001);
    free(bytes);

    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    struct editor_largefile_line line;
    assert(editor_largefile_locate(file, 0, &line));
    assert(line.start == 0);
    assert(line.length == 1000);

    struct editor_largefile_line slices[3];
    size_t count = 0;
    uint64_t next = editor_largefile_slice_line(file, &line, 0, 400, slices, 3, &count);
    assert(count == 3);
    assert(slices[0].start == 0 && slices[0].length == 400);
    assert(slices[1].start == 400 && slices[1].length == 400);
    assert(slices[2].start == 800 && slices[2].length == 200);
    assert(next == 1000);

    /* A later window starts at the requested byte column. */
    next = editor_largefile_slice_line(file, &line, 500, 400, slices, 3, &count);
    assert(count == 2);
    assert(slices[0].start == 500 && slices[0].length == 400);
    assert(slices[1].start == 900 && slices[1].length == 100);
    assert(next == 1000);

    /* A column past the line gives an empty window. */
    next = editor_largefile_slice_line(file, &line, 2000, 400, slices, 3, &count);
    assert(count == 0);
    assert(next == 1000);
    editor_largefile_close(file);
}

/* A CRLF pair is one line break. */
static void test_crlf_lines(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/crlf.txt", directory);
    const char bytes[] = "a\r\nb\r\nc";
    write_file(path, bytes, sizeof(bytes) - 1);
    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(editor_largefile_count_lines(file, 0, sizeof(bytes) - 1) == 3);
    struct editor_largefile_line line;
    assert(editor_largefile_locate(file, 0, &line));
    assert(line.length == 1);
    assert(editor_largefile_locate(file, 3, &line));
    assert(line.start == 3 && line.length == 1);
    assert(!editor_largefile_locate(file, sizeof(bytes) - 1, &line));
    editor_largefile_close(file);
}

static void test_empty_file(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/empty.txt", directory);
    write_file(path, "", 0);
    struct editor_largefile *file = editor_largefile_open(path);
    assert(file);
    assert(editor_largefile_size(file) == 0);
    assert(!editor_largefile_binary(file, 0));
    assert(strcmp(editor_largefile_bytes(file), "") == 0);
    editor_largefile_close(file);
}

int main(void)
{
    char *directory = make_directory();
    test_threshold(directory);
    test_large_file_mode(directory);
    test_binary_nul(directory);
    test_binary_invalid_utf8(directory);
    test_text_is_not_binary(directory);
    test_slice_long_line(directory);
    test_crlf_lines(directory);
    test_empty_file(directory);
    printf("editor largefile tests passed\n");
    return 0;
}
