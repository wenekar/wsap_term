#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void expect_cluster(const struct wsap_vtcore_snapshot *snapshot, uint32_t text)
{
    uint32_t cps[4];
    int count = wsap_vtcore_snapshot_cluster(snapshot, text, cps, 4);
    assert(count == 2);
    assert(cps[0] == 'e' && cps[1] == 0x301);
}

static void test_snapshot_owns_captured_data(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 8, .y = 2 } };
    wsap_vtcore_theme_default(&options.theme);
    options.theme.fg = 0xff123456u;
    struct wsap_vtcore *term = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(term && snapshot);

    const char text[] = "e\xcc\x81\xe4\xb8\xadX";
    assert(wsap_vtcore_feed(term, text, sizeof(text) - 1, 0));
    assert(wsap_vtcore_select_start(term,
           (struct wsap_vtcore_vector2i){0}, false, WSAP_VTCORE_SELECT_CELL));
    assert(wsap_vtcore_select_extend(term,
           (struct wsap_vtcore_vector2i){ .x = 3 }, true));
    assert(wsap_vtcore_snapshot_update(snapshot, term));

    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(snapshot);
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snapshot, 0);
    struct wsap_vtcore_span selection = wsap_vtcore_snapshot_row_selection(snapshot, 0);
    uint64_t key = wsap_vtcore_snapshot_row_key(snapshot, 0);
    uint64_t version = view->version;
    assert(row && row[0].width == 1 && row[1].width == 2 && row[2].width == 0);
    assert(row[3].text == 'X');
    expect_cluster(snapshot, row[0].text);
    assert(view->palette[WSAP_VTCORE_PALETTE_FG] == 0xff123456u);
    assert(selection.end > selection.begin);

    options.theme.fg = 0xffabcdefu;
    wsap_vtcore_set_theme(term, &options.theme);
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 4, .y = 3 }));
    assert(wsap_vtcore_feed(term, "\033[2J\033[HZ", 8, 0));
    wsap_vtcore_select_clear(term);
    wsap_vtcore_free(term);

    assert(view == wsap_vtcore_snapshot_view(snapshot));
    assert(row == wsap_vtcore_snapshot_row(snapshot, 0));
    assert(view->size.x == 8 && view->size.y == 2 && view->version == version);
    assert(view->palette[WSAP_VTCORE_PALETTE_FG] == 0xff123456u);
    assert(row[1].text == 0x4e2d && row[3].text == 'X');
    assert(wsap_vtcore_snapshot_row_key(snapshot, 0) == key);
    struct wsap_vtcore_span retained = wsap_vtcore_snapshot_row_selection(snapshot, 0);
    assert(retained.begin == selection.begin && retained.end == selection.end);
    expect_cluster(snapshot, row[0].text);

    wsap_vtcore_snapshot_free(snapshot);
}

static void test_snapshot_can_change_source(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 8, .y = 2 } };
    struct wsap_vtcore *first = wsap_vtcore_new(&options);
    struct wsap_vtcore *second = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(first && second && snapshot);

    assert(wsap_vtcore_feed(first, "e\xcc\x81", 3, 0));
    assert(wsap_vtcore_feed(second, "B", 1, 0));
    assert(wsap_vtcore_snapshot_update(snapshot, first));
    expect_cluster(snapshot, wsap_vtcore_snapshot_row(snapshot, 0)[0].text);
    assert(wsap_vtcore_snapshot_update(snapshot, second));
    assert(wsap_vtcore_snapshot_row(snapshot, 0)[0].text == 'B');
    assert(wsap_vtcore_snapshot_view(snapshot)->scrolled == 0);
    assert(wsap_vtcore_snapshot_update(snapshot, first));
    expect_cluster(snapshot, wsap_vtcore_snapshot_row(snapshot, 0)[0].text);

    wsap_vtcore_free(first);
    wsap_vtcore_free(second);
    wsap_vtcore_snapshot_free(snapshot);
}

static bool same_text(const struct wsap_vtcore_snapshot *a, uint32_t ta,
                      const struct wsap_vtcore_snapshot *b, uint32_t tb)
{
    uint32_t ca[32], cb[32];
    int na = wsap_vtcore_snapshot_cluster(a, ta, ca, 32);
    int nb = wsap_vtcore_snapshot_cluster(b, tb, cb, 32);
    return na == nb && na <= 32 && !memcmp(ca, cb, (size_t)na * sizeof(*ca));
}

/* The row store from wsap_vtcore_snapshot_rows must agree with the row getters,
 * and the snapshot must show what a new snapshot of the same terminal shows. */
