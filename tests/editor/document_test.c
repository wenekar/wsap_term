#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "editor/document.h"
#include "editor/file.h"

static void write_file(const char *path, const char *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static void expect_file(const char *path, const char *expected)
{
    size_t length = 0;
    char *bytes = editor_file_read(path, &length);
    assert(bytes);
    assert(length == strlen(expected));
    assert(memcmp(bytes, expected, length) == 0);
    free(bytes);
}

static void expect_document(const struct editor_document *document,
                            const char *expected)
{
    const struct editor_text *text = editor_document_text(document);
    size_t length = strlen(expected);
    assert(editor_text_length(text) == length);
    char *copy = malloc(length + 1);
    assert(copy);
    assert(editor_text_copy(text, 0, length, copy) == length);
    assert(memcmp(copy, expected, length) == 0);
    free(copy);
}

static void test_save_and_reload(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/save.txt", directory);
    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(editor_document_path(document));
    assert(strcmp(editor_document_path(document), path) == 0);
    assert(!editor_document_modified(document));
    assert(!editor_document_disk_changed(document));

    assert(editor_document_insert(document, 0, "one\ntwo\n", 8));
    assert(editor_document_modified(document));
    assert(editor_document_save(document));
    assert(!editor_document_modified(document));
    assert(!editor_document_disk_changed(document));
    expect_file(path, "one\ntwo\n");
    editor_document_destroy(document);

    document = editor_document_create();
    assert(editor_document_open(document, path));
    expect_document(document, "one\ntwo\n");
    assert(editor_text_line_count(editor_document_text(document)) == 3);
    assert(!editor_document_modified(document));
    assert(editor_document_reload(document));
    expect_document(document, "one\ntwo\n");
    editor_document_destroy(document);
    unlink(path);
}

static void test_crlf_round_trip(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/crlf.txt", directory);
    write_file(path, "a\r\nb\r\n", 6);
    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    const struct editor_text *text = editor_document_text(document);
    assert(editor_text_line_ending(text) == EDITOR_LINE_CRLF);
    assert(editor_text_crlf_count(text) == 2);
    assert(editor_text_line_count(text) == 3);
    assert(editor_document_save(document));
    expect_file(path, "a\r\nb\r\n");

    assert(editor_document_insert(document, 6, "x\n", 2));
    assert(editor_document_save(document));
    expect_file(path, "a\r\nb\r\nx\n");
    assert(editor_document_insert(document, 0, "\r\n", 2));
    assert(editor_document_save(document));
    expect_file(path, "\r\na\r\nb\r\nx\n");
    editor_document_destroy(document);
    unlink(path);
}

static void test_undo_groups(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/undo.txt", directory);
    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(editor_document_insert(document, 0, "a", 1));
    assert(editor_document_insert(document, 1, "b", 1));
    assert(editor_document_insert(document, 2, "c", 1));
    expect_document(document, "abc");
    assert(editor_document_undo(document));
    expect_document(document, "");
    assert(editor_document_redo(document));
    expect_document(document, "abc");

    editor_document_break_undo(document);
    assert(editor_document_insert(document, 3, "d", 1));
    editor_document_break_undo(document);
    assert(editor_document_insert(document, 4, "e", 1));
    expect_document(document, "abcde");
    assert(editor_document_undo(document));
    expect_document(document, "abcd");
    assert(editor_document_undo(document));
    expect_document(document, "abc");
    assert(editor_document_undo(document));
    expect_document(document, "");
    assert(!editor_document_undo(document));
    editor_document_destroy(document);
    unlink(path);
}

static void test_external_change(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/external.txt", directory);
    write_file(path, "one", 3);
    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(!editor_document_disk_changed(document));

    write_file(path, "one two", 7);
    assert(editor_document_disk_changed(document));
    assert(editor_document_reload(document));
    expect_document(document, "one two");
    assert(!editor_document_disk_changed(document));

    struct timespec times[2] = {
        { .tv_sec = time(NULL) + 10, .tv_nsec = 0 },
        { .tv_sec = time(NULL) + 10, .tv_nsec = 0 },
    };
    write_file(path, "other!!", 7);
    assert(utimensat(AT_FDCWD, path, times, 0) == 0);
    assert(editor_document_disk_changed(document));
    editor_document_destroy(document);
    unlink(path);
}

static void test_permissions(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/mode.txt", directory);
    write_file(path, "x", 1);
    assert(chmod(path, 0640) == 0);
    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(editor_document_insert(document, 0, "y", 1));
    assert(editor_document_save(document));
    struct stat st;
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 07777) == 0640);
    editor_document_destroy(document);
    unlink(path);
}

