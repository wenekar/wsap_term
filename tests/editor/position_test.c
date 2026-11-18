#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>

#include "editor/position.h"
#include "editor/text.h"

static struct editor_position position(const struct editor_text *text,
                                       uint64_t offset, enum editor_column column)
{
    struct editor_position result;
    assert(editor_text_position(text, offset, column, &result));
    return result;
}

static uint64_t offset_at(const struct editor_text *text,
                          struct editor_position position,
                          enum editor_column column)
{
    uint64_t offset = 0;
    assert(editor_text_offset_at(text, position, column, &offset));
    return offset;
}

static void test_ascii(void)
{
    struct editor_text *text = editor_text_create();
    assert(editor_text_set(text, "abc", 3));
    for (uint64_t offset = 0; offset <= 3; offset++) {
        struct editor_position result;
        assert(editor_text_position(text, offset, EDITOR_COLUMN_UTF8, &result));
        assert(result.line == 0 && result.character == (uint32_t)offset);
        assert(editor_text_position(text, offset, EDITOR_COLUMN_UTF16, &result));
        assert(result.line == 0 && result.character == (uint32_t)offset);
        assert(editor_text_position(text, offset, EDITOR_COLUMN_UTF32, &result));
        assert(result.line == 0 && result.character == (uint32_t)offset);
        assert(offset_at(text, (struct editor_position){ .line = 0, .character = (uint32_t)offset },
                         EDITOR_COLUMN_UTF16) == offset);
    }
    struct editor_position position_out;
    assert(!editor_text_position(text, 4, EDITOR_COLUMN_UTF16, &position_out));
    uint64_t offset = 0;
    assert(!editor_text_offset_at(text, (struct editor_position){ .line = 0, .character = 4 },
                                  EDITOR_COLUMN_UTF16, &offset));
    assert(!editor_text_offset_at(text, (struct editor_position){ .line = 1, .character = 0 },
                                  EDITOR_COLUMN_UTF16, &offset));
    editor_text_destroy(text);
}

static void test_two_byte_code_point(void)
{
    /* "aé\nb": the e-acute takes two UTF-8 bytes and one code point. */
    struct editor_text *text = editor_text_create();
    assert(editor_text_set(text, "a\xC3\xA9\nb", 5));
    struct editor_position result = position(text, 3, EDITOR_COLUMN_UTF8);
    assert(result.line == 0 && result.character == 3);
    result = position(text, 3, EDITOR_COLUMN_UTF16);
    assert(result.line == 0 && result.character == 2);
    result = position(text, 3, EDITOR_COLUMN_UTF32);
    assert(result.line == 0 && result.character == 2);
    assert(offset_at(text, (struct editor_position){ .line = 0, .character = 2 },
                     EDITOR_COLUMN_UTF16) == 3);
    assert(offset_at(text, (struct editor_position){ .line = 0, .character = 2 },
                     EDITOR_COLUMN_UTF32) == 3);
    result = position(text, 4, EDITOR_COLUMN_UTF16);
    assert(result.line == 1 && result.character == 0);
    editor_text_destroy(text);
}

