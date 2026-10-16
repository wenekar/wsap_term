#include <assert.h>
#include "term_api.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The scrollback view: the viewport can leave the live screen, and the
 * terminal then shows history rows. These tests cover what the view does
 * while the terminal keeps working underneath it.
 */

/* The tests use small terminals, so a row fits in a short buffer. */
#define VIEW_TEXT_CAP 256

/* The text of a view row, trailing blanks cut. */
static void view_row_text(struct wsap_vtcore *t, int row, char *buf, size_t cap)
{
    const struct wsap_vtcore_cell *cells = wsap_vtcore_snapshot_row(tt_snapshot_for(t), row);
    size_t n = 0;
    for (int c = 0; c < tt_cols(t) && n + 1 < cap; c++) {
        uint32_t cp = tt_cell_cp(t, &cells[c]);
        buf[n++] = !cp ? ' ' : cp < 0x80 ? (char)cp : '?';
    }
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = 0;
}

static bool view_row_is(struct wsap_vtcore *t, int row, const char *want)
{
    char got[VIEW_TEXT_CAP];
    view_row_text(t, row, got, sizeof(got));
    return !strcmp(got, want);
}

/* Every view row holds the text of the same row in want. A view row that
 * resolves to no row at all fails the text compare. */
static void assert_view_rows_equal(struct wsap_vtcore *t, struct wsap_vtcore *want)
{
    for (int row = 0; row < tt_rows(t); row++) {
        char got[VIEW_TEXT_CAP], expect[VIEW_TEXT_CAP];
        view_row_text(t, row, got, sizeof(got));
        view_row_text(want, row, expect, sizeof(expect));
        assert(!strcmp(got, expect));
    }
}

/* The view offset, in range, and the same as the snapshot reports. The two
 * must come from one field: a second copy of the offset is what let the
 * scrollbar and the rows disagree. */
static void assert_offset_agrees(struct wsap_vtcore *t)
{
    const struct wsap_vtcore_view *v = wsap_vtcore_snapshot_view(tt_snapshot_for(t));
    struct wsap_vtcore_scrollback sb = wsap_vtcore_scrollback(t);
    assert(sb.offset == v->scroll_offset);
    assert(sb.offset >= 0);
    assert(sb.offset <= sb.total);
}