static void test_journal_restore(const char *directory)
{
    char path[PATH_MAX];
    char journal[PATH_MAX];
    snprintf(path, sizeof(path), "%s/restore.txt", directory);
    snprintf(journal, sizeof(journal), "%s/restore.journal", directory);
    write_file(path, "base\n", 5);

    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(editor_document_journal_attach(document, journal));
    assert(editor_document_journal_ok(document));
    assert(editor_document_insert(document, 0, "typed", 5));
    assert(editor_document_journal_cursor(document, 5, 2));
    expect_document(document, "typedbase\n");
    /* A crash keeps the journal and leaves the file alone. */
    editor_document_destroy(document);
    expect_file(path, "base\n");

    document = editor_document_create();
    uint64_t caret = 0;
    uint64_t anchor = 0;
    assert(editor_document_restore(document, journal, &caret, &anchor) ==
           EDITOR_JOURNAL_OK);
    assert(editor_document_path(document));
    assert(strcmp(editor_document_path(document), path) == 0);
    expect_document(document, "typedbase\n");
    assert(editor_document_modified(document));
    assert(caret == 5 && anchor == 2);
    assert(editor_document_save(document));
    expect_file(path, "typedbase\n");
    assert(!editor_document_modified(document));
    editor_document_destroy(document);
    assert(access(journal, F_OK) == 0);
    unlink(journal);
    unlink(path);
}

static void test_journal_after_save(const char *directory)
{
    char path[PATH_MAX];
    char journal[PATH_MAX];
    snprintf(path, sizeof(path), "%s/saved.txt", directory);
    snprintf(journal, sizeof(journal), "%s/saved.journal", directory);

    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(editor_document_journal_attach(document, journal));
    assert(editor_document_insert(document, 0, "x", 1));
    assert(editor_document_save(document));
    /* The journal now starts from the saved file, with no edit record. */
    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(journal, &recovery) == EDITOR_JOURNAL_OK);
    assert(recovery.length == 1);
    assert(memcmp(recovery.text, "x", 1) == 0);
    assert(recovery.edits == 0);
    editor_recovery_destroy(&recovery);

    assert(editor_document_insert(document, 1, "y", 1));
    expect_document(document, "xy");
    editor_document_destroy(document);
    memset(&recovery, 0, sizeof(recovery));
    assert(editor_journal_replay(journal, &recovery) == EDITOR_JOURNAL_OK);
    assert(recovery.length == 2);
    assert(memcmp(recovery.text, "xy", 2) == 0);
    assert(recovery.edits == 1);
    editor_recovery_destroy(&recovery);
    expect_file(path, "x");
    unlink(journal);
    unlink(path);
}

int main(void)
{
    char directory[] = "/tmp/editor-document-XXXXXX";
    assert(mkdtemp(directory));
    test_save_and_reload(directory);
    test_crlf_round_trip(directory);
    test_undo_groups(directory);
    test_external_change(directory);
    test_permissions(directory);
    test_journal_restore(directory);
    test_journal_after_save(directory);
    assert(rmdir(directory) == 0);
    printf("PASS: editor document tests\n");
    return 0;
}
