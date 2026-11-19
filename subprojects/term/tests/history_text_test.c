#include "wsap_vtcore/wsap_vtcore.h"
#include "term_api.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wsap_vtcore *new_term(int cols)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = cols, .y = 2 },
        .scrollback = 100,
    });
    assert(term);
    return term;
}

static void feed(struct wsap_vtcore *term, const char *text)
{
    wsap_vtcore_feed(term, text, strlen(text), 0);
}

static void expect_history(struct wsap_vtcore *term, int rows, const char *expected)
{
    char *text = wsap_vtcore_history_text(term, rows);
    assert(text);
    if (strcmp(text, expected)) {
        fprintf(stderr, "history: got \"%s\" want \"%s\"\n", text, expected);
        assert(0);
    }
    free(text);
}

static void unicode_and_selection(void)
{
    struct wsap_vtcore *term = new_term(16);
    expect_history(term, 100, "");
    feed(term, "é👩‍💻界\r\nnext\r\nlast");
    expect_history(term, 100, "é👩‍💻界\n");
    expect_history(term, 0, "");
    wsap_vtcore_select_start(term, (struct wsap_vtcore_vector2i){0}, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_select_extend(term, (struct wsap_vtcore_vector2i){ .x = 4 }, false);
    char *before = wsap_vtcore_selection_text(term);
    assert(before);
    expect_history(term, 100, "é👩‍💻界\n");
    char *after = wsap_vtcore_selection_text(term);
    assert(after && !strcmp(before, after));
    free(before);
    free(after);
    feed(term, "\033[?1049h");
    expect_history(term, 100, "");
    feed(term, "\033[?1049l");
    expect_history(term, 100, "é👩‍💻界\n");
    wsap_vtcore_free(term);
}

static void limits_and_scrolled_view(void)
{
    struct wsap_vtcore *term = new_term(8);
    feed(term, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
    expect_history(term, 100, "one\ntwo\nthree\n");
    expect_history(term, 2, "two\nthree\n");
    wsap_vtcore_scroll(term, 2);
    expect_history(term, 100, "one\n");
    wsap_vtcore_scroll(term, 100);
    expect_history(term, 100, "");
    wsap_vtcore_scroll_to(term, 0);
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 12, .y = 2 }));
    expect_history(term, 100, "one\ntwo\nthree\n");
    wsap_vtcore_free(term);
}

static void wrapped_rows(void)
{
    struct wsap_vtcore *term = new_term(4);
    feed(term, "abcdefghi\r\nx\r\ny");
    expect_history(term, 100, "abcdefghi\n");
    expect_history(term, 2, "efghi\n");
    wsap_vtcore_free(term);

    term = new_term(4);
    feed(term, "abcdefghijklm");
    expect_history(term, 100, "abcdefgh");
    wsap_vtcore_free(term);
}

static void scrollback_limit(void)
{
    struct wsap_vtcore *term = new_term(8);
    feed(term, "1\r\n2\r\n3\r\n4\r\n5\r\n6");
    expect_history(term, 100, "1\n2\n3\n4\n");

    assert(!wsap_vtcore_set_scrollback_limit(NULL, 10));
    assert(!wsap_vtcore_set_scrollback_limit(term, -1));
    assert(!wsap_vtcore_set_scrollback_limit(term, WSAP_VTCORE_SCROLLBACK_MAX + 1));
    expect_history(term, 100, "1\n2\n3\n4\n");

    /* A lower limit keeps the newest lines and clamps a scrolled view. */
    wsap_vtcore_scroll(term, 4);
    assert(wsap_vtcore_scrollback(term).offset == 4);
    assert(wsap_vtcore_select_start(term, (struct wsap_vtcore_vector2i){0}, false,
                                    WSAP_VTCORE_SELECT_LINE));
    assert(wsap_vtcore_set_scrollback_limit(term, 2));
    assert(!wsap_vtcore_has_selection(term));
    struct wsap_vtcore_scrollback sb = wsap_vtcore_scrollback(term);
    assert(sb.total == 2 && sb.offset <= 2);
    wsap_vtcore_scroll_to(term, 0);
    expect_history(term, 100, "3\n4\n");
    feed(term, "\r\n7");
    expect_history(term, 100, "4\n5\n");

    /* Zero keeps no history. Lines that scroll off are gone. */
    assert(wsap_vtcore_set_scrollback_limit(term, 0));
    assert(wsap_vtcore_scrollback(term).total == 0);
    feed(term, "\r\n8\r\n9");
    assert(wsap_vtcore_scrollback(term).total == 0);
    expect_history(term, 100, "");

    /* A higher limit keeps new lines again. The same limit is a success. */
    assert(wsap_vtcore_set_scrollback_limit(term, WSAP_VTCORE_SCROLLBACK_MAX));
    assert(wsap_vtcore_set_scrollback_limit(term, WSAP_VTCORE_SCROLLBACK_MAX));
    feed(term, "\r\na\r\nb");
    expect_history(term, 100, "8\n9\n");
    wsap_vtcore_free(term);
}