/* A terminal whose history is small enough to fill. */
static struct wsap_vtcore *small_history_term(int cols, int rows, int scrollback)
{
    struct wsap_vtcore_options opt = {
        .size = { .x = cols, .y = rows },
        .scrollback = scrollback,
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    return t;
}

/* Fill the history with count lines of "Lnn" and leave the cursor on the
 * last screen row. */
static void feed_lines(struct wsap_vtcore *t, const char *tag, int count)
{
    char line[64];
    for (int i = 0; i < count; i++) {
        int len = snprintf(line, sizeof(line), "%s%02d\r\n", tag, i);
        tt_write(t, line, (size_t)len);
    }
}

/* Output that arrives while the view sits in the history leaves the view
 * on the same text. */
static void view_holds_text_while_output_arrives(void)
{
    struct wsap_vtcore *t = tt_new(12, 5);
    struct wsap_vtcore *reference = tt_new(12, 5);
    feed_lines(t, "L", 15);
    feed_lines(reference, "L", 15);
    wsap_vtcore_scroll(t, 2);
    wsap_vtcore_scroll(reference, 2);
    assert(tt_view_offset(t) == 2);
    assert_view_rows_equal(t, reference);

    feed_lines(t, "M", 4);
    feed_lines(reference, "M", 4);
    assert_offset_agrees(t);
    /* Nothing was evicted, so the view still shows the rows it showed. */
    assert_view_rows_equal(t, reference);
    wsap_vtcore_free(t);
    wsap_vtcore_free(reference);
}

/* A view that sits at the top of the history keeps the oldest text there
 * when more output arrives. The history grows by a row, so the offset
 * grows with it. */
static void view_at_the_top_keeps_the_oldest_text(void)
{
    const int scrollback = 20;
    struct wsap_vtcore *t = small_history_term(12, 5, scrollback);
    struct wsap_vtcore *reference = small_history_term(12, 5, scrollback);
    feed_lines(t, "L", 12);
    feed_lines(reference, "L", 12);
    wsap_vtcore_scroll_to(t, INT_MAX);
    wsap_vtcore_scroll_to(reference, INT_MAX);
    int total = tt_history(t);
    assert(total > 0 && total + tt_rows(t) <= scrollback);
    assert(tt_view_offset(t) == total);
    assert(view_row_is(t, 0, "L00"));

    /* Three more lines with room in the history: the oldest line stays on
     * the top row of the view, and the offset grows with the history. */
    feed_lines(t, "M", 3);
    feed_lines(reference, "M", 3);
    assert_offset_agrees(t);
    assert(tt_history(t) == total + 3);
    assert(tt_view_offset(t) == total + 3);
    assert(view_row_is(t, 0, "L00"));
    assert_view_rows_equal(t, reference);
    wsap_vtcore_free(t);
    wsap_vtcore_free(reference);
}

/* Output larger than the history evicts the rows the view was anchored to.
 * The view then shows the oldest text the terminal still keeps, and the
 * offset stays inside the history. */
static void view_survives_output_larger_than_history(void)
{
    const int scrollback = 20;
    struct wsap_vtcore *t = small_history_term(12, 5, scrollback);
    struct wsap_vtcore *reference = small_history_term(12, 5, scrollback);
    feed_lines(t, "L", 15);
    feed_lines(reference, "L", 15);
    wsap_vtcore_scroll(t, 3);

    feed_lines(t, "M", 40);
    feed_lines(reference, "M", 40);
    assert_offset_agrees(t);
    assert(tt_history(t) == tt_history(reference));
    assert(tt_history(t) == scrollback);
    assert(tt_view_offset(t) > 0);

    /* The top of the history is where the view ends up. */
    wsap_vtcore_scroll(reference, INT_MAX);
    assert(tt_view_offset(reference) == tt_history(reference));
    assert(tt_view_offset(t) == tt_history(t));
    assert_view_rows_equal(t, reference);
    wsap_vtcore_free(t);
    wsap_vtcore_free(reference);
}

/* An eviction can drop a line that wraps to more rows than the push adds.
 * The offset then stops at the oldest row the terminal still keeps, and
 * never sits above the history. */
static void view_survives_eviction_of_a_wrapped_line(void)
{
    const int scrollback = 20;
    struct wsap_vtcore *t = small_history_term(12, 5, scrollback);
    struct wsap_vtcore *reference = small_history_term(12, 5, scrollback);
    /* The oldest line wraps to three rows. */
    const char *wrapped = "0123456789012345678901234\r\n";
    tt_puts(t, wrapped);
    tt_puts(reference, wrapped);
    feed_lines(t, "s", 22);
    feed_lines(reference, "s", 22);
    wsap_vtcore_scroll_to(t, INT_MAX);
    wsap_vtcore_scroll_to(reference, INT_MAX);
    assert_offset_agrees(t);

    /* The next line evicts the wrapped one: three rows go, one arrives. */
    for (int i = 0; i < 4; i++) {
        tt_puts(t, "n\r\n");
        tt_puts(reference, "n\r\n");
        assert_offset_agrees(t);
        assert(tt_view_offset(t) <= tt_history(t));
    }
    /* The view sits on the oldest text the terminal still keeps. */
    wsap_vtcore_scroll_to(reference, INT_MAX);
    assert(tt_view_offset(t) == tt_view_offset(reference));
    assert_view_rows_equal(t, reference);
    wsap_vtcore_free(t);
    wsap_vtcore_free(reference);
}

/* A resize keeps the same retained text at the top of the view. */
static void view_keeps_anchor_on_resize(void)
{
    static const struct {
        int cols, rows;
    } sizes[] = { { 12, 6 }, { 20, 5 }, { 6, 5 } };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct wsap_vtcore *t = tt_new(12, 5);
        struct wsap_vtcore *reference = tt_new(12, 5);
        feed_lines(t, "L", 15);
        feed_lines(reference, "L", 15);
        wsap_vtcore_scroll(t, 3);
        assert(tt_view_offset(t) == 3);
        assert(tt_resize(t, sizes[i].cols, sizes[i].rows));
        assert(tt_resize(reference, sizes[i].cols, sizes[i].rows));
        assert_offset_agrees(t);
        assert(tt_view_offset(t) > 0);
        assert(view_row_is(t, 0, "L08"));
        wsap_vtcore_scroll_to(t, 0);
        assert_view_rows_equal(t, reference);
        wsap_vtcore_free(t);
        wsap_vtcore_free(reference);
    }
}

