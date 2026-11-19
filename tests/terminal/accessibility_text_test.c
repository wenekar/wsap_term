/* The text of the terminal accessibility document. The test feeds known
 * lines and compares the whole document with the fed text. Each fed line
 * drops its trailing blanks and ends with one '\n'. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "app/tabs/tabs.h"
#include "session/session.h"
#include "terminal/internal.h"
#include "ui/accessibility.h"
#include "ui/widget.h"

struct fixture {
    struct test_app app;
    struct terminal *content;
    struct wsap_vtcore *term;
    struct ui_accessibility_tree tree;
    char *expect;
    size_t expect_len;
    size_t expect_cap;
};

static void fixture_init(struct fixture *f, int cols, int rows)
{
    *f = (struct fixture){0};
    test_app_init(&f->app, "sleep 60", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(f->app.app);
    assert(w && app_tabs_new(w));
    f->content = app_tabs_active(w);
    assert(f->content);
    f->term = terminal_session(f->content)->term;
    assert(wsap_vtcore_set_scrollback_limit(f->term, 60000));
    assert(wsap_vtcore_resize(f->term,
                              (struct wsap_vtcore_vector2i){ .x = cols, .y = rows }));
}

static void fixture_fini(struct fixture *f)
{
    ui_accessibility_tree_destroy(&f->tree);
    test_app_fini(&f->app);
    free(f->expect);
}

static void feed(struct fixture *f, const char *text, size_t len)
{
    assert(wsap_vtcore_feed(f->term, text, len, 0));
}

static void feeds(struct fixture *f, const char *text)
{
    feed(f, text, strlen(text));
}

/* Adds one line to the expected text. */
static void expect_line(struct fixture *f, const char *text, size_t len)
{
    while (len && text[len - 1] == ' ')
        len--;
    if (f->expect_len + len + 2 > f->expect_cap) {
        f->expect_cap = (f->expect_len + len + 2) * 2;
        f->expect = realloc(f->expect, f->expect_cap);
        assert(f->expect);
    }
    memcpy(f->expect + f->expect_len, text, len);
    f->expect_len += len;
    f->expect[f->expect_len++] = '\n';
    f->expect[f->expect_len] = '\0';
}

/* Feeds one line and its line end, and adds it to the expected text. */
static void feed_line(struct fixture *f, const char *text, size_t len)
{
    expect_line(f, text, len);
    feed(f, text, len);
    feed(f, "\r\n", 2);
}

static char *read_document(struct fixture *f, size_t *out_len)
{
    ui_accessibility_tree_destroy(&f->tree);
    assert(ui_accessibility_tree_build(&f->content->view.widget, &f->tree));
    assert(f->tree.count > 0);
    const struct ui_accessibility_node *node = &f->tree.nodes[0];
    const struct ui_accessibility_text_provider *text = node->text;
    text->lock(text->instance);
    size_t length = ui_accessibility_node_text_length(node);
    char *document = malloc(length + 1);
    assert(document);
    size_t got = ui_accessibility_node_text_read(node, 0, document, length);
    text->unlock(text->instance);
    assert(got == length);
    document[got] = '\0';
    *out_len = got;
    return document;
}

static size_t first_diff(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t n = a_len < b_len ? a_len : b_len, i = 0;
    while (i < n && a[i] == b[i])
        i++;
    return i;
}

static void expect_document(struct fixture *f, const char *label)
{
    size_t len = 0;
    char *document = read_document(f, &len);
    bool same = len == f->expect_len && !memcmp(document, f->expect, len);
    printf("%s: document %zu bytes, fed text %zu bytes\n", label, len,
           f->expect_len);
    if (!same)
        printf("%s: the first difference is at byte %zu\n", label,
               first_diff(document, len, f->expect, f->expect_len));
    fflush(stdout);
    assert(same);
    free(document);
}

/* A line continues from the last history row into the first view row. The
 * last column of the history row holds a blank, which separates two words. */
