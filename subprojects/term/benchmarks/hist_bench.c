#include "bench.h"
#include "hist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make_row(const char *name, struct cell *row)
{
    struct cell d = { ' ', WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, 0, 0 };
    int n = 0;
    if (!strcmp(name, "ascii-row")) {
        const char *s = "The quick brown fox jumps over the lazy dog. 0123456789";
        for (; s[n]; n++) {
            row[n] = d;
            row[n].c = (uint8_t)s[n];
        }
    } else if (!strcmp(name, "cjk-row")) {
        for (int i = 0; i < 23; i++) {
            row[n] = d;
            row[n].c = 0x65e5 + (uint32_t)i;
            row[n++].flags = CELL_WIDE;
            row[n] = d;
            row[n].c = 0;
            row[n++].flags = CELL_TRAIL;
        }
    } else if (!strcmp(name, "colors-row")) {
        const char *s = "red RGB";
        for (; s[n]; n++) {
            row[n] = d;
            row[n].c = (uint8_t)s[n];
            if (n < 3)
                row[n].fg = wsap_vtcore_color_index(1);
            if (n > 3)
                row[n].fg = wsap_vtcore_color_rgb(12, 34, 56);
        }
    } else if (!strcmp(name, "fg-char-row")) {
        for (; n < 120; n++) {
            row[n] = d;
            row[n].c = 'a' + (uint32_t)(n % 26);
            row[n].fg = wsap_vtcore_color_rgb((uint8_t)n, (uint8_t)(n * 3), (uint8_t)(n * 7));
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : 10000000;
    struct hist h;
    if (!wsap_vtcore_hist_init(&h, 10000, 8 * 1024 * 1024)) {
        fprintf(stderr, "failed to initialize hist\n");
        return EXIT_FAILURE;
    }

    const char *line = "abcdefghijklmnopqrstuvwxyz\n";
    int len = (int)strlen(line);
    for (int i = 0; i < 10000; i++)
        wsap_vtcore_hist_push_ascii(&h, line, len, NULL, false);
    double t0 = bench_now();
    for (int i = 0; i < iterations; i++)
        wsap_vtcore_hist_push_ascii(&h, line, len, NULL, false);
    double elapsed = bench_now() - t0;
    printf("%-12s %6.2f ns/line  %.2f GiB/s (%d B/line)\n", "ascii",
           elapsed * 1e9 / iterations,
           (double)iterations * len / (1024.0 * 1024.0 * 1024.0) / elapsed, len);
    wsap_vtcore_hist_free(&h);

    static const char *rows[] = { "ascii-row", "cjk-row", "colors-row", "fg-char-row" };
    for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
        struct cell row[128];
        int n = make_row(rows[k], row);
        int iters = iterations / 4;
        if (!wsap_vtcore_hist_init(&h, 4096, 0))
            return EXIT_FAILURE;
        for (int i = 0; i < 10000; i++)
            wsap_vtcore_hist_push_row(&h, row, n, false, NULL);
        t0 = bench_now();
        for (int i = 0; i < iters; i++)
            wsap_vtcore_hist_push_row(&h, row, n, false, NULL);
        elapsed = bench_now() - t0;
        printf("%-12s %6.2f ns/row   (%d cells)\n", rows[k], elapsed * 1e9 / iters, n);
        wsap_vtcore_hist_free(&h);
    }
    return EXIT_SUCCESS;
}