static int expect_rows_match(const struct wsap_vtcore_snapshot *s, struct wsap_vtcore *term)
{
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(s);
    int cols = view->size.x, rows = view->size.y;
    struct wsap_vtcore_snapshot_rows store = wsap_vtcore_snapshot_rows(s);
    assert(store.cells && store.keys && store.top >= 0 && store.top < rows);
    struct wsap_vtcore_snapshot *fresh = wsap_vtcore_snapshot_new();
    assert(fresh && wsap_vtcore_snapshot_update(fresh, term));
    assert(wsap_vtcore_snapshot_view(fresh)->size.x == cols);
    assert(wsap_vtcore_snapshot_view(fresh)->size.y == rows);
    for (int y = 0; y < rows; y++) {
        int phys = (store.top + y) % rows;
        const struct wsap_vtcore_cell *ring = store.cells + (size_t)phys * cols;
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, y);
        const struct wsap_vtcore_cell *want = wsap_vtcore_snapshot_row(fresh, y);
        assert(!memcmp(ring, row, (size_t)cols * sizeof(*row)));
        assert(store.keys[phys] == wsap_vtcore_snapshot_row_key(s, y));
        assert(store.keys[phys] == wsap_vtcore_snapshot_row_key(fresh, y));
        for (int x = 0; x < cols; x++) {
            assert(row[x].fg == want[x].fg && row[x].bg == want[x].bg);
            assert(row[x].attrs == want[x].attrs && row[x].width == want[x].width);
            assert(same_text(s, row[x].text, fresh, want[x].text));
        }
    }
    wsap_vtcore_snapshot_free(fresh);
    return store.top;
}

static void feed(struct wsap_vtcore *term, const char *s)
{
    assert(wsap_vtcore_feed(term, s, strlen(s), 0));
}

static void test_snapshot_rows_follow_ring(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 10, .y = 4 }, .scrollback = 100 };
    struct wsap_vtcore *term = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(term && snapshot);

    // Scroll one and two lines at a time. The ring start must pass the end.
    bool wrapped = false;
    int last_top = 0;
    char line[32];
    for (int i = 0; i < 14; i++) {
        snprintf(line, sizeof(line), i % 3 ? "\r\nl%d e\xcc\x81" : "\r\nl%d\r\nm%d", i, i);
        feed(term, line);
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        int top = expect_rows_match(snapshot, term);
        wrapped |= top < last_top;
        last_top = top;
    }
    assert(wrapped);

    // A scroll, then a resize.
    feed(term, "\r\nbefore");
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 12, .y = 6 }));
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);
    for (int i = 0; i < 9; i++) {
        feed(term, "\r\nafter e\xcc\x81");
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        expect_rows_match(snapshot, term);
    }
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 7, .y = 3 }));
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);

    // The alternate screen, with scrolls inside it.
    feed(term, "\r\nx\r\ny");
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);
    feed(term, "\033[?1049halt");
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);
    for (int i = 0; i < 5; i++) {
        feed(term, "\r\nalt e\xcc\x81");
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        expect_rows_match(snapshot, term);
    }
    feed(term, "\033[?1049l");
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);

    // A full repaint after the ring start moved.
    feed(term, "\r\nz");
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);
    feed(term, "\033[2J\033[Hnew e\xcc\x81\r\nrows");
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);

    // A view scrolled into the history moves through the ring too.
    wsap_vtcore_scroll(term, 4);
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);
    for (int i = 0; i < 4; i++) {
        wsap_vtcore_scroll(term, -1);
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        expect_rows_match(snapshot, term);
    }

    wsap_vtcore_free(term);
    wsap_vtcore_snapshot_free(snapshot);
}

static void rewrite_rows(struct wsap_vtcore *term, int first, int count)
{
    char seq[64];
    for (int y = first; y < first + count; y++) {
        snprintf(seq, sizeof(seq), "\033[%d;1Hrow %d e\xcc\x81\033[K", y + 1, y);
        feed(term, seq);
    }
}

/* A wide screen has more new cells than the direct path takes. Change some
 * rows, then most rows, while the ring start is not zero. */
static void test_snapshot_rows_after_large_updates(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 200, .y = 40 }, .scrollback = 100 };
    struct wsap_vtcore *term = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(term && snapshot);
    rewrite_rows(term, 0, 40);
    assert(wsap_vtcore_snapshot_update(snapshot, term));
    expect_rows_match(snapshot, term);

    for (int round = 0; round < 3; round++) {
        feed(term, "\033[40;1H\r\nscroll e\xcc\x81\r\nagain");
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        assert(expect_rows_match(snapshot, term) != 0);
        rewrite_rows(term, 3, 18);
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        expect_rows_match(snapshot, term);
        feed(term, "\033[40;1H\r\nscroll");
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        assert(expect_rows_match(snapshot, term) != 0);
        rewrite_rows(term, 5, 30);
        assert(wsap_vtcore_snapshot_update(snapshot, term));
        expect_rows_match(snapshot, term);
    }

    wsap_vtcore_free(term);
    wsap_vtcore_snapshot_free(snapshot);
}

int main(void)
{
    test_snapshot_owns_captured_data();
    test_snapshot_can_change_source();
    test_snapshot_rows_follow_ring();
    test_snapshot_rows_after_large_updates();
    return 0;
}
