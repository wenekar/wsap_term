#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor/text.h"

static void expect_text(const struct editor_text *text, const char *expected)
{
    size_t length = strlen(expected);
    assert(editor_text_length(text) == length);
    char *copy = malloc(length + 1);
    assert(copy);
    assert(editor_text_copy(text, 0, length, copy) == length);
    assert(memcmp(copy, expected, length) == 0);
    free(copy);
}

static void test_insert_and_delete(void)
{
    struct editor_text *text = editor_text_create();
    assert(text);
    struct editor_line line;
    assert(editor_text_length(text) == 0);
    assert(editor_text_line_count(text) == 1);
    assert(editor_text_line(text, 0, &line));
    assert(line.start == 0 && line.length == 0 && line.end == 0);
    assert(!editor_text_line(text, 1, &line));

    assert(editor_text_insert(text, 0, "hello", 5));
    expect_text(text, "hello");
    assert(!editor_text_insert(text, 6, "x", 1));
    expect_text(text, "hello");

    assert(editor_text_insert(text, 5, " world", 6));
    expect_text(text, "hello world");
    assert(editor_text_insert(text, 0, ">> ", 3));
    expect_text(text, ">> hello world");
    assert(editor_text_delete(text, 3, 2));
    expect_text(text, ">> llo world");
    assert(editor_text_delete(text, 0, 3));
    expect_text(text, "llo world");
    assert(!editor_text_delete(text, 5, 100));
    expect_text(text, "llo world");
    assert(editor_text_replace(text, 0, 3, "HELLO", 5));
    expect_text(text, "HELLO world");
    assert(editor_text_replace(text, 5, 6, "", 0));
    expect_text(text, "HELLO");

    assert(editor_text_set(text, "fresh", 5));
    expect_text(text, "fresh");
    assert(editor_text_set(text, "", 0));
    expect_text(text, "");
    assert(editor_text_line_count(text) == 1);
    editor_text_destroy(text);
}

static void test_line_index(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_line line;
    uint32_t index;
    uint64_t column;

    assert(editor_text_set(text, "one\ntwo\nthree", 13));
    assert(editor_text_newline_count(text) == 2);
    assert(editor_text_line_count(text) == 3);
    assert(editor_text_line_ending(text) == EDITOR_LINE_LF);
    assert(editor_text_line(text, 0, &line));
    assert(line.start == 0 && line.length == 3 && line.end == 4);
    assert(editor_text_line(text, 1, &line));
    assert(line.start == 4 && line.length == 3 && line.end == 8);
    assert(editor_text_line(text, 2, &line));
    assert(line.start == 8 && line.length == 5 && line.end == 13);
    assert(!editor_text_line(text, 3, &line));

    assert(editor_text_locate(text, 0, &index, &column));
    assert(index == 0 && column == 0);
    assert(editor_text_locate(text, 3, &index, &column));
    assert(index == 0 && column == 3);
    assert(editor_text_locate(text, 4, &index, &column));
    assert(index == 1 && column == 0);
    assert(editor_text_locate(text, 6, &index, &column));
    assert(index == 1 && column == 2);
    assert(editor_text_locate(text, 13, &index, &column));
    assert(index == 2 && column == 5);
    assert(!editor_text_locate(text, 14, &index, &column));

    assert(editor_text_set(text, "a\n", 2));
    assert(editor_text_line_count(text) == 2);
    assert(editor_text_line(text, 0, &line));
    assert(line.start == 0 && line.length == 1 && line.end == 2);
    assert(editor_text_line(text, 1, &line));
    assert(line.start == 2 && line.length == 0 && line.end == 2);
    editor_text_destroy(text);
}

static void test_crlf(void)
{
    struct editor_text *text = editor_text_create();
    struct editor_line line;

    assert(editor_text_set(text, "a\r\nb\r\n", 6));
    assert(editor_text_crlf_count(text) == 2);
    assert(editor_text_newline_count(text) == 2);
    assert(editor_text_line_ending(text) == EDITOR_LINE_CRLF);
    assert(editor_text_line_count(text) == 3);
    assert(editor_text_line(text, 0, &line));
    assert(line.start == 0 && line.length == 1 && line.end == 3);
    assert(editor_text_line(text, 1, &line));
    assert(line.start == 3 && line.length == 1 && line.end == 6);
    assert(editor_text_line(text, 2, &line));
    assert(line.start == 6 && line.length == 0 && line.end == 6);

    /* The CRLF pair of a piece boundary still counts. */
    assert(editor_text_delete(text, 1, 1));
    expect_text(text, "a\nb\r\n");
    assert(editor_text_crlf_count(text) == 1);
    assert(editor_text_insert(text, 1, "\r", 1));
    expect_text(text, "a\r\nb\r\n");
    assert(editor_text_crlf_count(text) == 2);
    assert(editor_text_newline_count(text) == 2);
    assert(editor_text_insert(text, 3, "\r", 1));
    expect_text(text, "a\r\n\rb\r\n");
    assert(editor_text_crlf_count(text) == 2);
    assert(editor_text_delete(text, 3, 1));
    assert(editor_text_crlf_count(text) == 2);

    /* A lone CR is not a line break. */
    assert(editor_text_set(text, "x\ry", 3));
    assert(editor_text_crlf_count(text) == 0);
    assert(editor_text_newline_count(text) == 0);
    assert(editor_text_line_count(text) == 1);
    assert(editor_text_line_ending(text) == EDITOR_LINE_LF);
    editor_text_destroy(text);
}

