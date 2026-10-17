#include "bench.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double run_sample(struct wsap_vtcore *t, const char *buf, size_t len,
                         size_t chunk, int iterations, unsigned flags)
{
    double start = bench_now();
    for (int i = 0; i < iterations; i++) {
        for (size_t pos = 0; pos < len; pos += chunk)
            wsap_vtcore_feed(t, buf + pos, len - pos < chunk ? len - pos : chunk, flags);
    }
    return bench_now() - start;
}

enum { BENCH_SCROLLBACK = 4096 };

int main(int argc, char **argv)
{
    unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 42;
    size_t chunk = argc > 3 ? (size_t)strtoul(argv[3], NULL, 10) : 0;
    int iterations = argc > 4 ? atoi(argv[4]) : 512;
    int reps = argc > 5 ? atoi(argv[5]) : 7;
    if (chunk > 65536 || iterations <= 0 || reps <= 0) {
        fprintf(stderr, "usage: %s [case|all [seed [chunk (1..65536) [iterations (>0) [reps (>0)]]]]]\n"
                        "no chunk arg: each case runs whole-buffer and 4096-byte chunks\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    static const struct { const char *name, *text; } cases[] = {
        {"longline", NULL},
        {"manyline", NULL},
        {"lf-burst", NULL},
        {"crlf-burst", NULL},
        {"margin-lf", NULL},
        {"margin-crlf", NULL},
        {"ascii", "The quick brown fox jumps over the lazy dog. 0123456789\r\n"},
        {"utf8", "Türkçe: İstanbul — αβγ 日本語\r\n"},
        {"utf8var", "ab ér漢字 🙂 café x̂ 東京a\r\n"},
        {"cjk", "日本語漢字仮名交じり文中文測試終端輸出性能比較\r\n"},
        {"colors", "\033[31mred\033[0m \033[38;2;12;34;56mRGB\033[m\r\n"},
        {"fg-char", "\033[38;2;1;23;145ma\033[38;2;23;145;2mb\033[38;2;145;2;34mc"},
        {"fgbg-char", "\033[48;2;145;23;1m\033[38;2;1;23;145ma"
                      "\033[48;2;2;145;23m\033[38;2;23;145;2mb"
                      "\033[48;2;34;2;145m\033[38;2;145;2;34mc"},
        {"fg-rand", NULL},
        {"fgbg-rand", NULL},
        {"cursor", "\033[12;20Hhello\033[2DXY\033[K\033[1A\033[3C!\r\n"},
        {"erase", "\033[Hscreen\033[2J\033[Hnext\033[2K\r\n"},
        {"dch", "some text on a row\033[10D\033[4Ptail\r\n"},
        {"editor", NULL},
        {"random", NULL},
    };
    char buf[65536];
    double samples[64];
    if (reps > (int)(sizeof(samples) / sizeof(samples[0])))
        reps = sizeof(samples) / sizeof(samples[0]);
    bool matched = false, header = false;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        if (argc > 1 && strcmp(argv[1], "all") && strcmp(argv[1], cases[c].name)) continue;
        matched = true;
        size_t len = sizeof(buf);
        if (cases[c].text) {
            size_t n = strlen(cases[c].text);
            len = sizeof(buf) / n * n;
            for (size_t i = 0; i < len; i += n)
                memcpy(buf + i, cases[c].text, n);
        } else if (!strncmp(cases[c].name, "fg", 2)) {
            bool bg = !strncmp(cases[c].name, "fgbg", 4);
            unsigned state = seed;
            size_t n = 0;
            for (;;) {
                char seq[64];
                int k = 0;
                state = state * 1103515245u + 12345u;
                unsigned r = (state >> 8) & 255, g = (state >> 16) & 255, b = (state >> 24) & 255;
                if (bg)
                    k += snprintf(seq + k, sizeof(seq) - (size_t)k, "\033[48;2;%u;%u;%um", b, r, g);
                k += snprintf(seq + k, sizeof(seq) - (size_t)k, "\033[38;2;%u;%u;%um%c", r, g, b,
                              'a' + (int)(state % 26));
                if (n + (size_t)k > sizeof(buf)) break;
                memcpy(buf + n, seq, (size_t)k);
                n += (size_t)k;
            }
            len = n;
        } else if (strstr(cases[c].name, "-burst") ||
                   !strncmp(cases[c].name, "margin-", 7)) {
            bool crlf = strstr(cases[c].name, "crlf") != NULL;
            for (size_t i = 0; i < len; i++)
                buf[i] = crlf && (i & 1) == 0 ? '\r' : '\n';
        } else if (!strcmp(cases[c].name, "editor")) {
            size_t n = 0;
            for (int r = 1; r <= 40 && n + 140 < sizeof(buf); r++) {
                n += (size_t)snprintf(buf + n, sizeof(buf) - n, "\033[%d;1H", r);
                memset(buf + n, 'a' + r % 26, 110);
                n += 110;
            }
            len = n;
        } else {
            unsigned state = seed;
            unsigned choices = !strcmp(cases[c].name, "manyline") ? 27 : 26;
            bool random = !strcmp(cases[c].name, "random");
            for (size_t i = 0; i < len; i++) {
                state = state * 1103515245u + 12345u;
                unsigned pick = (state >> 16) % choices;
                buf[i] = random ? (char)(state >> 16) : pick == 26 ? '\n' : (char)('a' + pick);
            }
        }
        if (!header) {
            header = true;
            printf("# %-9s %6s %10s %10s %10s %8s\n",
                   "case", "chunk", "MiB/s med", "min", "max", "ns/B");
        }
        size_t chunks[2] = {chunk, 4096};
        int nchunks = 1;
        if (!chunk) {
            chunks[0] = len;
            nchunks = 2;
        }
        for (int k = 0; k < nchunks; k++) {
            size_t ck = chunks[k];
            const char *env_cols = getenv("WSAP_VTCORE_COLS");
            int cols_val = env_cols ? atoi(env_cols) : 120;
            struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){
                .size = { .x = cols_val, .y = 40 },
                .scrollback = BENCH_SCROLLBACK,
            });
            if (!t) {
                fprintf(stderr, "wsap_vtcore_new failed: WSAP_VTCORE_COLS must be 0 to %d\n", WSAP_VTCORE_MAX_COLS);
                return EXIT_FAILURE;
            }
            unsigned flags = !strcmp(cases[c].name, "manyline") ? WSAP_VTCORE_FEED_ONLCR : 0;
            if (!strncmp(cases[c].name, "margin-", 7)) {
                const char setup[] = "\033[5;36r\033[36;1H";
                wsap_vtcore_feed(t, setup, sizeof(setup) - 1, 0);
            } else if (strstr(cases[c].name, "-burst")) {
                const char setup[] = "\033[40;1H";
                wsap_vtcore_feed(t, setup, sizeof(setup) - 1, 0);
            }
            run_sample(t, buf, len, ck, 16, flags);
            for (int r = 0; r < reps; r++)
                samples[r] = run_sample(t, buf, len, ck, iterations, flags);
            bench_sort(samples, reps);
            double bytes = (double)len * iterations;
            double med = bytes / (1024 * 1024) / samples[reps / 2];
            double min = bytes / (1024 * 1024) / samples[reps - 1];
            double max = bytes / (1024 * 1024) / samples[0];
            printf("%-9s %6zu %10.2f %10.2f %10.2f %8.3f\n",
                   cases[c].name, ck, med, min, max,
                   samples[reps / 2] * 1e9 / bytes);
            wsap_vtcore_free(t);
        }
    }
    if (!matched) fprintf(stderr, "unknown benchmark case: %s\n", argv[1]);
    return matched ? EXIT_SUCCESS : EXIT_FAILURE;
}
