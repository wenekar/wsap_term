#include "bench.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUDGET_MS_DEFAULT 0.5
#define FILL_EXTRA_ROWS 512
#define MAX_SAMPLES 4096

static const int history_sizes[] = { 100, 4096, 10000 };

enum content { SHORT, WRAPPED, MIXED, PROMPT, WIDE, CONTENT_COUNT };
static const char *content_names[] = {"short", "wrapped", "mixed", "prompt", "wide"};

static void emit_line(struct wsap_vtcore *t, enum content kind, int n, int cols)
{
    char line[4096];
    int len = 0;
    switch (kind) {
    case SHORT:
        len = snprintf(line, sizeof(line), "%d: short output line\r\n", n);
        break;
    case WRAPPED:
        for (int i = 0; i < 420; i++) line[len++] = 'a' + (n + i) % 26;
        line[len++] = '\r';
        line[len++] = '\n';
        break;
    case MIXED:
        if (n % 4 == 0)
            for (int i = 0; i < 300; i++) line[len++] = 'a' + (n + i) % 26;
        else
            len = snprintf(line, sizeof(line), "%d: build step ok", n);
        line[len++] = '\r';
        line[len++] = '\n';
        break;
    case PROMPT:
        if (n % 3 == 0)
            len = snprintf(line, sizeof(line),
                           "\033]133;A\033\\user@host:~/very/long/project/path $ \033]133;B\033\\"
                           "cargo build --release --features=all\r\n");
        else
            len = snprintf(line, sizeof(line), "   Compiling crate v0.%d.0\r\n", n % 100);
        break;
    case WIDE:
        for (int i = 0; i < 60; i++) len += snprintf(line + len, sizeof(line) - len, "日本語");
        line[len++] = '\r';
        line[len++] = '\n';
        break;
    default:
        break;
    }
    (void)cols;
    wsap_vtcore_feed(t, line, (size_t)len, 0);
}

static struct wsap_vtcore *make_filled(enum content kind, int cols, int rows, int history)
{
    struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = cols, .y = rows },
        .scrollback = history,
    });
    if (!t) return NULL;
    for (int n = 0; n < history + FILL_EXTRA_ROWS; n++)
        emit_line(t, kind, n, cols);
    return t;
}

static void resize(struct wsap_vtcore *t, int cols, int rows)
{
    if (!wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = cols, .y = rows })) {
        fprintf(stderr, "wsap_vtcore_resize failed: out of memory\n");
        exit(EXIT_FAILURE);
    }
}

static void report(const char *content, const char *scenario, double *s, int n, double budget_ms)
{
    bench_sort(s, n);
    double p50 = s[n / 2];
    double p99 = s[(int)((double)(n - 1) * 0.99)];
    double max = s[n - 1];
    printf("%-8s %-10s  p50 %7.3f  p99 %7.3f  max %7.3f ms  %s\n",
           content, scenario, p50, p99, max, max <= budget_ms ? "ok" : "OVER");
}

static double bench_pair(enum content kind, int from_cols, int to_cols, int rows,
                         int history, double *out, int samples)
{
    double worst = 0;
    for (int i = 0; i < samples; i++) {
        struct wsap_vtcore *t = make_filled(kind, from_cols, rows, history);
        if (!t) exit(EXIT_FAILURE);
        double start = bench_now();
        resize(t, to_cols, rows);
        double ms = (bench_now() - start) * 1e3;
        wsap_vtcore_free(t);
        out[i] = ms;
        if (ms > worst) worst = ms;
    }
    return worst;
}

static int bench_drag(enum content kind, int from_cols, int to_cols, int rows,
                      int history, double *out, int passes)
{
    int n = 0;
    int step = from_cols > to_cols ? -1 : 1;
    for (int p = 0; p < passes; p++) {
        struct wsap_vtcore *t = make_filled(kind, from_cols, rows, history);
        if (!t) exit(EXIT_FAILURE);
        for (int c = from_cols + step; c != to_cols + step; c += step) {
            double start = bench_now();
            resize(t, c, rows);
            out[n++] = (bench_now() - start) * 1e3;
        }
        wsap_vtcore_free(t);
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 && strcmp(argv[1], "all") ? argv[1] : NULL;
    double budget = argc > 2 ? strtod(argv[2], NULL) : BUDGET_MS_DEFAULT;
    int only_history = argc > 3 ? atoi(argv[3]) : 0;
    if (argc > 4 || only_history < 0) {
        fprintf(stderr, "usage: %s [content|all [budget-ms [history]]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int rows = 40, samples = 32;
    static const struct { const char *name; int from, to; } pairs[] = {
        {"grow", 80, 120},
        {"shrink", 120, 80},
        {"double", 80, 160},
        {"halve", 160, 80},
        {"narrow", 120, 20},
        {"widen", 20, 120},
    };
    static double s[MAX_SAMPLES];
    bool matched = false, over = false;

    for (size_t h = 0; h < sizeof(history_sizes) / sizeof(history_sizes[0]); h++) {
        int history = history_sizes[h];
        if (only_history && only_history != history) continue;
        bool history_over = false;
        printf("reflow budget %.3f ms, %d rows, scrollback %d\n\n", budget, rows, history);
        for (int k = 0; k < CONTENT_COUNT; k++) {
            if (only && strcmp(only, content_names[k])) continue;
            matched = true;
            for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
                double worst = bench_pair((enum content)k, pairs[p].from, pairs[p].to, rows,
                                          history, s, samples);
                history_over |= worst > budget;
                report(content_names[k], pairs[p].name, s, samples, budget);
            }
            int n = bench_drag((enum content)k, 120, 80, rows, history, s, 8);
            for (int i = 0; i < n; i++) history_over |= s[i] > budget;
            report(content_names[k], "drag", s, n, budget);
            putchar('\n');
        }
        printf("scrollback %d: %s\n\n", history,
               history_over ? "OVER budget" : "within budget");
        over |= history_over;
    }
    if (!matched) {
        if (only_history)
            fprintf(stderr, "unknown content mix or history size: %s %d\n",
                    argv[1], only_history);
        else
            fprintf(stderr, "unknown content mix: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    printf("%s\n", over ? "FAIL: at least one resize exceeded budget"
                        : "PASS: all resizes within budget");
    return over ? EXIT_FAILURE : EXIT_SUCCESS;
}
