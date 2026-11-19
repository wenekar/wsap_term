#include <assert.h>
#include <stdio.h>
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

static void feed_lines(struct wsap_vtcore *term, int lines)
{
    char block[8192];
    size_t used = 0;
    for (int i = 0; i < lines; i++) {
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

int main(void)
{
    struct test_app app;
    test_app_init(&app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    assert(content);
    struct wsap_vtcore *term = terminal_session(content)->term;
    assert(wsap_vtcore_set_scrollback_limit(term, 60000));
    feed_lines(term, 50000);
    printf("history rows after feed: %d\n", wsap_vtcore_history_rows(term));
    assert(wsap_vtcore_history_rows(term) >= 49000);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct ui_accessibility_tree tree = {0};
    assert(ui_accessibility_tree_build(&content->view.widget, &tree));
    double build_ms = elapsed_ms(&start);
    assert(tree.count > 0);
    const struct ui_accessibility_node *node = &tree.nodes[0];
    assert(node->role == UI_ACCESSIBILITY_ROLE_TERMINAL);
    assert(node->name && strcmp(node->name, "Terminal") == 0);
    assert((node->states & UI_ACCESSIBILITY_STATE_READ_ONLY) &&
           (node->states & UI_ACCESSIBILITY_STATE_MULTILINE));
    assert(node->actions == 0);
    /* The node takes the provider. A whole-value copy would hold the
     * document text here. */
    assert(!node->value);
    assert(node->text && node->text->length && node->text->read);
    assert(node->text_length == SIZE_MAX);

    size_t length = ui_accessibility_node_text_length(node);
    assert(length > 50000 * 10);
    printf("terminal accessibility: %zu document bytes, build %.3f ms\n",
           length, build_ms);

    /* A read of one range copies its own bytes only. */
    char buffer[64];
    size_t offset = length - sizeof(buffer);
    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t copied = ui_accessibility_node_text_read(node, offset, buffer,
                                                    sizeof(buffer));
    double read_ms = elapsed_ms(&start);
    assert(copied > 0 && copied <= sizeof(buffer));
    printf("one range read: %zu bytes copied, %.3f ms\n", copied, read_ms);
    assert(read_ms < 50.0);

    /* A publish after one new line reads that line only. */
    feed_lines(term, 1);
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(ui_accessibility_tree_build(&content->view.widget, &tree));
    double next_build_ms = elapsed_ms(&start);
    node = &tree.nodes[0];
    size_t next_length = ui_accessibility_node_text_length(node);
    assert(next_length > length);
    printf("publish after one line: %zu bytes, %.3f ms\n", next_length,
           next_build_ms);

    /* The old path built 500 history rows at each publish. Measure the same
     * build for the comparison. */
    clock_gettime(CLOCK_MONOTONIC, &start);
    char *history = wsap_vtcore_history_text(term, 500);
    double scrape_ms = elapsed_ms(&start);
    assert(history);
    size_t scrape_bytes = strlen(history);
    wsap_vtcore_text_free(history);
    printf("old 500-row scrape: %zu bytes, %.3f ms\n", scrape_bytes, scrape_ms);

    /* A later publish keeps the roles, the names, and the actions. */
    struct ui_accessibility_tree next = {0};
    assert(ui_accessibility_tree_build(&content->view.widget, &next));
    assert(next.count == tree.count);
    for (size_t i = 0; i < tree.count; i++) {
        assert(next.nodes[i].id == tree.nodes[i].id);
        assert(next.nodes[i].role == tree.nodes[i].role);
        assert(next.nodes[i].actions == tree.nodes[i].actions);
        assert(!next.nodes[i].name == !tree.nodes[i].name);
        if (tree.nodes[i].name)
            assert(strcmp(next.nodes[i].name, tree.nodes[i].name) == 0);
    }
    ui_accessibility_tree_destroy(&next);
    ui_accessibility_tree_destroy(&tree);
    test_app_fini(&app);
    return 0;
}
