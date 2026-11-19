#define _POSIX_C_SOURCE 200809L
#include "font/font.h"
#include "font/glyph_cache.h"
#include "font/backends/freetype/internal.h"

#include <stdio.h>
#include <time.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

static const struct { uint32_t lo, hi; } scan_ranges[] = {
    { 0x20, 0x10000 },
    { 0x1F000, 0x1FB00 },
};

static const struct { uint32_t lo, hi, partner; } run_ranges[] = {
    { 0x20, 0x10000, 'a' },
    { 0x2E80, 0xA000, 0x4E00 },
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static long long heap_bytes(void)
{
#if defined(__GLIBC__)
    struct mallinfo2 m = mallinfo2();
    return (long long)(m.uordblks + m.hblkhd);
#else
    return 0;
#endif
}

static uint64_t mix(uint64_t h, uint64_t v)
{
    return (h ^ v) * UINT64_C(0x100000001b3);
}

static bool is_surrogate(uint32_t cp)
{
    return cp >= 0xD800 && cp <= 0xDFFF;
}

static uint64_t scan_all(unsigned *count, unsigned *uncovered)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t r = 0; r < sizeof(scan_ranges) / sizeof(scan_ranges[0]); r++)
        for (uint32_t cp = scan_ranges[r].lo; cp < scan_ranges[r].hi; cp++) {
            if (is_surrogate(cp))
                continue;
            uint64_t hash;
            int dead_at;
            uint64_t cfg = ft_scan_run(&cp, 1, 0, &hash, &dead_at);
            (*count)++;
            *uncovered += dead_at >= 0;
            h = mix(mix(h, cfg), dead_at >= 0);
        }
    return h;
}

static uint64_t check_runs(unsigned *count, unsigned *covered)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t r = 0; r < sizeof(run_ranges) / sizeof(run_ranges[0]); r++)
        for (uint32_t cp = run_ranges[r].lo; cp < run_ranges[r].hi; cp++) {
            if (is_surrogate(cp))
                continue;
            uint32_t run[2] = { cp, run_ranges[r].partner };
            bool ok = run_coverable(run, 2);
            (*count)++;
            *covered += ok;
            h = mix(h, ok);
        }
    return h;
}

int main(void)
{
    if (!font_system_init())
        return 1;
    static struct glyph glyph;
    font_instance_render_glyph(font_instance_default(), 'a', 0, &glyph);
    glyph_release(&glyph);

    double start = now();
    if (!fallback_init())
        return 1;
    printf("fallback init  %.3f ms\n", (now() - start) * 1e3);

    long long heap = heap_bytes();
    for (int pass = 1; pass <= 2; pass++) {
        unsigned count = 0, uncovered = 0;
        start = now();
        uint64_t h = scan_all(&count, &uncovered);
        printf("scan pass %d    %.3f ms cps=%u uncovered=%u results=%016llx\n",
               pass, (now() - start) * 1e3, count, uncovered, (unsigned long long)h);
    }

    unsigned count = 0, covered = 0;
    start = now();
    uint64_t h = check_runs(&count, &covered);
    printf("runs           %.3f ms runs=%u covered=%u results=%016llx\n",
           (now() - start) * 1e3, count, covered, (unsigned long long)h);
#if defined(__GLIBC__)
    printf("heap growth    %lld KiB\n", (heap_bytes() - heap) / 1024);
#endif

    font_system_shutdown();
    return 0;
}
