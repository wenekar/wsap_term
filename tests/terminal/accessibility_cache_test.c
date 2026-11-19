/* The accessibility document cache of one provider publish. The cache lives
 * only while the provider lock is held. A length call and every read of the
 * publish reuse the snapshot and the measure of the first lock call. */
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

static double elapsed_ms(struct timespec *start)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (double)(end.tv_sec - start->tv_sec) * 1000.0 +
           (double)(end.tv_nsec - start->tv_nsec) / 1000000.0;
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
        int n = snprintf(line, sizeof(line), "cost %06d\n", i);
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

static const struct ui_accessibility_node *build_node(
    struct terminal *content, struct ui_accessibility_tree *tree)
{
    ui_accessibility_tree_destroy(tree);
    assert(ui_accessibility_tree_build(&content->view.widget, tree));
    assert(tree->count > 0);
    return &tree->nodes[0];
}

/* Copies the whole document, then one run per line, as a publish does. The
 * provider lock covers the whole call. */
static void publish_read(const struct ui_accessibility_node *node,
                         double *out_whole_ms, double *out_runs_ms,
                         double *out_run_avg_ms)
{
    const struct ui_accessibility_text_provider *text = node->text;
    assert(text && text->lock && text->unlock);
    text->lock(text->instance);
    size_t length = ui_accessibility_node_text_length(node);
    char *whole = malloc(length + 1);
    char *run = malloc(length + 1);
    assert(whole && run);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t got = ui_accessibility_node_text_read(node, 0, whole, length);
    *out_whole_ms = elapsed_ms(&start);
    assert(got == length);
    whole[got] = '\0';
    size_t runs = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t at = 0; at < got; ) {
        const char *newline = memchr(whole + at, '\n', got - at);
        size_t len = newline ? (size_t)(newline - whole) + 1 - at : got - at;
        size_t n = ui_accessibility_node_text_read(node, at, run, len);
        assert(n == len);
        at += len;
        runs++;
    }
    *out_runs_ms = elapsed_ms(&start);
    *out_run_avg_ms = runs ? *out_runs_ms / (double)runs : 0.0;
    text->unlock(text->instance);
    free(run);
    free(whole);
}

/* The cost test: one run read must not take the history size into account,
 * and a publish at 20000 lines must stay far below one second. */
static void cost(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    struct ui_accessibility_tree tree = {0};
    double whole_ms = 0, runs_ms = 0, run_avg_ms = 0;

    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 1999);
    const struct ui_accessibility_node *node = build_node(content, &tree);
    publish_read(node, &whole_ms, &runs_ms, &run_avg_ms);
    double small_avg_ms = run_avg_ms;

    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 19999);
    node = build_node(content, &tree);
    publish_read(node, &whole_ms, &runs_ms, &run_avg_ms);
    printf("accessibility publish at 20000 lines: whole %.1f ms, "
           "runs %.1f ms, one run %.3f ms (2000 lines: %.3f ms)\n",
           whole_ms, runs_ms, run_avg_ms, small_avg_ms);
    fflush(stdout);
    assert(run_avg_ms < 0.015);
    assert(whole_ms + runs_ms < 500.0);

    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
}

/* Adds one read of each shape under one lock and checks that all of them
 * return the same bytes. */
static char *read_consistent(const struct ui_accessibility_node *node)
{
    const struct ui_accessibility_text_provider *text = node->text;
    text->lock(text->instance);
    size_t length = ui_accessibility_node_text_length(node);
    char *whole = malloc(length + 1);
    assert(whole);
    size_t got = ui_accessibility_node_text_read(node, 0, whole, length);
    whole[got] = '\0';
    assert(got == length);
    char *chunked = calloc(length + 1, 1);
    char *runs = calloc(length + 1, 1);
    assert(chunked && runs);
    size_t at = 0;
    while (at < length) {
        size_t want = length - at > 7 ? 7 : length - at;
        size_t n = ui_accessibility_node_text_read(node, at, chunked + at, want);
        assert(n == want);
        at += n;
    }
    at = 0;
    while (at < got) {
        const char *newline = memchr(whole + at, '\n', got - at);
        size_t len = newline ? (size_t)(newline - whole) + 1 - at : got - at;
        size_t n = ui_accessibility_node_text_read(node, at, runs + at, len);
        assert(n == len);
        at += len;
    }
    text->unlock(text->instance);
    assert(strcmp(chunked, whole) == 0);
    assert(strcmp(runs, whole) == 0);
    free(chunked);
    free(runs);
    return whole;
}

/* The document must hold the current history text after each change. A stale
 * cache would return the text of the state before the change. */
static void check_document(const char *label, struct terminal *content,
                           struct wsap_vtcore *term,
                           struct ui_accessibility_tree *tree)
{
    const struct ui_accessibility_node *node = build_node(content, tree);
    char *document = read_consistent(node);
    int rows = wsap_vtcore_history_rows(term);
    struct wsap_vtcore_scrollback scrollback = wsap_vtcore_scrollback(term);
    rows -= scrollback.offset;
    char *history = wsap_vtcore_history_text(term, rows);
    assert(history);
    bool prefix = strncmp(document, history, strlen(history)) == 0;
    if (!prefix)
        printf("%s: document does not start with the history text\n", label);
    assert(prefix);
    printf("%s: %zu document bytes, %zu history bytes\n", label,
           strlen(document), strlen(history));
    wsap_vtcore_text_free(history);
    free(document);
}

static void lifetime(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    struct ui_accessibility_tree tree = {0};

    assert(wsap_vtcore_resize(term,
                              (struct wsap_vtcore_vector2i){ .x = 80, .y = 24 }));
    feed_lines(term, 0, 299);
    check_document("after the feed", content, term, &tree);

    assert(wsap_vtcore_resize(term,
                              (struct wsap_vtcore_vector2i){ .x = 53, .y = 24 }));
    check_document("after the resize", content, term, &tree);

    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 99);
    check_document("after the edit", content, term, &tree);

    wsap_vtcore_scroll(term, 20);
    check_document("after the scroll", content, term, &tree);

    wsap_vtcore_scroll_to(term, 0);
    check_document("after the scroll back", content, term, &tree);

    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
}

/* The text build interval. A change inside the interval waits for the
 * timer, and the next build after the interval carries the new version. */
static void text_interval(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    struct ui_accessibility_tree tree = {0};

    feed_lines(term, 0, 9);
    const struct ui_accessibility_node *node = build_node(content, &tree);
    uint64_t first = node->text_version;
    feed_lines(term, 10, 19);
    node = build_node(content, &tree);
    assert(node->text_version == first);
    struct timespec delay = { .tv_nsec = 110000000 };
    nanosleep(&delay, NULL);
    node = build_node(content, &tree);
    assert(node->text_version != first);

    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
}

int main(void)
{
    cost();
    lifetime();
    text_interval();
    return 0;
}