/* The alternate screen has no history to show. The normal screen comes
 * back at its own bottom. */
static void view_follows_the_alternate_screen(void)
{
    struct wsap_vtcore *t = tt_new(12, 5);
    feed_lines(t, "L", 15);
    wsap_vtcore_scroll(t, 3);
    assert(tt_view_offset(t) == 3);

    tt_puts(t, "\033[?1049hALT");
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == 0);
    assert(view_row_is(t, 0, "ALT"));

    tt_puts(t, "\033[?1049l");
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == 0);
    assert(view_row_is(t, 0, "L11"));
    wsap_vtcore_free(t);
}

/* The view offset has one owner. Every operation that moves or clears the
 * view leaves the public offset and the snapshot offset equal. */
static void view_offset_has_one_owner(void)
{
    struct wsap_vtcore *t = tt_new(12, 5);
    feed_lines(t, "L", 15);
    assert_offset_agrees(t);
    wsap_vtcore_scroll(t, 3);
    assert_offset_agrees(t);
    wsap_vtcore_scroll_to(t, INT_MAX);
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == tt_history(t));
    wsap_vtcore_scroll(t, INT_MIN);
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == 0);
    wsap_vtcore_free(t);
}

/* ED2 and ED3 return the view to the live screen. RIS keeps the offset, as
 * it did before the view had an anchor of its own. */
static void view_returns_to_bottom_on_erase(void)
{
    struct wsap_vtcore *t = tt_new(12, 5);
    feed_lines(t, "L", 15);
    wsap_vtcore_scroll(t, 3);
    assert(tt_view_offset(t) == 3);

    tt_puts(t, "\033[2J");
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == 0);

    wsap_vtcore_scroll(t, 2);
    assert(tt_view_offset(t) == 2);
    tt_puts(t, "\033[3J");
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == 0);
    assert(tt_history(t) == 0);
    wsap_vtcore_free(t);

    t = tt_new(12, 5);
    feed_lines(t, "L", 15);
    wsap_vtcore_scroll(t, 3);
    int before = tt_view_offset(t);
    tt_puts(t, "\033c");
    assert_offset_agrees(t);
    assert(tt_view_offset(t) == before);
    wsap_vtcore_free(t);
}

/* Rewrap keeps the cell that selected the anchored segment in view. */
static void wrapped_anchor_survives_resize(void)
{
    struct wsap_vtcore *t = tt_new(10, 3);
    tt_puts(t, "abcdefghijKLMNOPQRSTuvwxyz\r\n");
    feed_lines(t, "L", 10);
    wsap_vtcore_scroll_to(t, INT_MAX);
    wsap_vtcore_scroll(t, -1);
    assert(view_row_is(t, 0, "KLMNOPQRST"));
    tt_select(t, 0, 0, 2, 0);
    char *selected = tt_selection_text(t);
    assert(selected && !strcmp(selected, "KLM"));
    free(selected);
    assert(tt_resize(t, 6, 3));
    assert_offset_agrees(t);
    assert(view_row_is(t, 0, "ghijKL"));
    selected = tt_selection_text(t);
    assert(selected && !strcmp(selected, "KLM"));
    free(selected);
    feed_lines(t, "N", 5);
    assert(view_row_is(t, 0, "ghijKL"));
    assert(tt_resize(t, 13, 3));
    assert(view_row_is(t, 0, "abcdefghijKLM"));
    assert(tt_resize(t, 6, 3));
    assert(view_row_is(t, 0, "ghijKL"));
    assert_offset_agrees(t);
    wsap_vtcore_free(t);
}