static void blank_at_history_edge(void)
{
    struct fixture f;
    fixture_init(&f, 80, 24);
    char line[512];
    for (int i = 0; i < 30; i++) {
        int n = snprintf(line, sizeof(line), "s%02d", i);
        feed_line(&f, line, (size_t)n);
    }
    size_t n = 0;
    memset(line, 'A', 79);
    n = 79;
    line[n++] = ' ';
    memset(line + n, 'B', 79);
    n += 79;
    line[n++] = ' ';
    memset(line + n, 'C', 10);
    n += 10;
    feed_line(&f, line, n);
    for (int i = 0; i < 21; i++) {
        int m = snprintf(line, sizeof(line), "t%02d", i);
        feed_line(&f, line, (size_t)m);
    }
    /* The first row of the long line is the last history row. */
    assert(wsap_vtcore_history_rows(f.term) == 31);
    expect_document(&f, "blank at the history edge");
    fixture_fini(&f);
}

static uint64_t next_random(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* Feeds lines of wide cells, clusters, emoji, and blanks. A line has no tab:
 * a tab stop depends on the width at the write. */
static void feed_wide_lines(struct fixture *f, uint64_t *state, int lines,
                            int max_pieces)
{
    static const char *const pieces[] = {
        "\u4e2d", "e\xcc\x81", "x", "\u4e2d\u6587", "\u4e2d\u4e2d", "abc",
        " ", "  ", "\xf0\x9f\x98\x80", "   ",
    };
    const uint64_t kinds = sizeof(pieces) / sizeof(pieces[0]);
    char line[8192];
    for (int i = 0; i < lines; i++) {
        size_t n = (size_t)snprintf(line, sizeof(line), "L%04d:", i);
        int count = 1 + (int)(next_random(state) % (uint64_t)max_pieces);
        for (int k = 0; k < count; k++) {
            const char *piece = pieces[next_random(state) % kinds];
            size_t m = strlen(piece);
            memcpy(line + n, piece, m);
            n += m;
        }
        feed_line(f, line, n);
    }
}

/* A line drops its trailing blanks as one line. When its last row holds
 * only blanks, the row before it drops its trailing blanks too. The text of
 * a line then stays the same at every width. */
static void text_does_not_depend_on_width(void)
{
    struct fixture f;
    fixture_init(&f, 100, 35);
    uint64_t state = UINT64_C(0x5eed) * UINT64_C(0x9e3779b97f4a7c15) + 1;
    feed_wide_lines(&f, &state, 300, 200);
    expect_document(&f, "fed at 100");
    static const struct wsap_vtcore_vector2i sizes[] = {
        { .x = 54, .y = 41 }, { .x = 100, .y = 35 }, { .x = 37, .y = 35 },
        { .x = 161, .y = 35 }, { .x = 79, .y = 20 },
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        assert(wsap_vtcore_resize(f.term, sizes[i]));
        char label[64];
        snprintf(label, sizeof(label), "rewrap to %dx%d", sizes[i].x, sizes[i].y);
        expect_document(&f, label);
    }

    /* Blanks pushed past the edge give a last row with no text. */
    assert(wsap_vtcore_resize(f.term, (struct wsap_vtcore_vector2i){ .x = 20, .y = 10 }));
    feeds(&f, "\x1b[H\x1b[2J\x1b[3J");
    f.expect_len = 0;
    char line[128];
    for (int i = 0; i < 30; i++) {
        int n = snprintf(line, sizeof(line), "word%02d", i);
        memset(line + n, ' ', 17 + (size_t)i % 5);
        feed_line(&f, line, (size_t)n + 17 + (size_t)i % 5);
    }
    expect_document(&f, "blank last rows at 20");
    assert(wsap_vtcore_resize(f.term, (struct wsap_vtcore_vector2i){ .x = 40, .y = 10 }));
    expect_document(&f, "blank last rows at 40");
    fixture_fini(&f);
}

int main(void)
{
    blank_at_history_edge();
    text_does_not_depend_on_width();
    return 0;
}