struct writer {
    struct wsap_vtcore *term;
    _Atomic bool done;
};

static const char unicode_line[] = "é👩‍💻界\r\n";
static const char history_line[] = "é👩‍💻界\n";

static void *write_history(void *userdata)
{
    struct writer *writer = userdata;
    for (int i = 0; i < 1000; i++)
        feed(writer->term, unicode_line);
    atomic_store(&writer->done, true);
    return NULL;
}

static void concurrent_history(void)
{
    struct wsap_vtcore *term = new_term(16);
    struct writer writer = { .term = term };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, write_history, &writer) == 0);
    do {
        char *text = wsap_vtcore_history_text(term, 10);
        assert(text);
        size_t n = strlen(text);
        size_t line_len = sizeof(history_line) - 1;
        assert(n % line_len == 0 && n / line_len <= 10);
        for (size_t offset = 0; offset < n; offset += line_len)
            assert(!memcmp(text + offset, history_line, line_len));
        free(text);
    } while (!atomic_load(&writer.done));
    assert(pthread_join(thread, NULL) == 0);
    wsap_vtcore_free(term);
}

static void multi_line_batch_history(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 10, .y = 2 },
        .scrollback = 200,
    });
    assert(term);
    for (int i = 0; i < 150; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "line%03d\r\n", i);
        feed(term, buf);
    }
    char *text = wsap_vtcore_history_text(term, 150);
    assert(text);
    for (int i = 0; i < 149; i++) {
        char expected[16];
        snprintf(expected, sizeof(expected), "line%03d\n", i);
        char *found = strstr(text, expected);
        assert(found);
    }
    free(text);
    wsap_vtcore_free(term);
}

/* A CRLF stream longer than 1024 bytes takes the vector line scan. The varied
 * line lengths walk each "\r\n" pair past every offset of a 64-byte chunk. */
static void crlf_stream_lines(void)
{
    static const int lens[] = { 63, 62, 60, 45, 33, 61, 59, 47, 31, 20 };
    const int kinds = (int)(sizeof(lens) / sizeof(lens[0]));
    struct wsap_vtcore *term = new_term(80);
    char buf[2048], want[2048];
    int n = 0, lines = 0;
    while (n < (int)sizeof(buf) - 80) {
        int len = lens[lines % kinds];
        for (int j = 0; j < len; j++)
            buf[n++] = (char)('a' + (lines + j) % 26);
        buf[n++] = '\r';
        buf[n++] = '\n';
        lines++;
    }
    /* The last line and the empty row below it stay on the two screen rows. */
    int w = 0;
    for (int k = 0; k < lines - 1; k++) {
        int len = lens[k % kinds];
        for (int j = 0; j < len; j++)
            want[w++] = (char)('a' + (k + j) % 26);
        want[w++] = '\n';
    }
    want[w] = '\0';
    wsap_vtcore_feed(term, buf, (size_t)n, 0);
    expect_history(term, 200, want);
    wsap_vtcore_free(term);
}

static void scrollback_zero_option(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 10, .y = 2 },
        .scrollback = 0,
    });
    assert(term);
    feed(term, "1\r\n2\r\n3\r\n4\r\n5\r\n6");
    assert(wsap_vtcore_scrollback(term).total == 0);
    expect_history(term, 100, "");
    wsap_vtcore_free(term);
}

static void scrollback_small_option(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 10, .y = 2 },
        .scrollback = 3,
    });
    assert(term);
    feed(term, "1\r\n2\r\n3\r\n4\r\n5\r\n6");
    assert(wsap_vtcore_scrollback(term).total == 3);
    expect_history(term, 100, "2\n3\n4\n");
    wsap_vtcore_free(term);
}

