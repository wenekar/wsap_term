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
#include <unistd.h>

#include "editor/file.h"
#include "editor/journal.h"

static void write_file(const char *path, const char *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static void expect_text(const struct editor_recovery *recovery, const char *expected)
{
    size_t length = strlen(expected);
    assert(recovery->length == length);
    assert(memcmp(recovery->text, expected, length) == 0);
    assert(recovery->text[length] == '\0');
}

static void test_untitled(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/untitled.journal", directory);
    struct editor_session session = { .base_hash = editor_file_hash("", 0) };
    struct editor_journal *journal = editor_journal_create(path, &session);
    assert(journal);
    assert(editor_journal_edit(journal, 0, 0, "hello", 5));
    assert(editor_journal_edit(journal, 5, 0, " world", 6));
    assert(editor_journal_cursor(journal, 11, 3));
    assert(editor_journal_sync(journal));
    editor_journal_destroy(journal, false);

    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(path, &recovery) == EDITOR_JOURNAL_OK);
    assert(recovery.path == NULL);
    expect_text(&recovery, "hello world");
    assert(recovery.caret == 11 && recovery.anchor == 3);
    assert(recovery.edits == 2);
    assert(!recovery.truncated);
    assert(!recovery.base.present);
    editor_recovery_destroy(&recovery);
    unlink(path);
}

static void test_base_file(const char *directory)
{
    char file[PATH_MAX];
    char journal_path[PATH_MAX];
    snprintf(file, sizeof(file), "%s/base.txt", directory);
    snprintf(journal_path, sizeof(journal_path), "%s/base.journal", directory);
    write_file(file, "one\ntwo\n", 8);

    struct editor_session session = { .path = file };
    assert(editor_file_state_get(file, &session.base));
    session.base_hash = editor_file_hash("one\ntwo\n", 8);
    struct editor_journal *journal = editor_journal_create(journal_path, &session);
    assert(journal);
    assert(editor_journal_edit(journal, 0, 0, "> ", 2));
    assert(editor_journal_edit(journal, 9, 1, "", 0));
    editor_journal_destroy(journal, false);

    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(journal_path, &recovery) == EDITOR_JOURNAL_OK);
    assert(recovery.path && strcmp(recovery.path, file) == 0);
    expect_text(&recovery, "> one\ntwo");
    assert(recovery.base.present);
    assert(recovery.edits == 2);
    assert(!recovery.truncated);
    editor_recovery_destroy(&recovery);
    unlink(journal_path);
    unlink(file);
}

static void test_crash_replay(const char *directory)
{
    char file[PATH_MAX];
    char journal_path[PATH_MAX];
    snprintf(file, sizeof(file), "%s/crash.txt", directory);
    snprintf(journal_path, sizeof(journal_path), "%s/crash.journal", directory);
    write_file(file, "base\n", 5);

    struct editor_session session = { .path = file };
    assert(editor_file_state_get(file, &session.base));
    session.base_hash = editor_file_hash("base\n", 5);
    struct editor_journal *journal = editor_journal_create(journal_path, &session);
    assert(journal);
    assert(editor_journal_edit(journal, 0, 0, "abc", 3));
    struct stat st;
    assert(stat(journal_path, &st) == 0);
    off_t after_first = st.st_size;
    assert(editor_journal_edit(journal, 3, 0, "def", 3));
    /* A crash closes the descriptor and keeps the records. */
    editor_journal_destroy(journal, false);

    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(journal_path, &recovery) == EDITOR_JOURNAL_OK);
    expect_text(&recovery, "abcdefbase\n");
    assert(recovery.edits == 2);
    editor_recovery_destroy(&recovery);

    /* A write that a crash stopped part way gives the records before it. */
    assert(truncate(journal_path, after_first + 1) == 0);
    memset(&recovery, 0, sizeof(recovery));
    assert(editor_journal_replay(journal_path, &recovery) == EDITOR_JOURNAL_OK);
    expect_text(&recovery, "abcbase\n");
    assert(recovery.edits == 1);
    assert(recovery.truncated);
    editor_recovery_destroy(&recovery);
    unlink(journal_path);
    unlink(file);
}

static void test_base_changed(const char *directory)
{
    char file[PATH_MAX];
    char journal_path[PATH_MAX];
    snprintf(file, sizeof(file), "%s/changed.txt", directory);
    snprintf(journal_path, sizeof(journal_path), "%s/changed.journal", directory);
    write_file(file, "one", 3);

    struct editor_session session = { .path = file };
    assert(editor_file_state_get(file, &session.base));
    session.base_hash = editor_file_hash("one", 3);
    struct editor_journal *journal = editor_journal_create(journal_path, &session);
    assert(journal);
    assert(editor_journal_edit(journal, 0, 0, "x", 1));
    assert(editor_journal_cursor(journal, 4, 4));
    editor_journal_destroy(journal, false);

    write_file(file, "two", 3);
    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(journal_path, &recovery) == EDITOR_JOURNAL_BASE_CHANGED);
    assert(recovery.path && strcmp(recovery.path, file) == 0);
    assert(recovery.caret == 4 && recovery.anchor == 4);
    assert(recovery.text == NULL);
    editor_recovery_destroy(&recovery);

    unlink(file);
    memset(&recovery, 0, sizeof(recovery));
    assert(editor_journal_replay(journal_path, &recovery) == EDITOR_JOURNAL_BASE_MISSING);
    assert(recovery.path && strcmp(recovery.path, file) == 0);
    editor_recovery_destroy(&recovery);
    unlink(journal_path);
}

static void test_none(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/absent.journal", directory);
    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(path, &recovery) == EDITOR_JOURNAL_NONE);
    assert(recovery.text == NULL);

    write_file(path, "", 0);
    assert(editor_journal_replay(path, &recovery) == EDITOR_JOURNAL_NONE);
    write_file(path, "not a journal", 13);
    assert(editor_journal_replay(path, &recovery) == EDITOR_JOURNAL_NONE);
    editor_recovery_destroy(&recovery);
    unlink(path);
}

static void test_commit(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/commit.journal", directory);
    struct editor_session session = { .base_hash = editor_file_hash("", 0) };
    struct editor_journal *journal = editor_journal_create(path, &session);
    assert(journal);
    assert(editor_journal_edit(journal, 0, 0, "x", 1));
    editor_journal_destroy(journal, true);
    assert(access(path, F_OK) != 0);
    struct editor_recovery recovery = {0};
    assert(editor_journal_replay(path, &recovery) == EDITOR_JOURNAL_NONE);
    editor_recovery_destroy(&recovery);
}

int main(void)
{
    char directory[] = "/tmp/editor-journal-XXXXXX";
    assert(mkdtemp(directory));
    test_untitled(directory);
    test_base_file(directory);
    test_crash_replay(directory);
    test_base_changed(directory);
    test_none(directory);
    test_commit(directory);
    assert(rmdir(directory) == 0);
    printf("PASS: editor journal tests\n");
    return 0;
}