/* A wide glyph leaves a wrap pad. That pad does not move the anchor. */
static void wide_anchor_survives_resize(void)
{
    struct wsap_vtcore *t = tt_new(5, 3);
    tt_puts(t, "abcd日本語XYZ\r\n");
    feed_lines(t, "L", 10);
    wsap_vtcore_scroll_to(t, INT_MAX);
    wsap_vtcore_scroll(t, -1);
    assert(tt_cell_cp(t, tt_view_cell(t, 0, 0)) == 0x65e5);
    assert(tt_resize(t, 3, 3));
    assert_offset_agrees(t);
    assert(tt_cell_cp(t, tt_view_cell(t, 0, 0)) == 'd');
    assert(tt_cell_cp(t, tt_view_cell(t, 1, 0)) == 0x65e5);
    assert(tt_resize(t, 8, 3));
    assert(tt_cell_cp(t, tt_view_cell(t, 0, 0)) == 'a');
    assert(tt_resize(t, 3, 3));
    assert(tt_cell_cp(t, tt_view_cell(t, 1, 0)) == 0x65e5);
    wsap_vtcore_free(t);
}

/* Incremental and new snapshots must show the same retained rows. */
static void snapshots_agree_after_eviction_and_rewrap(void)
{
    struct wsap_vtcore *t = small_history_term(11, 5, 23);
    struct wsap_vtcore_snapshot *persistent = wsap_vtcore_snapshot_new();
    assert(persistent);
    for (int n = 0; n < 120; n++) {
        char line[120];
        snprintf(line, sizeof(line), "%03d abcdefghijklmnop 日本語 café\r\n", n);
        tt_puts(t, line);
        if (n % 13 == 0)
            assert(tt_resize(t, 6 + n % 17, 5));
        int total = tt_history(t);
        wsap_vtcore_scroll_to(t, n % (total + 1));
        assert(wsap_vtcore_snapshot_update(persistent, t));
        struct wsap_vtcore_snapshot *fresh = wsap_vtcore_snapshot_new();
        assert(fresh && wsap_vtcore_snapshot_update(fresh, t));
        struct wsap_vtcore_vector2i size = wsap_vtcore_size(t);
        for (int y = 0; y < size.y; y++) {
            const struct wsap_vtcore_cell *a = wsap_vtcore_snapshot_row(persistent, y);
            const struct wsap_vtcore_cell *b = wsap_vtcore_snapshot_row(fresh, y);
            for (int x = 0; x < size.x; x++) {
                assert(a[x].fg == b[x].fg && a[x].bg == b[x].bg);
                assert(a[x].width == b[x].width && a[x].attrs == b[x].attrs);
                uint32_t acp[32], bcp[32];
                int an = wsap_vtcore_snapshot_cluster(persistent, a[x].text, acp, 32);
                int bn = wsap_vtcore_snapshot_cluster(fresh, b[x].text, bcp, 32);
                assert(an == bn);
                if (an)
                    assert(!memcmp(acp, bcp, (size_t)an * sizeof(*acp)));
                else
                    assert(a[x].text == b[x].text);
            }
            assert(wsap_vtcore_snapshot_row_key(persistent, y) == wsap_vtcore_snapshot_row_key(fresh, y));
        }
        assert_offset_agrees(t);
        wsap_vtcore_snapshot_free(fresh);
    }
    wsap_vtcore_snapshot_free(persistent);
    wsap_vtcore_free(t);
}