/* A fixed random stream keeps the test the same on every run. */
static uint64_t next_random(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* The text of a row range must be the same bytes as those rows of the whole
 * history text. The rows of a soft-wrapped line join with no newline, so the
 * text of a row starts where the text of the row before it ends. */
static void expect_row_slices(struct wsap_vtcore *term)
{
    int rows = wsap_vtcore_history_rows(term);
    char *whole = wsap_vtcore_history_text(term, rows);
    assert(whole);
    size_t len = strlen(whole);
    size_t *start = malloc(((size_t)rows + 1) * sizeof(*start));
    assert(start);
    start[0] = 0;
    for (int row = 0; row < rows; row++) {
        char *one = wsap_vtcore_history_text_range(term, row, 1, NULL);
        assert(one);
        size_t n = strlen(one);
        size_t at = start[row];
        if (n > len - at || memcmp(whole + at, one, n)) {
            fprintf(stderr, "row %d: got \"%s\", the whole text has \"%.*s\"\n",
                    row, one, (int)(len - at), whole + at);
            assert(0);
        }
        start[row + 1] = at + n;
        wsap_vtcore_text_free(one);
    }
    /* The row texts hold the whole text, each row once. */
    assert(start[rows] == len);
    for (int first = 0; first < rows; first++) {
        for (int count = 1; first + count <= rows; count++) {
            char *got = wsap_vtcore_history_text_range(term, first, count, NULL);
            assert(got);
            size_t want = start[first + count] - start[first];
            if (strlen(got) != want || memcmp(got, whole + start[first], want)) {
                fprintf(stderr, "rows %d..%d: got \"%s\", want \"%.*s\"\n",
                        first, first + count, got, (int)want, whole + start[first]);
                assert(0);
            }
            wsap_vtcore_text_free(got);
        }
    }
    free(start);
    wsap_vtcore_text_free(whole);
}

/* A logical line of the whole history text must hold the text of exactly the
 * rows that the wrap flags give for that line. */
static void expect_line_rows(struct wsap_vtcore *term)
{
    int rows = wsap_vtcore_history_rows(term);
    if (rows <= 0)
        return;
    bool *wrapped = calloc((size_t)rows, sizeof(*wrapped));
    assert(wrapped);
    char *whole = wsap_vtcore_history_text_range(term, 0, rows, wrapped);
    assert(whole);
    size_t len = strlen(whole), pos = 0;
    int row = 0;
    while (row < rows && pos < len) {
        int line_rows = 1;
        while (row + line_rows < rows && wrapped[row + line_rows - 1])
            line_rows++;
        const char *newline = memchr(whole + pos, '\n', len - pos);
        size_t bytes = newline ? (size_t)(newline - (whole + pos)) + 1 : len - pos;
        char *got = wsap_vtcore_history_text_range(term, row, line_rows, NULL);
        assert(got);
        if (strlen(got) != bytes || memcmp(got, whole + pos, bytes)) {
            fprintf(stderr, "line at row %d of %d rows: got \"%s\", want \"%.*s\"\n",
                    row, line_rows, got, (int)bytes, whole + pos);
            assert(0);
        }
        wsap_vtcore_text_free(got);
        pos += bytes;
        row += line_rows;
    }
    /* The lines hold the whole text and every row. */
    assert(row == rows);
    assert(pos == len);
    free(wrapped);
    wsap_vtcore_text_free(whole);
}

/* A row of a line with wide cells loses its last column when the next glyph
 * does not fit, so the cell count alone does not give the row count. A row
 * number must still map to the same part of the line in a range read and in
 * the whole history text. */
static void wide_row_count(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 3, .y = 10 },
        .scrollback = 100,
    });
    assert(term);
    /* Three wide cells take three rows of three columns, not two. */
    feed(term, "\u4e2d\u4e2d\u4e2d\r\n");
    for (int i = 0; i < 16; i++) {
        char line[8];
        snprintf(line, sizeof(line), "%c\r\n", 'a' + i);
        feed(term, line);
    }
    assert(wsap_vtcore_history_rows(term) == 10);
    expect_history(term, 100, "\u4e2d\u4e2d\u4e2d\na\nb\nc\nd\ne\nf\ng\n");
    expect_row_slices(term);
    wsap_vtcore_free(term);
}

/* A narrower resize rewraps the rows of a line with wide cells. The range
 * text of the rows must stay a slice of the whole history text. */
