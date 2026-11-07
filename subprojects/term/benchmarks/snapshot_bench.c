#include "bench.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Time wsap_vtcore_snapshot_update alone. Each step changes the terminal, and
 * then one update copies the change. The feed time is not in the result. */

enum { COLS = 120, ROWS = 40 };

struct bench_case {
    const char *name;
    void (*step)(struct wsap_vtcore **terms, int i);
};

static void feed(struct wsap_vtcore *t, const char *s)
{
    wsap_vtcore_feed(t, s, strlen(s), 0);
}

// One new line: the snapshot scrolls by one row.
static void step_scroll(struct wsap_vtcore **terms, int i)
{
    char line[96];
    snprintf(line, sizeof(line), "\r\nline %d: The quick brown fox jumps over the lazy dog.", i);
    feed(terms[0], line);
}

// One new line with clusters in it.
static void step_scroll_clusters(struct wsap_vtcore **terms, int i)
{
    char line[96];
    snprintf(line, sizeof(line), "\r\nline %d: e\xcc\x81 \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd caf\xc3\xa9", i);
    feed(terms[0], line);
}

// No change.
static void step_idle(struct wsap_vtcore **terms, int i)
{
    (void)terms;
    (void)i;
}

// Every row changes, as in a full-screen program that redraws.
static void step_repaint(struct wsap_vtcore **terms, int i)
{
    char row[160];
    for (int y = 0; y < ROWS; y++) {
        snprintf(row, sizeof(row), "\033[%d;1H\033[3%dmrow %d frame %d status text\033[K",
                 y + 1, (y + i) % 8, y, i);
        feed(terms[0], row);
    }
}

// The snapshot follows another terminal. Each update copies all rows.
static void step_switch(struct wsap_vtcore **terms, int i)
{
    (void)terms;
    (void)i;
}

static const struct bench_case cases[] = {
    {"scroll", step_scroll},
    {"scroll-clusters", step_scroll_clusters},
    {"idle", step_idle},
    {"repaint", step_repaint},
    {"switch", step_switch},
};

int main(int argc, char **argv)
{
    int iterations = argc > 2 ? atoi(argv[2]) : 5000;
    int reps = argc > 3 ? atoi(argv[3]) : 5;
    double samples[64];
    if (iterations <= 0 || reps <= 0 || reps > 64) {
        fprintf(stderr, "usage: %s [case|all [iterations (>0) [reps (1..64)]]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    bool matched = false;
    printf("# %-15s %10s %10s %10s\n", "case", "ns med", "min", "max");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (argc > 1 && strcmp(argv[1], "all") && strcmp(argv[1], cases[c].name))
            continue;
        matched = true;
        bool alternate = cases[c].step == step_switch;
        struct wsap_vtcore *terms[2];
        for (int k = 0; k < 2; k++) {
            terms[k] = wsap_vtcore_new(&(struct wsap_vtcore_options){
                .size = { .x = COLS, .y = ROWS }, .scrollback = 4096,
            });
            if (!terms[k])
                return EXIT_FAILURE;
            // Fill the screen and some history.
            for (int i = 0; i < 2 * ROWS; i++)
                step_scroll(&terms[k], i);
        }
        struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
        if (!snap || !wsap_vtcore_snapshot_update(snap, terms[0]))
            return EXIT_FAILURE;
        int step = 0;
        for (int r = -1; r < reps; r++) {
            double spent = 0;
            for (int i = 0; i < iterations; i++, step++) {
                cases[c].step(terms, step);
                struct wsap_vtcore *t = alternate ? terms[step & 1] : terms[0];
                double start = bench_now();
                if (!wsap_vtcore_snapshot_update(snap, t))
                    return EXIT_FAILURE;
                spent += bench_now() - start;
            }
            if (r >= 0)
                samples[r] = spent * 1e9 / iterations;
        }
        bench_sort(samples, reps);
        printf("%-17s %10.1f %10.1f %10.1f\n", cases[c].name,
               samples[reps / 2], samples[0], samples[reps - 1]);
        wsap_vtcore_snapshot_free(snap);
        wsap_vtcore_free(terms[0]);
        wsap_vtcore_free(terms[1]);
    }
    if (!matched)
        fprintf(stderr, "unknown benchmark case: %s\n", argv[1]);
    return matched ? EXIT_SUCCESS : EXIT_FAILURE;
}
