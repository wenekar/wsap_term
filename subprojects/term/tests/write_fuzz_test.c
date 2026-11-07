#include "term_api.h"
#include "fuzz_gen.h"
#include "seeds.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pair {
    struct wsap_vtcore *bulk, *bytes;
    struct wsap_vtcore_snapshot *snap_bulk, *snap_bytes;
    bool verbose;
};

static int cell_text(const struct wsap_vtcore_snapshot *s, const struct wsap_vtcore_cell *cell,
                     uint32_t *cps, int cap)
{
    int n = wsap_vtcore_snapshot_cluster(s, cell->text, cps, cap);
    return n < cap ? n : cap;
}

static bool same_cell(const struct pair *p, const struct wsap_vtcore_cell *a,
                      const struct wsap_vtcore_cell *b)
{
    if (a->fg != b->fg || a->bg != b->bg || a->attrs != b->attrs || a->width != b->width)
        return false;
    uint32_t ta[16], tb[16];
    int na = cell_text(p->snap_bulk, a, ta, 16);
    int nb = cell_text(p->snap_bytes, b, tb, 16);
    return na == nb && memcmp(ta, tb, (size_t)na * sizeof(ta[0])) == 0;
}

static bool same(const struct pair *p, int seed, int step)
{
    struct wsap_vtcore *bulk = p->bulk, *bytes = p->bytes;
    int cols = tt_cols(bulk), rows = tt_rows(bulk);
    int history = tt_history(bulk);
    bool same = tt_cursor_x(bulk) == tt_cursor_x(bytes) &&
                tt_cursor_y(bulk) == tt_cursor_y(bytes) &&
                history == tt_history(bytes);
    wsap_vtcore_scroll(bulk, history);
    wsap_vtcore_scroll(bytes, history);
    for (int page = 0; same && page <= history; page += rows) {
        bool copied = wsap_vtcore_snapshot_update(p->snap_bulk, bulk);
        copied &= wsap_vtcore_snapshot_update(p->snap_bytes, bytes);
        assert(copied);
        for (int y = 0; same && y < rows; y++) {
            const struct wsap_vtcore_cell *ra = wsap_vtcore_snapshot_row(p->snap_bulk, y);
            const struct wsap_vtcore_cell *rb = wsap_vtcore_snapshot_row(p->snap_bytes, y);
            for (int x = 0; x < cols; x++) {
                const struct wsap_vtcore_cell *a = &ra[x], *b = &rb[x];
                if (!same_cell(p, a, b)) {
                    if (p->verbose)
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
    if (!same && p->verbose)
        fprintf(stderr, "seed %d step %d: cursor %d,%d vs %d,%d, history %d vs %d\n",
                seed, step, tt_cursor_x(bulk), tt_cursor_y(bulk),
                tt_cursor_x(bytes), tt_cursor_y(bytes),
                history, tt_history(bytes));
    return same;
}

/* Feed the same input in two writes and one byte at a time. Return true when
 * both terminals show the same screen and history after each step. */
static bool run_seed(int seed, bool verbose)
{
    rng = 0x9e3779b97f4a7c15ull * (uint64_t)seed;
    int cols = 4 + rn(30), rows = 2 + rn(10);
    struct pair p = {
        .bulk = tt_new(cols, rows), .bytes = tt_new(cols, rows),
        .snap_bulk = wsap_vtcore_snapshot_new(), .snap_bytes = wsap_vtcore_snapshot_new(),
        .verbose = verbose,
    };
    assert(p.bulk && p.bytes && p.snap_bulk && p.snap_bytes);
    bool ok = true;
    for (int step = 0; ok && step < 6; step++) {
        blen = 0;
        gen(20 + rn(60), cols, rows);
        size_t split = blen ? (size_t)rn((int)blen + 1) : 0;
        tt_write(p.bulk, buf, split);
        tt_write(p.bulk, buf + split, blen - split);
        for (size_t i = 0; i < blen; i++)
            tt_write(p.bytes, buf + i, 1);
        ok = same(&p, seed, step);
        if (ok && rn(4) == 0) {
            cols = 4 + rn(30);
            rows = 2 + rn(10);
            bool resized = tt_resize(p.bulk, cols, rows);
            resized &= tt_resize(p.bytes, cols, rows);
            assert(resized);
            ok = same(&p, seed, step);
        }
    }
    wsap_vtcore_snapshot_free(p.snap_bulk);
    wsap_vtcore_snapshot_free(p.snap_bytes);
    wsap_vtcore_free(p.bulk);
    wsap_vtcore_free(p.bytes);
    return ok;
}

static bool seed_passes(int seed)
{
    return run_seed(seed, false);
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 50000;
    gen_unmodeled = true;
    int failed = run_seeds(seed_passes, 1, seeds, 0);
    if (failed < 0)
        return 0;
    // Run the first failed seed again on this thread to print the difference.
    bool ok = run_seed(failed, true);
    assert(!ok);
    return 1;
}