static void wide_rewrap_rows(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 6, .y = 3 },
        .scrollback = 100,
    });
    assert(term);
    /* A wide cell at a row end, a combining mark, and a tab. */
    feed(term, "\u4e2d\u4e2d\u4e2da\xcc\x81\t \u4e2d\r\n");
    for (int i = 0; i < 8; i++) {
        char line[16];
        snprintf(line, sizeof(line), "line%d\r\n", i);
        feed(term, line);
    }
    expect_line_rows(term);
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 3, .y = 5 }));
    expect_line_rows(term);
    wsap_vtcore_free(term);
}

/* A random stream of wide cells, combining marks, tabs, spaces, and line ends,
 * with resizes and eviction. */
static void wide_resize_stream(void)
{
    static const char *const pieces[] = {
        "\u4e2d", "e\xcc\x81", "\t", "   ", "\r\n", "x", "\u4e2d\u6587", " ",
        "\u4e2d\u4e2d", "abc", "\r\n\r\n", "\u4e2d ", " \u4e2d", "\t\u4e2d",
    };
    const int kinds = (int)(sizeof(pieces) / sizeof(pieces[0]));
    uint64_t state = UINT64_C(0x5eed5eed) * UINT64_C(0x9E3779B97F4A7C15) + 1;
    for (int trial = 0; trial < 32; trial++) {
        int cols = 1 + (int)(next_random(&state) % 120u);
        int rows = 1 + (int)(next_random(&state) % 24u);
        struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
            .size = { .x = cols, .y = rows },
            .scrollback = 400,
        });
        assert(term);
        char buf[4096];
        size_t n = 0;
        int count = 20 + (int)(next_random(&state) % 400u);
        for (int i = 0; i < count; i++) {
            const char *piece = pieces[next_random(&state) % (uint64_t)kinds];
            size_t len = strlen(piece);
            if (n + len > sizeof(buf) - 16) {
                wsap_vtcore_feed(term, buf, n, 0);
                n = 0;
            }
            memcpy(buf + n, piece, len);
            n += len;
        }
        wsap_vtcore_feed(term, buf, n, 0);
        int resizes = 1 + (int)(next_random(&state) % 2u);
        for (int r = 0; r < resizes; r++) {
            int next_cols = 1 + (int)(next_random(&state) % 120u);
            int next_rows = 1 + (int)(next_random(&state) % 60u);
            assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){
                .x = next_cols, .y = next_rows }));
        }
        expect_line_rows(term);
        wsap_vtcore_free(term);
    }
}

static void expect_range(struct wsap_vtcore *term, int first, int count,
                         const char *expected)
{
    char *text = wsap_vtcore_history_text_range(term, first, count, NULL);
    assert(text);
    if (strcmp(text, expected)) {
        fprintf(stderr, "rows %d+%d: got \"%s\" want \"%s\"\n", first, count,
                text, expected);
        assert(0);
    }
    wsap_vtcore_text_free(text);
}

/* A blank in the last column of a row that wraps is text of its line. When
 * the range ends at that row, the line goes on after the range, so the blank
 * stays. The two parts then join to the text of the whole line. */
static void wrapped_range_keeps_tail(void)
{
    struct wsap_vtcore *term = new_term(8);
    feed(term, "abcdefg hij\r\nx\r\ny");
    expect_history(term, 100, "abcdefg hij\n");
    expect_range(term, 0, 1, "abcdefg ");
    expect_range(term, 1, 1, "hij\n");
    expect_range(term, 0, 2, "abcdefg hij\n");
    wsap_vtcore_free(term);

    /* The last history row continues into the screen. */
    term = new_term(8);
    feed(term, "abcdefg hijklmn pq");
    assert(wsap_vtcore_history_rows(term) == 1);
    expect_range(term, 0, 1, "abcdefg ");
    expect_history(term, 100, "abcdefg ");
    wsap_vtcore_free(term);
}

int main(void)
{
    unicode_and_selection();
    limits_and_scrolled_view();
    wrapped_rows();
    wrapped_range_keeps_tail();
    scrollback_limit();
    scrollback_zero_option();
    scrollback_small_option();
    multi_line_batch_history();
    crlf_stream_lines();
    wide_row_count();
    wide_rewrap_rows();
    wide_resize_stream();
    concurrent_history();
    puts("PASS: history text");
    return 0;
}