static void test_many_pieces(void)
{
    struct editor_text *text = editor_text_create();
    size_t length = 0;
    char *bytes = malloc(20000);
    assert(bytes);
    for (int i = 0; i < 1000; i++)
        length += (size_t)snprintf(bytes + length, 20000 - length, "line %d\n", i);
    assert(editor_text_set(text, bytes, length));
    assert(editor_text_line_count(text) == 1001);
    struct editor_line line;
    for (uint32_t i = 0; i < 1000; i++) {
        assert(editor_text_line(text, i, &line));
        char expected[32];
        int size = snprintf(expected, sizeof(expected), "line %d", i);
        assert(line.length == (uint64_t)size);
        char copy[32];
        assert(editor_text_copy(text, line.start, line.length, copy) == line.length);
        assert(memcmp(copy, expected, (size_t)size) == 0);
    }
    free(bytes);
    editor_text_destroy(text);
}

struct model {
    char *bytes;
    size_t length;
    size_t capacity;
};

static void model_insert(struct model *model, size_t offset, const char *bytes,
                         size_t length)
{
    if (model->length + length > model->capacity) {
        size_t capacity = model->capacity ? model->capacity : 16;
        while (capacity < model->length + length)
            capacity *= 2;
        char *grown = realloc(model->bytes, capacity);
        assert(grown);
        model->bytes = grown;
        model->capacity = capacity;
    }
    memmove(model->bytes + offset + length, model->bytes + offset,
            model->length - offset);
    memcpy(model->bytes + offset, bytes, length);
    model->length += length;
}

static void model_delete(struct model *model, size_t offset, size_t length)
{
    memmove(model->bytes + offset, model->bytes + offset + length,
            model->length - offset - length);
    model->length -= length;
}

static uint64_t model_crlf(const struct model *model)
{
    uint64_t count = 0;
    for (size_t i = 1; i < model->length; i++) {
        if (model->bytes[i - 1] == '\r' && model->bytes[i] == '\n')
            count++;
    }
    return count;
}

static uint64_t model_newlines(const struct model *model)
{
    uint64_t count = 0;
    for (size_t i = 0; i < model->length; i++) {
        if (model->bytes[i] == '\n')
            count++;
    }
    return count;
}

static void model_line(const struct model *model, uint32_t index, uint64_t *start,
                       uint64_t *length)
{
    uint64_t offset = 0;
    for (uint32_t line = 0; line < index; line++) {
        while (offset < model->length && model->bytes[offset] != '\n')
            offset++;
        offset++;
    }
    uint64_t end = offset;
    while (end < model->length && model->bytes[end] != '\n')
        end++;
    *start = offset;
    *length = end - offset;
    if (end < model->length && end > offset && model->bytes[end - 1] == '\r')
        (*length)--;
}

static void model_locate(const struct model *model, uint64_t offset,
                         uint32_t *line, uint64_t *column)
{
    uint64_t start = 0;
    uint32_t index = 0;
    for (uint64_t i = 0; i < offset; i++) {
        if (model->bytes[i] == '\n') {
            index++;
            start = i + 1;
        }
    }
    *line = index;
    *column = offset - start;
}

static void model_check(const struct editor_text *text, const struct model *model)
{
    assert(editor_text_length(text) == model->length);
    char *copy = malloc(model->length + 1);
    assert(copy);
    assert(editor_text_copy(text, 0, model->length, copy) == model->length);
    assert(memcmp(copy, model->bytes, model->length) == 0);
    free(copy);
    assert(editor_text_newline_count(text) == model_newlines(model));
    assert(editor_text_crlf_count(text) == model_crlf(model));
    assert(editor_text_line_count(text) == model_newlines(model) + 1);
    uint64_t count = model_newlines(model) + 1;
    uint64_t step = count > 64 ? count / 64 : 1;
    for (uint64_t i = 0; i < count; i += step) {
        struct editor_line line;
        uint64_t start;
        uint64_t length;
        assert(editor_text_line(text, (uint32_t)i, &line));
        model_line(model, (uint32_t)i, &start, &length);
        assert(line.start == start);
        assert(line.length == length);
    }
    step = model->length > 128 ? model->length / 128 : 1;
    for (uint64_t offset = 0; offset <= model->length; offset += step) {
        uint32_t index;
        uint64_t column;
        uint32_t expected_line;
        uint64_t expected_column;
        assert(editor_text_locate(text, offset, &index, &column));
        model_locate(model, offset, &expected_line, &expected_column);
        assert(index == expected_line);
        assert(column == expected_column);
    }
}

static void test_random_edits(void)
{
    static const char alphabet[] = "ab\t\r\n";
    struct editor_text *text = editor_text_create();
    struct model model = {0};
    uint32_t state = 12345;
    assert(text);
    for (int step = 0; step < 800; step++) {
        state = state * 1664525u + 1013904223u;
        size_t offset = model.length ? (state >> 8) % (model.length + 1) : 0;
        if ((state & 7) < 5) {
            state = state * 1664525u + 1013904223u;
            size_t length = 1 + (state >> 8) % 6;
            char bytes[8];
            for (size_t i = 0; i < length; i++) {
                state = state * 1664525u + 1013904223u;
                bytes[i] = alphabet[(state >> 8) % (sizeof(alphabet) - 1)];
            }
            assert(editor_text_insert(text, offset, bytes, length));
            model_insert(&model, offset, bytes, length);
        } else {
            size_t length = model.length ? 1 + (state >> 12) % model.length : 0;
            if (length > model.length - offset)
                length = model.length - offset;
            assert(editor_text_delete(text, offset, length));
            model_delete(&model, offset, length);
        }
        model_check(text, &model);
    }
    free(model.bytes);
    editor_text_destroy(text);
}

int main(void)
{
    test_insert_and_delete();
    test_line_index();
    test_crlf();
    test_many_pieces();
    test_random_edits();
    printf("PASS: editor text tests\n");
    return 0;
}
