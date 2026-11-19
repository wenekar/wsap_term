/* The accessibility document cache of one provider publish. The cache lives
 * only while the provider lock is held. A length call and every read of the
 * publish reuse the snapshot and the measure of the first lock call. */
#include <assert.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
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
#include <wsap_vtcore/unicode/grapheme.h>
#include <wsap_vtcore/unicode/utf8.h>

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

/* The cost test: the run reads of one publish must cost about as much as
 * one whole read, because both copy the same bytes. A run read that measures
 * the document again costs O(history) each, and the runs then cost many
 * times the whole read. The test compares the two reads of one publish, so
 * load and sanitizers change both alike. */
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
    assert(runs_ms < 4.0 * whole_ms);

    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
}

static char *read_consistent(const struct ui_accessibility_node *node);

static int compare_ms(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The cost of one new line at a full scrollback ring. Each new line evicts
 * the oldest line. The provider lock must then drop that line from the
 * index and add the new one, at a cost that does not grow with the history.
 * A rebuild of the index reads the whole history, as a whole read does. The
 * test compares the two in one process, so load and sanitizers change both
 * alike. */
static void full_ring_cost(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    assert(wsap_vtcore_set_scrollback_limit(term, 20000));
    struct ui_accessibility_tree tree = {0};

    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_lines(term, 0, 21999);
    const struct ui_accessibility_node *node = build_node(content, &tree);
    const struct ui_accessibility_text_provider *text = node->text;
    double whole_ms = 0, runs_ms = 0, run_avg_ms = 0;
    publish_read(node, &whole_ms, &runs_ms, &run_avg_ms);

    enum { REPS = 9 };
    double lock_ms[REPS];
    for (int i = 0; i < REPS; i++) {
        feed_lines(term, 22000 + i, 22000 + i);
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        text->lock(text->instance);
        text->unlock(text->instance);
        lock_ms[i] = elapsed_ms(&start);
    }
    qsort(lock_ms, REPS, sizeof(lock_ms[0]), compare_ms);
    double lock_median = lock_ms[REPS / 2];

    /* The document still holds the newest line. */
    char *document = read_consistent(node);
    assert(strstr(document, "cost 022008\n"));
    assert(!strstr(document, "cost 000000"));
    free(document);
    printf("full ring of 20000 lines: lock after one new line %.3f ms "
           "(median of %d), whole read %.1f ms\n", lock_median, REPS,
           whole_ms);
    fflush(stdout);
    assert(lock_median < 0.05 * whole_ms);

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

/* The document must start with the current history text. A stale cache
 * would return the text of the state before the change. */
static void check_text(const char *label,
                       const struct ui_accessibility_node *node,
                       struct wsap_vtcore *term)
{
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

/* Builds the node again and checks its document after a change. */
static void check_document(const char *label, struct terminal *content,
                           struct wsap_vtcore *term,
                           struct ui_accessibility_tree *tree)
{
    check_text(label, build_node(content, tree), term);
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

struct text {
    char *bytes;
    size_t len;
    size_t cap;
};

static void text_add(struct text *t, const char *bytes, size_t len)
{
    if (t->len + len + 1 > t->cap) {
        t->cap = (t->len + len + 1) * 2;
        t->bytes = realloc(t->bytes, t->cap);
        assert(t->bytes);
    }
    memcpy(t->bytes + t->len, bytes, len);
    t->len += len;
    t->bytes[t->len] = '\0';
}

/* Drops the trailing blanks of the line that starts at `line_start`. */
static void text_trim(struct text *t, size_t line_start)
{
    while (t->len > line_start && (unsigned char)t->bytes[t->len - 1] <= ' ')
        t->len--;
    if (t->bytes)
        t->bytes[t->len] = '\0';
}

static bool cell_blank(const struct wsap_vtcore_snapshot *snapshot,
                       const struct wsap_vtcore_cell *cell)
{
    if (!cell->width || (cell->attrs & WSAP_VTCORE_ATTR_WRAP_PAD))
        return true;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = cell->text ? wsap_vtcore_snapshot_cluster(snapshot, cell->text, cps,
                                                      WSAP_VTCORE_GRAPHEME_MAX_LEN)
                       : 0;
    for (int i = 0; i < n; i++)
        if (cps[i] != ' ')
            return false;
    return true;
}

static void cell_text(struct text *t, const struct wsap_vtcore_snapshot *snapshot,
                      const struct wsap_vtcore_cell *cell)
{
    if (!cell->width || (cell->attrs & WSAP_VTCORE_ATTR_WRAP_PAD))
        return;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = cell->text ? wsap_vtcore_snapshot_cluster(snapshot, cell->text, cps,
                                                      WSAP_VTCORE_GRAPHEME_MAX_LEN)
                       : 0;
    if (n <= 0) {
        text_add(t, " ", 1);
        return;
    }
    for (int i = 0; i < n; i++) {
        char encoded[4];
        int bytes = wsap_vtcore_utf8_encode(cps[i], encoded);
        if (bytes > 0)
            text_add(t, encoded, (size_t)bytes);
    }
}

/* The document by its rule, from the term alone: the range text of every
 * history row, then the rows of a new live snapshot. A line drops its
 * trailing blanks. One '\n' ends each line but the last. Call it with the
 * term lock held, or with no writer. */
static struct text term_document(struct wsap_vtcore *term)
{
    struct text t = {0};
    text_add(&t, "", 0);
    int rows = wsap_vtcore_history_rows(term);
    bool open = false;
    if (rows > 0) {
        bool *wrapped = calloc((size_t)rows, sizeof(*wrapped));
        assert(wrapped);
        char *history = wsap_vtcore_history_text_range(term, 0, rows, wrapped);
        assert(history);
        text_add(&t, history, strlen(history));
        open = wrapped[rows - 1];
        wsap_vtcore_text_free(history);
        free(wrapped);
    }
    /* The last history line goes on in the screen. */
    size_t line_start = t.len;
    if (open)
        while (line_start > 0 && t.bytes[line_start - 1] != '\n')
            line_start--;
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(snapshot && wsap_vtcore_snapshot_update_live(snapshot, term));
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(snapshot);
    int last = -1;
    for (int y = 0; y < view->size.y; y++) {
        const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
        for (int x = 0; cells && x < view->size.x; x++)
            if (!cell_blank(snapshot, &cells[x]))
                last = y;
    }
    if (view->cursor.visible && view->cursor.pos.y > last &&
        view->cursor.pos.y < view->size.y)
        last = view->cursor.pos.y;
    for (int y = 0; y <= last; y++) {
        const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(snapshot, y);
        for (int x = 0; cells && x < view->size.x; x++)
            cell_text(&t, snapshot, &cells[x]);
        if (y < last && wsap_vtcore_snapshot_row_wrapped(snapshot, y))
            continue;
        text_trim(&t, line_start);
        if (y < last)
            text_add(&t, "\n", 1);
        line_start = t.len;
    }
    if (last < 0)
        text_trim(&t, line_start);
    wsap_vtcore_snapshot_free(snapshot);
    return t;
}

static void feed_wide(struct wsap_vtcore *term, uint64_t *state, int lines)
{
    static const char *const pieces[] = {
        "\u4e2d", "e\xcc\x81", "x", "\u4e2d\u6587", "abc", " ", "  ",
        "\xf0\x9f\x98\x80",
    };
    const uint64_t kinds = sizeof(pieces) / sizeof(pieces[0]);
    for (int i = 0; i < lines; i++) {
        char line[2048];
        size_t n = (size_t)snprintf(line, sizeof(line), "w%04d:", i);
        *state ^= *state << 13;
        *state ^= *state >> 7;
        *state ^= *state << 17;
        int count = 1 + (int)(*state % 90u);
        for (int k = 0; k < count; k++) {
            *state ^= *state << 13;
            *state ^= *state >> 7;
            *state ^= *state << 17;
            const char *piece = pieces[*state % kinds];
            memcpy(line + n, piece, strlen(piece));
            n += strlen(piece);
        }
        memcpy(line + n, "\r\n", 3);
        feed(term, line);
    }
}

enum change {
    CHANGE_CLEAR,
    CHANGE_LINES,
    CHANGE_NARROW,
    CHANGE_WIDE,
    CHANGE_SCROLL,
    CHANGE_EVICT,
    CHANGE_ALT_ON,
    CHANGE_ALT_OFF,
    CHANGE_EDIT,
    CHANGE_COUNT,
};

static const char *const change_names[] = {
    "a clear", "new lines", "a narrow resize", "a wide resize", "a scroll",
    "an eviction", "the alternate screen", "the primary screen",
    "an edit of a shown row",
};

static void change_setup(struct wsap_vtcore *term, enum change change,
                         uint64_t *state)
{
    feed(term, "\x1b[?1049l");
    wsap_vtcore_scroll_to(term, 0);
    assert(wsap_vtcore_set_scrollback_limit(term,
                                            change == CHANGE_EVICT ? 150 : 60000));
    assert(wsap_vtcore_resize(term,
                              (struct wsap_vtcore_vector2i){ .x = 80, .y = 24 }));
    feed(term, "\x1b[H\x1b[2J\x1b[3J");
    feed_wide(term, state, change == CHANGE_EVICT ? 400 : 250);
    if (change == CHANGE_ALT_OFF)
        feed(term, "\x1b[?1049hALT TEXT\r\n");
}

static void change_apply(struct wsap_vtcore *term, enum change change,
                         uint64_t *state)
{
    switch (change) {
    case CHANGE_CLEAR: feed(term, "\x1b[H\x1b[2J\x1b[3J"); break;
    case CHANGE_LINES: feed_wide(term, state, 30); break;
    case CHANGE_NARROW:
        assert(wsap_vtcore_resize(term,
                                  (struct wsap_vtcore_vector2i){ .x = 47, .y = 24 }));
        break;
    case CHANGE_WIDE:
        assert(wsap_vtcore_resize(term,
                                  (struct wsap_vtcore_vector2i){ .x = 140, .y = 24 }));
        break;
    case CHANGE_SCROLL: wsap_vtcore_scroll(term, 25); break;
    case CHANGE_EVICT: feed_wide(term, state, 40); break;
    case CHANGE_ALT_ON: feed(term, "\x1b[?1049hALT\r\n"); break;
    case CHANGE_ALT_OFF: feed(term, "\x1b[?1049l"); break;
    case CHANGE_EDIT: feed(term, "\x1b[3;2HIN-LOCK EDIT\x1b[24;1H"); break;
    case CHANGE_COUNT: break;
    }
}

static bool same_text(const char *a, size_t a_len, const struct text *b)
{
    return a_len == b->len && !memcmp(a, b->bytes, a_len);
}

/* The thread that holds the provider lock changes the term. The term lock
 * nests on one thread, so the change runs. Each read must give a whole
 * document: the new one after a history change, as an oracle from the term
 * gives it, and the old one after an edit of a shown row only, as the
 * provider rule allows. A nested lock reads the same document. After the
 * unlock, a publish gives the new document. */
static void same_thread_change(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    struct ui_accessibility_tree tree = {0};
    uint64_t state = UINT64_C(0xf11) * UINT64_C(0x9e3779b97f4a7c15) + 1;

    for (int c = 0; c < CHANGE_COUNT; c++) {
        enum change change = (enum change)c;
        change_setup(term, change, &state);
        const struct ui_accessibility_node *node = build_node(content, &tree);
        const struct ui_accessibility_text_provider *text = node->text;

        text->lock(text->instance);
        struct text before = term_document(term);
        char *old_document = read_consistent(node);
        size_t old_len = strlen(old_document);
        assert(same_text(old_document, old_len, &before));
        change_apply(term, change, &state);
        struct text after = term_document(term);
        char *document = read_consistent(node);
        size_t len = strlen(document);
        text->lock(text->instance);
        char *nested = read_consistent(node);
        text->unlock(text->instance);
        bool kept_old = len == old_len && !memcmp(document, old_document, len);
        bool is_new = same_text(document, len, &after);
        printf("in the lock, after %s: %zu bytes, old %zu, term %zu: %s\n",
               change_names[c], len, old_len, after.len,
               is_new ? "new" : kept_old ? "old" : "TORN");
        fflush(stdout);
        assert(!strcmp(nested, document));
        if (change == CHANGE_EDIT) {
            assert(!same_text(old_document, old_len, &after));
            assert(kept_old);
        } else {
            assert(is_new);
        }
        text->unlock(text->instance);

        char *published = read_consistent(build_node(content, &tree));
        struct text now = term_document(term);
        assert(same_text(published, strlen(published), &now));
        free(published);
        free(now.bytes);
        free(nested);
        free(document);
        free(old_document);
        free(after.bytes);
        free(before.bytes);
    }
    feed(term, "\x1b[?1049l");
    wsap_vtcore_scroll_to(term, 0);

    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
}

struct reader {
    struct ui_accessibility_text_provider text;
    atomic_bool started;
    atomic_bool stop;
};

/* Calls length and read with no provider lock until the test stops it. */
static void *reader_main(void *arg)
{
    struct reader *reader = arg;
    size_t sum = 0;
    while (!atomic_load(&reader->stop)) {
        char buffer[64];
        sum += reader->text.length(reader->text.instance);
        sum += reader->text.read(reader->text.instance, 0, buffer,
                                 sizeof(buffer));
        atomic_store(&reader->started, true);
        sched_yield();
    }
    return (void *)(uintptr_t)sum;
}

/* A second thread calls length and read while this thread publishes. Each
 * call of the second thread must wait for the publish and read the term on
 * its own. A call that uses the cache of the publish races with it, and TSan
 * reports the race. */
static void second_thread(void)
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
    feed_lines(term, 0, 299);
    const struct ui_accessibility_node *node = build_node(content, &tree);

    struct reader reader = { .text = *node->text };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, reader_main, &reader) == 0);
    while (!atomic_load(&reader.started))
        sched_yield();
    for (int i = 0; i < 100; i++) {
        char *document = read_consistent(node);
        assert(strncmp(document, "cost 000000", 11) == 0);
        free(document);
    }
    atomic_store(&reader.stop, true);
    void *sum = NULL;
    assert(pthread_join(thread, &sum) == 0);
    assert((uintptr_t)sum > 0);

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
    full_ring_cost();
    lifetime();
    second_thread();
    same_thread_change();
    text_interval();
    return 0;
}
