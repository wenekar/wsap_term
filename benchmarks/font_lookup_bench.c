#define _POSIX_C_SOURCE 200809L
#include "font/font.h"
#include "font/glyph_cache.h"

#include <stdio.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
    double start = now();
    if (!font_system_init())
        return 1;
    struct font_instance *fi = font_instance_default();
    printf("font initialization: %.6f s\n", now() - start);

    static struct glyph glyph;
    for (int pass = 0; pass < 2; pass++) {
        start = now();
        unsigned count = 0;
        for (unsigned i = 0; i < 4096; i++) {
            uint32_t cp = 0x800 + ((i * 7919u) % 0x108000);
            if (cp >= 0xd800 && cp <= 0xdfff)
                continue;
            font_instance_render_glyph(fi, cp, 0, &glyph);
            count++;
        }
        printf("pass %d: %u glyphs, %.6f s\n", pass + 1, count, now() - start);
    }
    glyph_release(&glyph);
    font_system_shutdown();
    return 0;
}
