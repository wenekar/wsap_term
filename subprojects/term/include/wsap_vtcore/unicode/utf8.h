#ifndef WSAP_VTCORE_UNICODE_UTF8_H
#define WSAP_VTCORE_UNICODE_UTF8_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define WSAP_VTCORE_UTF8_ACCEPT 0
#define WSAP_VTCORE_UTF8_REJECT 1

#define WSAP_VTCORE_UTF8_3BYTE_MIN 0x800u
#define WSAP_VTCORE_UTF8_SURROGATE_LO 0xd800u
#define WSAP_VTCORE_UTF8_SURROGATE_HI 0xdfffu

static const uint8_t wsap_vtcore_utf8d[] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
  8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3,
  0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,
  0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1,
  1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,
  1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1,
  1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

static inline uint32_t wsap_vtcore_utf8_decode(uint32_t *state, uint32_t *codep, uint32_t byte) {
  byte &= 0xff;
  uint32_t type = wsap_vtcore_utf8d[byte];

  *codep = (*state != WSAP_VTCORE_UTF8_ACCEPT) ?
    (byte & 0x3fu) | (*codep << 6) :
    (0xff >> type) & (byte);

  *state = wsap_vtcore_utf8d[256 + *state*16 + type];
  return *state;
}

static const uint8_t wsap_vtcore_utf8_acc_state[256] = {
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 5, 3, 3,
6, 7, 7, 7, 8, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const uint8_t wsap_vtcore_utf8_acc_codep[256] = {
0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static inline int wsap_vtcore_utf8_decode_ground(uint32_t *state, uint32_t *codep,
                                     const char *buf, size_t len, uint32_t *cp) {
    uint32_t st = *state;
    uint32_t cpv = *codep;
    int n = 0;
    *cp = 0;
    if (len == 0)
        return 0;
    do {
        unsigned char c = (unsigned char)buf[n];
        if (c < 0x20) {
            if (st != WSAP_VTCORE_UTF8_ACCEPT) *cp = 0xFFFD;
            st = WSAP_VTCORE_UTF8_ACCEPT;
            break;
        }
        uint32_t before = st;
        n++;
        if (st == WSAP_VTCORE_UTF8_ACCEPT) {
            cpv = wsap_vtcore_utf8_acc_codep[c];
            st = wsap_vtcore_utf8_acc_state[c];
        } else {
            uint32_t type = wsap_vtcore_utf8d[c];
            cpv = (c & 0x3fu) | (cpv << 6);
            st = wsap_vtcore_utf8d[256 + st*16 + type];
        }
        if (st == WSAP_VTCORE_UTF8_ACCEPT) { *cp = cpv; break; }
        if (st == WSAP_VTCORE_UTF8_REJECT) {
            st = WSAP_VTCORE_UTF8_ACCEPT;
            *cp = 0xFFFD;
            if (before != WSAP_VTCORE_UTF8_ACCEPT) n--;
            break;
        }
    } while ((size_t)n < len);
    *state = st;
    *codep = cpv;
    return n;
}

static inline int
wsap_vtcore_utf8_decode_one(const char *buf, size_t len, uint32_t *codep)
{
    const unsigned char *s = (const unsigned char *)buf;
    unsigned c0, c1, c2, c3;
    uint32_t cp;

    if (len == 0)
        return 0;
    c0 = s[0];
    if (c0 < 0x80) {
        *codep = c0;
        return 1;
    }
    if (c0 < 0xc2 || c0 > 0xf4)
        return 0;

    if (c0 <= 0xdf) {
        if (len < 2)
            return 0;
        c1 = s[1];
        if ((c1 & 0xc0) != 0x80)
            return 0;
        *codep = ((c0 & 0x1f) << 6) | (c1 & 0x3f);
        return 2;
    }

    if (c0 <= 0xef) {
        if (len < 3)
            return 0;
        c1 = s[1];
        c2 = s[2];
        if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80)
            return 0;
        cp = ((c0 & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (c2 & 0x3f);
        if (cp < WSAP_VTCORE_UTF8_3BYTE_MIN ||
            (cp >= WSAP_VTCORE_UTF8_SURROGATE_LO && cp <= WSAP_VTCORE_UTF8_SURROGATE_HI))
            return 0;
        *codep = cp;
        return 3;
    }

    if (len < 4)
        return 0;
    c1 = s[1];
    c2 = s[2];
    c3 = s[3];
    if ((c1 & 0xc0) != 0x80 || (c2 & 0xc0) != 0x80 ||
        (c3 & 0xc0) != 0x80)
        return 0;
    cp = ((c0 & 0x07) << 18) | ((c1 & 0x3f) << 12) |
         ((c2 & 0x3f) << 6) | (c3 & 0x3f);
    if (cp < 0x10000 || cp > 0x10ffff)
        return 0;
    *codep = cp;
    return 4;
}

static inline void
wsap_vtcore_utf8_sanitize(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return;
    size_t i = 0, len = strlen(src), off = 0;
    while (off < len && i + 1 < cap) {
        uint32_t cp;
        int n = wsap_vtcore_utf8_decode_one(src + off, len - off, &cp);
        if (n <= 0 || i + (size_t)n >= cap) {
            off++;
            continue;
        }
        memcpy(dst + i, src + off, (size_t)n);
        i += (size_t)n;
        off += (size_t)n;
    }
    dst[i] = '\0';
}

static inline char *wsap_vtcore_utf8_sanitize_dup(const char *src)
{
    size_t cap = strlen(src) + 1;
    char *dst = (char *)malloc(cap);
    if (dst)
        wsap_vtcore_utf8_sanitize(dst, cap, src);
    return dst;
}

#define WSAP_VTCORE_UTF8_MAX_BYTES 4
static inline int wsap_vtcore_utf8_encode(uint32_t cp, char *out)
{
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

#endif
