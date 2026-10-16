/*
 * Differential fuzz test for wsap_vtcore_feed. Each seed makes random terminal
 * output: text, controls, CSI and ESC sequences, raw bytes, and resizes.
 * One terminal gets the output in a few large writes; the bulk fast paths
 * handle it. A second terminal gets it one byte at a time; the generic
 * parser handles it. The two must show the same cursor, screen, and
 * scrollback.
 */
#include "term_api.h"
#include "fuzz_gen.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each terminal has its own snapshot, so cells of both stay readable at
 * the same time. */
static struct wsap_vtcore_snapshot *snap_bulk, *snap_bytes;

/* The code points of a cell's text. A cluster has handles that differ
 * between snapshots, so the test compares its code points. */
static int cell_text(const struct wsap_vtcore_snapshot *s, const struct wsap_vtcore_cell *cell,
                     uint32_t *cps, int cap)
{
    int n = wsap_vtcore_snapshot_cluster(s, cell->text, cps, cap);
    return n < cap ? n : cap;
}

static bool same_cell(const struct wsap_vtcore_cell *a, const struct wsap_vtcore_cell *b)
{
    if (a->fg != b->fg || a->bg != b->bg || a->attrs != b->attrs || a->width != b->width)
        return false;
    uint32_t ta[16], tb[16];
    int na = cell_text(snap_bulk, a, ta, 16);
    int nb = cell_text(snap_bytes, b, tb, 16);
    return na == nb && memcmp(ta, tb, (size_t)na * sizeof(ta[0])) == 0;
}

static void assert_same(struct wsap_vtcore *bulk, struct wsap_vtcore *bytes, int seed, int step)
{
    int cols = tt_cols(bulk), rows = tt_rows(bulk);
    int history = tt_history(bulk);
    bool same = tt_cursor_x(bulk) == tt_cursor_x(bytes) &&
                tt_cursor_y(bulk) == tt_cursor_y(bytes) &&
                history == tt_history(bytes);
    wsap_vtcore_scroll(bulk, history);
    wsap_vtcore_scroll(bytes, history);
    for (int page = 0; same && page <= history; page += rows) {
        bool copied = wsap_vtcore_snapshot_update(snap_bulk, bulk);
        copied &= wsap_vtcore_snapshot_update(snap_bytes, bytes);
        assert(copied);
        for (int y = 0; same && y < rows; y++) {
            const struct wsap_vtcore_cell *ra = wsap_vtcore_snapshot_row(snap_bulk, y);
            const struct wsap_vtcore_cell *rb = wsap_vtcore_snapshot_row(snap_bytes, y);
            for (int x = 0; x < cols; x++) {
                const struct wsap_vtcore_cell *a = &ra[x], *b = &rb[x];
                if (!same_cell(a, b)) {
                    fprintf(stderr, "seed %d step %d: page %d row %d col %d: "
                            "bulk %x/%x/%x/%x/%u, bytes %x/%x/%x/%x/%u\n",
                            seed, step, page, y, x, a->text, a->fg, a->bg, a->attrs,
                            a->width, b->text, b->fg, b->bg, b->attrs, b->width);
                    same = false;
                    break;
                }
            }
        }
        wsap_vtcore_scroll(bulk, -rows);
        wsap_vtcore_scroll(bytes, -rows);
    }
    wsap_vtcore_scroll_to(bulk, 0);
    wsap_vtcore_scroll_to(bytes, 0);
    if (!same)
        fprintf(stderr, "seed %d step %d: cursor %d,%d vs %d,%d, history %d vs %d\n",
                seed, step, tt_cursor_x(bulk), tt_cursor_y(bulk),
                tt_cursor_x(bytes), tt_cursor_y(bytes),
                history, tt_history(bytes));
    assert(same);
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 50000;
    for (int seed = 1; seed <= seeds; seed++) {
        rng = 0x9e3779b97f4a7c15ull * (uint64_t)seed;
        int cols = 4 + rn(30), rows = 2 + rn(10);
        struct wsap_vtcore *bulk = tt_new(cols, rows);
        struct wsap_vtcore *bytes = tt_new(cols, rows);
        /* A new terminal can get the address of a freed one, so each seed
         * uses new snapshots. */
        snap_bulk = wsap_vtcore_snapshot_new();
        snap_bytes = wsap_vtcore_snapshot_new();
        assert(bulk && bytes && snap_bulk && snap_bytes);
        for (int step = 0; step < 6; step++) {
            blen = 0;
            gen(20 + rn(60), cols, rows);
            size_t split = blen ? (size_t)rn((int)blen + 1) : 0;
            tt_write(bulk, buf, split);
            tt_write(bulk, buf + split, blen - split);
            for (size_t i = 0; i < blen; i++)
                tt_write(bytes, buf + i, 1);
            assert_same(bulk, bytes, seed, step);
            if (rn(4) == 0) {
                cols = 4 + rn(30);
                rows = 2 + rn(10);
                bool resized = tt_resize(bulk, cols, rows);
                resized &= tt_resize(bytes, cols, rows);
                assert(resized);
                assert_same(bulk, bytes, seed, step);
            }
        }
        wsap_vtcore_snapshot_free(snap_bulk);
        wsap_vtcore_snapshot_free(snap_bytes);
        wsap_vtcore_free(bulk);
        wsap_vtcore_free(bytes);
    }
    return 0;
}
