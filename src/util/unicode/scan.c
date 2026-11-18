#include "util/unicode/utf8.h"

#include <stdint.h>
#include <string.h>

/* One word holds eight bytes. The high bit of each byte carries the result,
 * so no compare borrows across a byte boundary. */
#define UNICODE_BYTE_HIGH 0x8080808080808080ull
#define UNICODE_BYTE_ONES 0x0101010101010101ull

/* Sets the high bit of each byte that is below 0x20 or at or above `limit`.
 * `limit` is the same value in every byte and stays at or below 0x80.
 * The caller clears the high bits of high bytes by the last term. */
static inline uint64_t unicode_outside_bytes(uint64_t word, uint64_t limit)
{
    uint64_t subject = word | UNICODE_BYTE_HIGH;
    uint64_t below = ~(subject - 0x20ull * UNICODE_BYTE_ONES) & UNICODE_BYTE_HIGH;
    uint64_t above = (subject - limit) & UNICODE_BYTE_HIGH;
    return below | above | (word & UNICODE_BYTE_HIGH);
}

size_t unicode_utf8_printable_span(const char *buf, size_t len, unsigned char hi)
{
    if (!buf || hi <= 0x20)
        return 0;
    size_t i = 0;
    /* A byte above 0x80 is always outside the range, and the word kernel
     * cannot compare bytes above 0x80. So the word loop handles hi to 0x80,
     * which is the whole range in use. */
    if (hi <= 0x80) {
        uint64_t limit = (uint64_t)hi * UNICODE_BYTE_ONES;
        for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
            uint64_t word;
            memcpy(&word, buf + i, sizeof(word));
            if (unicode_outside_bytes(word, limit) == 0)
                continue;
            for (size_t k = 0; k < sizeof(uint64_t); k++) {
                unsigned char c = (unsigned char)buf[i + k];
                if (c < 0x20 || c >= hi)
                    return i + k;
            }
        }
    }
    for (; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 || c >= hi)
            return i;
    }
    return len;
}
