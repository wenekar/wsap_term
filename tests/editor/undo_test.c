#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "editor/text.h"
#include "editor/undo.h"

static void expect_text(const struct editor_text *text, const char *expected)
{
    size_t length = strlen(expected);
    char copy[64];
    assert(length < sizeof(copy));
    assert(editor_text_length(text) == length);
    assert(editor_text_copy(text, 0, length, copy) == length);
    assert(memcmp(copy, expected, length) == 0);
}

static void replace(struct editor_undo *undo, struct editor_text *text,
                    enum editor_edit_kind kind, uint64_t offset, uint64_t remove,
                    const char *insert)
{
    size_t length = insert ? strlen(insert) : 0;
    assert(editor_undo_replace(undo, text, kind, offset, remove, insert, length));
}

static void test_typing_group(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);
    assert(!editor_undo_can_undo(undo));
    assert(!editor_undo_can_redo(undo));

    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "a");
    replace(undo, text, EDITOR_EDIT_TYPING, 1, 0, "b");
    replace(undo, text, EDITOR_EDIT_TYPING, 2, 0, "c");
    expect_text(text, "abc");
    assert(editor_undo_can_undo(undo));
    assert(editor_undo_undo(undo, text));
    expect_text(text, "");
    assert(!editor_undo_can_undo(undo));
    assert(editor_undo_can_redo(undo));
    assert(editor_undo_redo(undo, text));
    expect_text(text, "abc");

    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

static void test_break(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "abc");
    editor_undo_break(undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 3, 0, "d");
    expect_text(text, "abcd");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "abc");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "");
    assert(!editor_undo_undo(undo, text));
    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

static void test_delete_runs(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);
    assert(editor_text_set(text, "abcd", 4));

    replace(undo, text, EDITOR_EDIT_DELETE, 3, 1, NULL);
    replace(undo, text, EDITOR_EDIT_DELETE, 2, 1, NULL);
    replace(undo, text, EDITOR_EDIT_DELETE, 1, 1, NULL);
    expect_text(text, "a");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "abcd");
    assert(editor_undo_redo(undo, text));
    expect_text(text, "a");

    editor_undo_clear(undo);
    assert(editor_text_set(text, "abcd", 4));
    for (int i = 0; i < 3; i++)
        replace(undo, text, EDITOR_EDIT_DELETE, 0, 1, NULL);
    expect_text(text, "d");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "abcd");
    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

static void test_groups_stay_apart(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);

    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "a");
    replace(undo, text, EDITOR_EDIT_COMMAND, 1, 0, "b");
    expect_text(text, "ab");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "a");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "");

    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "a");
    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "x");
    expect_text(text, "xa");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "a");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "");

    replace(undo, text, EDITOR_EDIT_COMMAND, 0, 0, "abc");
    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "z");
    expect_text(text, "zabc");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "abc");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "");
    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

static void test_replace_and_undo(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);
    assert(editor_text_set(text, "abcdef", 6));
    replace(undo, text, EDITOR_EDIT_COMMAND, 2, 3, "XY");
    expect_text(text, "abXYf");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "abcdef");
    assert(editor_undo_redo(undo, text));
    expect_text(text, "abXYf");

    /* A failed edit leaves the text and the history alone. */
    assert(!editor_undo_replace(undo, text, EDITOR_EDIT_TYPING, 100, 0, "x", 1));
    expect_text(text, "abXYf");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "abcdef");
    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

static void test_modified_state(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);
    assert(!editor_undo_modified(undo));

    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "a");
    editor_undo_break(undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 1, 0, "b");
    assert(editor_undo_modified(undo));
    editor_undo_mark_saved(undo);
    assert(!editor_undo_modified(undo));

    editor_undo_break(undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 2, 0, "c");
    assert(editor_undo_modified(undo));
    assert(editor_undo_undo(undo, text));
    assert(!editor_undo_modified(undo));
    assert(editor_undo_undo(undo, text));
    assert(editor_undo_modified(undo));
    assert(editor_undo_redo(undo, text));
    assert(!editor_undo_modified(undo));

    editor_undo_mark_modified(undo);
    assert(editor_undo_modified(undo));
    editor_undo_mark_saved(undo);
    assert(!editor_undo_modified(undo));

    /* A new edit drops the redo part of the history. */
    assert(editor_undo_undo(undo, text));
    editor_undo_break(undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 1, 0, "z");
    assert(!editor_undo_can_redo(undo));
    assert(editor_undo_modified(undo));
    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

static void test_redo_cleared(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_undo *undo = editor_undo_create();
    assert(text && undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 0, 0, "a");
    editor_undo_break(undo);
    replace(undo, text, EDITOR_EDIT_TYPING, 1, 0, "b");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "a");
    assert(editor_undo_can_redo(undo));
    replace(undo, text, EDITOR_EDIT_TYPING, 1, 0, "c");
    expect_text(text, "ac");
    assert(!editor_undo_can_redo(undo));
    assert(editor_undo_undo(undo, text));
    expect_text(text, "a");
    assert(editor_undo_undo(undo, text));
    expect_text(text, "");
    editor_undo_destroy(undo);
    editor_text_destroy(text);
}

int main(void)
{
    test_typing_group();
    test_break();
    test_delete_runs();
    test_groups_stay_apart();
    test_replace_and_undo();
    test_modified_state();
    test_redo_cleared();
    printf("PASS: editor undo tests\n");
    return 0;
}