static void snapshots_agree_after_prefix_eviction(void)
{
    struct wsap_vtcore *t = tt_new(1000, 3);
    char line[1000];
    for (int i = 0; i < 9500; i++) {
        memset(line, 'A' + i % 26, sizeof(line));
        tt_write(t, line, sizeof(line));
    }
    tt_write(t, "\r\n", 2);
    wsap_vtcore_scroll(t, INT_MAX);
    struct wsap_vtcore_snapshot *persistent = wsap_vtcore_snapshot_new();
    assert(persistent && wsap_vtcore_snapshot_update(persistent, t));
    uint64_t old_key = wsap_vtcore_snapshot_row_key(persistent, 0);
    memset(line, 'z', sizeof(line));
    for (int i = 0; i < 1200; i++) {
        tt_write(t, line, sizeof(line));
        tt_write(t, "\r\n", 2);
    }
    assert(wsap_vtcore_snapshot_update(persistent, t));
    assert(wsap_vtcore_snapshot_row_key(persistent, 0) != old_key);
    struct wsap_vtcore_snapshot *fresh = wsap_vtcore_snapshot_new();
    assert(fresh && wsap_vtcore_snapshot_update(fresh, t));
    for (int y = 0; y < 3; y++) {
        const struct wsap_vtcore_cell *a = wsap_vtcore_snapshot_row(persistent, y);
        const struct wsap_vtcore_cell *b = wsap_vtcore_snapshot_row(fresh, y);
        for (int x = 0; x < 1000; x++)
            assert(a[x].text == b[x].text && a[x].fg == b[x].fg && a[x].bg == b[x].bg);
        assert(wsap_vtcore_snapshot_row_key(persistent, y) == wsap_vtcore_snapshot_row_key(fresh, y));
    }
    wsap_vtcore_snapshot_free(fresh);
    wsap_vtcore_snapshot_free(persistent);
    wsap_vtcore_free(t);
}

/* A line that needs more than one storage part stays scannable. Every
 * scroll offset names one history row, and every view row shows the text
 * of the history row the offset puts there. */
static void view_scrolls_a_line_that_needs_several_parts(void)
{
    const int cols = 40, rows = 6, cells = 2000000;
    struct wsap_vtcore_options opt = { .size = { .x = cols, .y = rows }, .scrollback = 60000 };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    char *big = malloc((size_t)cells);
    assert(big);
    for (int i = 0; i < cells; i++)
        big[i] = (char)('0' + i % 10);
    tt_write(t, big, (size_t)cells);
    tt_write(t, "\r\n", 2);
    free(big);

    /* The rows still on the screen are not history rows. */
    int total = tt_history(t);
    assert(total == cells / cols - (rows - 1));
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap);
    for (int off = 1; off <= total; off += 5) {
        wsap_vtcore_scroll_to(t, off);
        assert(tt_view_offset(t) == off);
        assert(wsap_vtcore_snapshot_update(snap, t));
        for (int y = 0; y < rows; y++) {
            int row = total - off + y;
            if (row >= total)
                continue;
            const struct wsap_vtcore_cell *line = wsap_vtcore_snapshot_row(snap, y);
            for (int x = 0; x < cols; x++)
                assert(line[x].text == (uint32_t)('0' + (row * cols + x) % 10));
        }
    }
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
}

int main(void)
{
    snapshots_agree_after_prefix_eviction();
    wrapped_anchor_survives_resize();
    wide_anchor_survives_resize();
    snapshots_agree_after_eviction_and_rewrap();
    view_holds_text_while_output_arrives();
    view_at_the_top_keeps_the_oldest_text();
    view_survives_output_larger_than_history();
    view_survives_eviction_of_a_wrapped_line();
    view_keeps_anchor_on_resize();
    view_follows_the_alternate_screen();
    view_offset_has_one_owner();
    view_scrolls_a_line_that_needs_several_parts();
    view_returns_to_bottom_on_erase();
    printf("PASS: scrollback view tests\n");
    return 0;
}
