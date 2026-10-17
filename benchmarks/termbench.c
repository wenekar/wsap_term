#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <unistd.h>

static char pattern[32 * 1024 * 1024];
static const char *const names[] = { "ManyLine", "LongLine", "FGPerChar", "FGBGPerChar" };

/* Prepare changing frames before timing the writes. */
static size_t make_pattern(int kind)
{
    size_t n = 0;
    if (kind < 2) {
        unsigned state = 42;
        for (; n < 65536; n++) {
            state = state * 1103515245u + 12345u;
            unsigned pick = (state >> 16) % (kind == 0 ? 27 : 26);
            pattern[n] = pick == 26 ? '\n' : (char)('a' + pick);
        }
        return n;
    }
    for (unsigned frame = 0; frame < 256; frame++) {
        for (unsigned y = 0; y < 24; y++) {
            n += (size_t)snprintf(pattern + n, sizeof(pattern) - n, "\033[%u;1H", y + 1);
            for (unsigned x = 0; x < 80; x++) {
                unsigned r = frame, g = (frame + y) & 255, b = (frame + y + x) & 255;
                if (kind == 3)
                    n += (size_t)snprintf(pattern + n, sizeof(pattern) - n,
                                         "\033[48;2;%u;%u;%um", b, g, r);
                n += (size_t)snprintf(pattern + n, sizeof(pattern) - n,
                                     "\033[38;2;%u;%u;%um%c", r, g, b,
                                     'a' + (int)((frame + x + y) % 26));
            }
        }
    }
    return n;
}

static void write_all(const char *data, size_t len)
{
    while (len) {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            perror("write");
            exit(EXIT_FAILURE);
        }
        data += n;
        len -= (size_t)n;
    }
}

int main(int argc, char **argv)
{
    const char *size = argc > 1 ? argv[1] : "small";
    const char *filter = argc > 2 ? argv[2] : NULL;
    uint64_t bytes = !strcmp(size, "small") ? 1ull << 20
                   : !strcmp(size, "normal") ? 1ull << 30
                   : !strcmp(size, "large") ? 16ull << 30 : 0;
    if (argc > 3 || !bytes) {
        fprintf(stderr, "usage: %s [small|normal|large [case]]\n", argv[0]);
        return 1;
    }
    const char *path = getenv("TERMBENCH_REPORT");
    FILE *report = path ? fopen(path, "w") : NULL;
    if (path && !report) {
        perror(path);
        return 1;
    }
    double seconds[4] = {0}, total = 0;
    int count = 0;
    for (int c = 0; c < 4; c++) {
        if (filter && strcasecmp(filter, names[c]))
            continue;
        size_t len = make_pattern(c), pos = 0;
        uint64_t start = bench_now_ns();
        for (uint64_t left = bytes; left;) {
            size_t chunk = len - pos;
            if (chunk > 65536) chunk = 65536;
            if (chunk > left) chunk = (size_t)left;
            write_all(pattern + pos, chunk);
            pos = (pos + chunk) % len;
            left -= chunk;
        }
        seconds[c] = (double)(bench_now_ns() - start) * 1e-9;
        total += seconds[c];
        count++;
    }
    if (!count) {
        fprintf(stderr, "unknown benchmark case: %s\n", filter);
        if (report) fclose(report);
        return 1;
    }
    write_all("\033[0m\n", 5);
    struct utsname host;
    if (uname(&host) < 0) {
        perror("uname");
        if (report) fclose(report);
        return 1;
    }
    FILE *outputs[] = { stdout, report };
    for (int i = 0; i < 2; i++) {
        FILE *out = outputs[i];
        if (!out) continue;
        fprintf(out, "Host: %s %s\n", host.machine, host.sysname);
        for (int c = 0; c < 4; c++)
            if (seconds[c] > 0)
                fprintf(out, "%s: %.4fs (%.4fgb/s)\n", names[c], seconds[c],
                        (double)bytes / (1ull << 30) / seconds[c]);
        fprintf(out, "termbench-v3 %s: %.4fs (%.4fgb/s)\n", size, total,
                (double)bytes * count / (1ull << 30) / total);
    }
    int failed = fflush(stdout) != 0 || ferror(stdout);
    if (report) failed |= fclose(report) != 0;
    return failed;
}
