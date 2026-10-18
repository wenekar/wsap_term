#ifndef WSAP_TEST_FLOOD_H
#define WSAP_TEST_FLOOD_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum flood_kind {
    FLOOD_BYTES, FLOOD_PRINTABLE, FLOOD_CODE, FLOOD_LOG, FLOOD_COLOR, FLOOD_TRUECOLOR
};

/* Triangle wave over 0..255. It gives smooth colour ramps without libm. */
static inline unsigned flood_wave(unsigned x)
{
    unsigned v = x % 510;
    return v < 255 ? v : 510 - v;
}

static inline size_t flood_chunk(enum flood_kind kind, uint32_t *seed, char *buf, size_t cap)
{
    static const char *const code[] = {
        "    if (s->stream && len >= 4096 || s->credit != 0) {\r\n",
        "        loop_cancel_timer(s->loop, &s->timer); // re-arm -> idle\r\n",
        "    for (int i = 0; i <= n; i++) x[i] = y[i] == z ? a : b;\r\n",
        "    return a != b && c >= d || e <=> f; /* ==> ok */\r\n",
        "fn main() -> Result<(), Error> { let x = |a| a >> 2; Ok(()) }\r\n",
        "    const auto &p = *it; assert(p->next != nullptr); ++it;\r\n",
    };
    size_t n = 0;
    unsigned phase = *seed >> 8, column = 0;
    while (n + 128 < cap) {
        *seed ^= *seed << 13;
        *seed ^= *seed >> 17;
        *seed ^= *seed << 5;
        uint32_t r = *seed;
        switch (kind) {
        case FLOOD_BYTES:
            buf[n++] = (char)r;
            break;
        case FLOOD_PRINTABLE:
            buf[n++] = r % 97 < 94 ? (char)('!' + r % 94) : (r % 97 == 96 ? '\n' : ' ');
            break;
        case FLOOD_CODE: {
            const char *line = code[r % (sizeof(code) / sizeof(code[0]))];
            size_t len = strlen(line);
            memcpy(buf + n, line, len);
            n += len;
            break;
        }
        case FLOOD_LOG:
            n += (size_t)snprintf(buf + n, cap - n,
                "2026-10-06T10:%02u:%02u.%03uZ INFO  req=%08x path=/api/v%u/items "
                "status=%u -> %uus\r\n", r % 60, (r >> 6) % 60, (r >> 12) % 1000,
                r, r % 3 + 1, 200 + r % 4 * 100, r % 9000);
            break;
        case FLOOD_COLOR:
            if (r % 4 == 0)
                n += (size_t)snprintf(buf + n, cap - n, "\033[38;5;%um", (r >> 8) & 255);
            if (r % 12 == 0)
                n += (size_t)snprintf(buf + n, cap - n, "\033[48;5;%um", (r >> 16) & 255);
            buf[n++] = ' ' + 1 + (char)((r >> 4) % 94);
            if (++column >= 100) {
                memcpy(buf + n, "\r\n", 2);
                n += 2;
                column = 0;
            }
            break;
        case FLOOD_TRUECOLOR:
            phase += 3;
            n += (size_t)snprintf(buf + n, cap - n, "\033[38;2;%u;%u;%um",
                                  flood_wave(phase), flood_wave(phase + 170),
                                  flood_wave(phase + 340));
            buf[n++] = ' ' + 1 + (char)((r >> 4) % 94);
            if (++column >= 100) {
                memcpy(buf + n, "\r\n", 2);
                n += 2;
                column = 0;
            }
            break;
        }
    }
    return n;
}

static const struct { enum flood_kind kind; const char *name; } flood_kinds[] = {
    { FLOOD_BYTES, "bytes" }, { FLOOD_PRINTABLE, "printable" },
    { FLOOD_CODE, "code" }, { FLOOD_LOG, "log" },
    { FLOOD_COLOR, "color" }, { FLOOD_TRUECOLOR, "truecolor" },
};

#endif
