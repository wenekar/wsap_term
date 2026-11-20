#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

bool yasl_private_utf8_validate(const char *str, size_t len)
{
    if (!str) return len == 0;
    const unsigned char *p = (const unsigned char *)str;
    const unsigned char *end = p + len;

    while (p < end) {
        while ((size_t)(end - p) >= 8) {
            uint64_t v;
            memcpy(&v, p, 8);
            if (v & 0x8080808080808080ull)
                break;
            p += 8;
        }
        if (p >= end) break;

        if (likely(*p < 0x80)) {
            p++;
        } else if ((*p & 0xe0) == 0xc0) {
            if (unlikely(end - p < 2 || (p[1] & 0xc0) != 0x80 || (*p & 0x1e) == 0))
                return false;
            p += 2;
        } else if ((*p & 0xf0) == 0xe0) {
            if (unlikely(end - p < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80))
                return false;
            uint32_t cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            if (unlikely(cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)))
                return false;
            p += 3;
        } else if ((*p & 0xf8) == 0xf0) {
            if (unlikely(end - p < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80))
                return false;
            uint32_t cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            if (unlikely(cp < 0x10000 || cp > 0x10ffff))
                return false;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

char *yasl_private_utf8_from_latin1(const char *str, size_t len, size_t *out_len)
{
    if (len > (SIZE_MAX - 1) / 2) return NULL;
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch < 0x80) out[n++] = (char)ch;
        else {
            out[n++] = (char)(0xc0 | (ch >> 6));
            out[n++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

char *yasl_private_utf8_to_latin1(const char *str, size_t *out_len)
{
    char *out = malloc(strlen(str) + 1);
    if (!out) return NULL;
    const unsigned char *p = (const unsigned char *)str;
    size_t n = 0;
    while (*p) {
        if (*p < 0x80) out[n++] = (char)*p++;
        else if ((*p == 0xc2 || *p == 0xc3) && (p[1] & 0xc0) == 0x80) {
            out[n++] = (char)(((*p & 3) << 6) | (p[1] & 0x3f));
            p += 2;
        } else {
            free(out);
            return NULL;
        }
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

char *yasl_private_utf8_sanitize_dup(const char *str)
{
    if (unlikely(!str)) return NULL;
    size_t len = strlen(str);
    if (likely(yasl_private_utf8_validate(str, len))) {
        char *out = malloc(len + 1);
        if (unlikely(!out)) return NULL;
        memcpy(out, str, len + 1);
        return out;
    }

    char *out = malloc(len + 1);
    if (unlikely(!out)) return NULL;

    const unsigned char *p = (const unsigned char *)str;
    size_t o = 0;
    while (*p) {
        if (*p < 0x80) {
            out[o++] = (char)*p++;
        } else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80 && (*p & 0x1e)) {
            out[o++] = (char)*p++;
            out[o++] = (char)*p++;
        } else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            uint32_t cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            if (cp >= 0x800 && !(cp >= 0xd800 && cp <= 0xdfff)) {
                out[o++] = (char)*p++;
                out[o++] = (char)*p++;
                out[o++] = (char)*p++;
            } else {
                out[o++] = '?';
                p++;
            }
        } else if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
            uint32_t cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            if (cp >= 0x10000 && cp <= 0x10ffff) {
                out[o++] = (char)*p++;
                out[o++] = (char)*p++;
                out[o++] = (char)*p++;
                out[o++] = (char)*p++;
            } else {
                out[o++] = '?';
                p++;
            }
        } else {
            out[o++] = '?';
            p++;
        }
    }
    out[o] = '\0';
    return out;
}
