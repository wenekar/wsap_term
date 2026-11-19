#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "app/tabs/tabs.h"
#include "session/session.h"
#include "terminal/internal.h"
#include "ui/accessibility.h"
#include "ui/widget.h"

static struct terminal *active_terminal(struct test_app *app)
{
    struct window *w = test_terminal_window(app->app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    return content;
}

static void feed(struct wsap_vtcore *term, const char *text)
{
    assert(wsap_vtcore_feed(term, text, strlen(text), 0));
}

static void feed_lines(struct wsap_vtcore *term, int first, int last)
{
    char block[8192];
    size_t used = 0;
    for (int i = first; i <= last; i++) {
        char line[32];
        int n = snprintf(line, sizeof(line), "line %05d\n", i);
        assert(n > 0 && (size_t)n < sizeof(line));
        if (used + (size_t)n > sizeof(block)) {
            assert(wsap_vtcore_feed(term, block, used, 0));
            used = 0;
        }
        memcpy(block + used, line, (size_t)n);
        used += (size_t)n;
    }
    if (used)
        assert(wsap_vtcore_feed(term, block, used, 0));
}

static const struct ui_accessibility_node *build_node(struct terminal *content,
                                                      struct ui_accessibility_tree *tree)
{
    ui_accessibility_tree_destroy(tree);
    assert(ui_accessibility_tree_build(&content->view.widget, tree));
    assert(tree->count > 0);
    return &tree->nodes[0];
}

static char *read_document(const struct ui_accessibility_node *node, size_t *out_len)
{
    size_t length = ui_accessibility_node_text_length(node);
    char *text = malloc(length + 1);
    assert(text);
    size_t got = ui_accessibility_node_text_read(node, 0, text, length);
    text[got] = '\0';
    if (out_len)
        *out_len = got;
    return text;
}

static size_t count(const char *text, const char *needle)
{
    size_t n = 0;
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle))
        n++;
    return n;
}

static void check_history_prefix(struct terminal *content,
                                 const struct ui_accessibility_node *node)
{
    struct wsap_vtcore *term = terminal_session(content)->term;
    int rows = wsap_vtcore_history_rows(term);
    struct wsap_vtcore_scrollback sb = wsap_vtcore_scrollback(term);
    rows -= sb.offset;
    char *history = wsap_vtcore_history_text(term, rows);
    assert(history);
    char *document = read_document(node, NULL);
    assert(strncmp(document, history, strlen(history)) == 0);
    free(document);
    wsap_vtcore_text_free(history);
}

