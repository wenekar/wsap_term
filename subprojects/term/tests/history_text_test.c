#include "wsap_vtcore/wsap_vtcore.h"
#include "term_api.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
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

int main(void)
{
    unicode_and_selection();
    limits_and_scrolled_view();
    wrapped_rows();
    scrollback_limit();
    concurrent_history();
    puts("PASS: history text");
    return 0;
}