static void test_surrogate_pair(void)
{
    /* "a😀b": U+1F600 takes two UTF-16 code units and one UTF-32 code point. */
    struct editor_text *text = editor_text_create();
    assert(editor_text_set(text, "a\xF0\x9F\x98\x80" "b", 6));
    struct editor_position result = position(text, 1, EDITOR_COLUMN_UTF16);
    assert(result.line == 0 && result.character == 1);
    result = position(text, 5, EDITOR_COLUMN_UTF8);
    assert(result.line == 0 && result.character == 5);
    result = position(text, 5, EDITOR_COLUMN_UTF16);
    assert(result.line == 0 && result.character == 3);
    result = position(text, 5, EDITOR_COLUMN_UTF32);
    assert(result.line == 0 && result.character == 2);
    result = position(text, 6, EDITOR_COLUMN_UTF16);
    assert(result.line == 0 && result.character == 4);
    result = position(text, 6, EDITOR_COLUMN_UTF32);
    assert(result.line == 0 && result.character == 3);

    /* An offset inside the sequence is not a character position. */
    assert(!editor_text_position(text, 2, EDITOR_COLUMN_UTF16, &result));
    assert(!editor_text_position(text, 3, EDITOR_COLUMN_UTF32, &result));
    assert(editor_text_position(text, 2, EDITOR_COLUMN_UTF8, &result));
    assert(result.character == 2);

    assert(offset_at(text, (struct editor_position){ .line = 0, .character = 3 },
                     EDITOR_COLUMN_UTF16) == 5);
    assert(offset_at(text, (struct editor_position){ .line = 0, .character = 2 },
                     EDITOR_COLUMN_UTF32) == 5);
    /* A column inside the surrogate pair is not a position. */
    uint64_t offset = 0;
    assert(!editor_text_offset_at(text, (struct editor_position){ .line = 0, .character = 2 },
                                  EDITOR_COLUMN_UTF16, &offset));
    assert(editor_text_offset_at(text, (struct editor_position){ .line = 0, .character = 1 },
                                 EDITOR_COLUMN_UTF32, &offset));
    assert(offset == 1);
    editor_text_destroy(text);
}

static void test_line_terminator(void)
{
    struct editor_text *text = editor_text_create();
    assert(editor_text_set(text, "a\r\nb", 4));
    struct editor_position result = position(text, 1, EDITOR_COLUMN_UTF16);
    assert(result.line == 0 && result.character == 1);
    result = position(text, 3, EDITOR_COLUMN_UTF16);
    assert(result.line == 1 && result.character == 0);
    assert(offset_at(text, (struct editor_position){ .line = 1, .character = 0 },
                     EDITOR_COLUMN_UTF16) == 3);
    assert(offset_at(text, (struct editor_position){ .line = 0, .character = 1 },
                     EDITOR_COLUMN_UTF16) == 1);
    uint64_t offset = 0;
    assert(!editor_text_offset_at(text, (struct editor_position){ .line = 0, .character = 2 },
                                  EDITOR_COLUMN_UTF16, &offset));
    editor_text_destroy(text);
}

static void test_invalid_bytes(void)
{
    /* A byte that is not UTF-8 counts as one character. */
    struct editor_text *text = editor_text_create();
    assert(editor_text_set(text, "a\xFF" "b", 3));
    struct editor_position result = position(text, 2, EDITOR_COLUMN_UTF16);
    assert(result.line == 0 && result.character == 2);
    result = position(text, 2, EDITOR_COLUMN_UTF32);
    assert(result.line == 0 && result.character == 2);
    assert(offset_at(text, (struct editor_position){ .line = 0, .character = 2 },
                     EDITOR_COLUMN_UTF16) == 2);
    editor_text_destroy(text);
}

static void test_round_trip(void)
{
    struct editor_text *text = editor_text_create();
    assert(editor_text_set(text, "a\xC3\xA9" "\n\xF0\x9F\x98\x80" ";", 10));
    for (uint64_t offset = 0; offset <= editor_text_length(text); offset++) {
        struct editor_position result;
        uint64_t back;
        if (!editor_text_position(text, offset, EDITOR_COLUMN_UTF16, &result))
            continue;
        assert(editor_text_offset_at(text, result, EDITOR_COLUMN_UTF16, &back));
        assert(back == offset);
        assert(editor_text_position(text, offset, EDITOR_COLUMN_UTF32, &result));
        assert(editor_text_offset_at(text, result, EDITOR_COLUMN_UTF32, &back));
        assert(back == offset);
    }
    editor_text_destroy(text);
}

int main(void)
{
    test_ascii();
    test_two_byte_code_point();
    test_surrogate_pair();
    test_line_terminator();
    test_invalid_bytes();
    test_round_trip();
    printf("PASS: editor position tests\n");
    return 0;
}