int main(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct terminal *content = active_terminal(&app);
    struct wsap_vtcore *term = terminal_session(content)->term;
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    struct ui_accessibility_tree tree = {0};

    /* A1: a clear and a new set of lines rebuild the document. */
    feed_lines(term, 0, 99);
    const struct ui_accessibility_node *node = build_node(content, &tree);
    char *before = read_document(node, NULL);
    assert(strstr(before, "line 00099"));
    check_history_prefix(content, node);
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 39);
    node = build_node(content, &tree);
    size_t after_len = 0;
    char *after = read_document(node, &after_len);
    assert(after_len >= 40 * 11 - 1);
    assert(strstr(after, "line 00039"));
    assert(strstr(after, "line 00099") == NULL);
    check_history_prefix(content, node);
    free(before);
    free(after);

    /* A2: a full ring evicts lines and keeps the offsets correct. */
    assert(wsap_vtcore_set_scrollback_limit(term, 100));
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 299);
    node = build_node(content, &tree);
    before = read_document(node, NULL);
    check_history_prefix(content, node);
    feed_lines(term, 300, 349);
    node = build_node(content, &tree);
    after = read_document(node, &after_len);
    assert(strstr(after, "line 00349"));
    assert(strstr(after, "line 00000") == NULL);
    check_history_prefix(content, node);
    free(before);
    free(after);

    /* A3: a clear and new output do not count as appended lines. */
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    feed_lines(term, 0, 49);
    node = build_node(content, &tree);
    check_history_prefix(content, node);
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 9);
    node = build_node(content, &tree);
    after = read_document(node, &after_len);
    assert(count(after, "line 00000") == 1);
    check_history_prefix(content, node);
    free(after);

    /* A3: a narrower resize rewraps the lines. */
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 40, .y = 30 }));
    node = build_node(content, &tree);
    after = read_document(node, &after_len);
    assert(count(after, "line 00009") == 1);
    check_history_prefix(content, node);
    free(after);

    /* A4: wrapped lines map to the right rows. */
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 80, .y = 30 }));
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    char long_line[201];
    memset(long_line, 'x', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';
    for (int i = 0; i < 20; i++) {
        char line[256];
        int n = snprintf(line, sizeof(line), "%02d%s\n", i, long_line);
        assert(n > 0 && (size_t)n < sizeof(line));
        feed(term, line);
    }
    node = build_node(content, &tree);
    after = read_document(node, &after_len);
    char *chunked = malloc(after_len + 1);
    assert(chunked);
    size_t got = 0;
    while (got < after_len) {
        size_t want = after_len - got > 7 ? 7 : after_len - got;
        size_t n = ui_accessibility_node_text_read(node, got, chunked + got, want);
        assert(n == want);
        got += n;
    }
    chunked[got] = '\0';
    assert(strcmp(chunked, after) == 0);
    assert(count(after, "19") >= 1);
    check_history_prefix(content, node);
    free(chunked);
    free(after);

    /* A6: a scroll moves the view. The rows above the view stay in the
     * document once, and the view rows follow. */
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 199);
    node = build_node(content, &tree);
    before = read_document(node, NULL);
    assert(strstr(before, "line 00199"));
    wsap_vtcore_scroll(term, 20);
    node = build_node(content, &tree);
    after = read_document(node, NULL);
    assert(count(after, "line 00000") == 1);
    assert(count(after, "line 00150") == 1);
    check_history_prefix(content, node);
    char *scroll_chunked = malloc(strlen(after) + 1);
    assert(scroll_chunked);
    size_t scroll_len = strlen(after), scroll_got = 0;
    while (scroll_got < scroll_len) {
        size_t want = scroll_len - scroll_got > 7 ? 7 : scroll_len - scroll_got;
        size_t n = ui_accessibility_node_text_read(node, scroll_got,
                                                   scroll_chunked + scroll_got,
                                                   want);
        assert(n == want);
        scroll_got += n;
    }
    scroll_chunked[scroll_got] = '\0';
    assert(strcmp(scroll_chunked, after) == 0);
    free(scroll_chunked);
    wsap_vtcore_scroll_to(term, 0);
    free(before);
    free(after);

    /* A7: a content change without a caret move publishes the node. */
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed(term, "$ 10%\r");
    node = build_node(content, &tree);
    struct ui_accessibility_tree previous = {0};
    assert(ui_accessibility_tree_build(&content->view.widget, &previous));
    uint64_t terminal_id = tree.nodes[0].id;
    feed(term, "$ 20%\r");
    node = build_node(content, &tree);
    struct ui_accessibility_publish_set set = {0};
    assert(ui_accessibility_publish_set_build(&previous, &tree, &set));
    assert(ui_accessibility_publish_set_has(&set, terminal_id));
    ui_accessibility_publish_set_destroy(&set);
    ui_accessibility_tree_destroy(&previous);

    /* A5: a read costs the rows of its range, not the history. */
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 49999);
    node = build_node(content, &tree);
    size_t length = ui_accessibility_node_text_length(node);
    assert(length > 50000 * 10);
    char small[11];
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t copied = ui_accessibility_node_text_read(node, length / 2, small,
                                                    sizeof(small));
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
    assert(copied > 0);
    printf("a3: one 11-byte read at 50k lines: %.3f ms\n", ms);
    assert(ms < 1.5);

    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
    printf("a3: accessibility index cases passed\n");
    return 0;
}
