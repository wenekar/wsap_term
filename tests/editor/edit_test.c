#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "editor/edit.h"

static const struct editor_language c_language = {
    .name = "c",
    .comments = {.line = "//", .block_start = "/*", .block_end = "*/"},
    .quotes = "\"'",
    .indent = "    ",
};

static void test_bracket_match(void)
{
    const char text[] = "f(a[b]c)";
    size_t length = sizeof(text) - 1;
    size_t match = 0;
    assert(editor_bracket_match(text, length, &c_language, 1, &match));
    assert(match == 7);
    assert(editor_bracket_match(text, length, &c_language, 7, &match));
    assert(match == 1);
    assert(editor_bracket_match(text, length, &c_language, 3, &match));
    assert(match == 5);

    /* No pair. */
    assert(!editor_bracket_match("(a", 2, &c_language, 0, &match));
    /* Not a bracket. */
    assert(!editor_bracket_match("abc", 3, &c_language, 1, &match));
    /* The caret is past the end. */
    assert(!editor_bracket_match("(a)", 3, &c_language, 5, &match));
}

/* A bracket inside a quoted string does not pair with one outside it. */
static void test_bracket_match_skips_quotes(void)
{
    const char text[] = "f(\"(\")";
    size_t length = sizeof(text) - 1;
    size_t match = 0;
    assert(editor_bracket_match(text, length, &c_language, 1, &match));
    assert(match == 5);
}

static void test_bracket_kind(void)
{
    assert(editor_bracket_kind('{') == EDITOR_BRACKET_BRACE);
    assert(editor_bracket_kind(']') == EDITOR_BRACKET_SQUARE);
    assert(editor_bracket_kind('x') == EDITOR_BRACKET_NONE);
    assert(editor_bracket_closes(EDITOR_BRACKET_BRACE, '}'));
    assert(!editor_bracket_closes(EDITOR_BRACKET_BRACE, ')'));
    assert(editor_bracket_close(EDITOR_BRACKET_PAREN) == ')');
    assert(editor_bracket_open(EDITOR_BRACKET_SQUARE) == '[');
}

static void test_auto_close_pair(void)
{
    struct editor_auto_close_result result =
        editor_auto_close("", 0, &c_language, 0, '(');
    assert(result.action == EDITOR_AUTO_CLOSE_PAIR);
    assert(result.close == ')');

    /* A closing character that is already at the caret inserts nothing. */
    result = editor_auto_close("()", 2, &c_language, 1, ')');
    assert(result.action == EDITOR_AUTO_CLOSE_SKIP);

    /* A closing character elsewhere stays as typed. */
    result = editor_auto_close("(a)", 3, &c_language, 1, ')');
    assert(result.action == EDITOR_AUTO_CLOSE_NONE);

    /* A quote pairs. */
    result = editor_auto_close("", 0, &c_language, 0, '"');
    assert(result.action == EDITOR_AUTO_CLOSE_PAIR);
    assert(result.close == '"');

    /* A letter does nothing. */
    result = editor_auto_close("x", 1, &c_language, 1, 'a');
    assert(result.action == EDITOR_AUTO_CLOSE_NONE);
}

static void test_smart_indent(void)
{
    char indent[16] = {0};
    size_t length = editor_smart_indent("if (x) {\n", 9, &c_language, 9, indent,
                                        sizeof(indent));
    assert(length == 4);
    assert(memcmp(indent, "    ", 4) == 0);

    /* A closing bracket removes one level from the line that it starts. */
    const char closing[] = "{\n        y;\n    }\n";
    length = editor_smart_indent(closing, sizeof(closing) - 1, &c_language,
                                 sizeof(closing) - 1, indent, sizeof(indent));
    assert(length == 0);

    /* A line of code keeps its indentation. */
    length = editor_smart_indent("  a;\n", 5, &c_language, 5, indent, sizeof(indent));
    assert(length == 2);
    assert(memcmp(indent, "  ", 2) == 0);

    /* The count comes back without a buffer. */
    assert(editor_smart_indent("if (x) {\n", 9, &c_language, 9, NULL, 0) == 4);
}

static void test_comment_start(void)
{
    const char text[] = "int x; // note\n";
    size_t length = sizeof(text) - 1;
    assert(editor_comment_start(text, length, &c_language, 0) == 7);
    /* A token inside a string does not count. */
    const char quoted[] = "a = \"//\";\n";
    assert(editor_comment_start(quoted, sizeof(quoted) - 1, &c_language, 0) ==
           sizeof(quoted) - 2);
    /* A language without a line comment gives the line length. */
    const struct editor_language plain = {.name = "plain"};
    assert(editor_comment_start(text, length, &plain, 0) == 14);
}

int main(void)
{
    test_bracket_match();
    test_bracket_match_skips_quotes();
    test_bracket_kind();
    test_auto_close_pair();
    test_smart_indent();
    test_comment_start();
    printf("editor edit tests passed\n");
    return 0;
}
