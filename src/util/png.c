#include "util/png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PNG_STORED_MAX 65535u

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool write_chunk(FILE *f, const char type[4], const uint8_t *data,
                        size_t len)
{
    uint8_t head[8];
    put_be32(head, (uint32_t)len);
    memcpy(head + 4, type, 4);
    uint32_t crc = crc_update(0xFFFFFFFFu, head + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    uint8_t tail[4];
    put_be32(tail, crc);
    return fwrite(head, 1, 8, f) == 8 &&
           (!len || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}

bool png_write_xrgb(const char *path, const uint32_t *px,
                    struct vector2i size, int stride)
{
    int width = size.x;
    int height = size.y;
    if (!path || !px || width <= 0 || height <= 0 || stride < width ||
        width > (1 << 24) / 3 || height > (1 << 24))
        return false;
    crc_init();

    size_t row = 1 + (size_t)width * 3;
    size_t raw_len = row * (size_t)height;
    size_t blocks = raw_len / PNG_STORED_MAX + 1;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    if (z_len > 0x7FFFFFFFu)
        return false;
    uint8_t *z = malloc(z_len);
    uint8_t *raw = malloc(raw_len);
    if (!z || !raw) {
        free(z);
        free(raw);
        return false;
    }

    for (int y = 0; y < height; y++) {
        uint8_t *out = raw + (size_t)y * row;
        const uint32_t *in = px + (size_t)y * (size_t)stride;
        *out++ = 0;
        for (int x = 0; x < width; x++) {
            *out++ = (uint8_t)(in[x] >> 16);
            *out++ = (uint8_t)(in[x] >> 8);
            *out++ = (uint8_t)in[x];
        }
    }

    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len;) {
        size_t n = raw_len - off;
        if (n > PNG_STORED_MAX)
            n = PNG_STORED_MAX;
        z[o++] = off + n >= raw_len ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[off + i]) % 65521u;
            b = (b + a) % 65521u;
        }
        o += n;
        off += n;
    }
    put_be32(z + o, (b << 16) | a);
    o += 4;
    free(raw);

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)width);
    put_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(sig, 1, sizeof(sig), f) == sizeof(sig) &&
              write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) &&
              write_chunk(f, "IDAT", z, o) &&
              write_chunk(f, "IEND", NULL, 0);
    if (f && fclose(f) != 0)
        ok = false;
    free(z);
    return ok;
}
