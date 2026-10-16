#include "wsap_vtcore/unicode/utf8.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_utf8_decode_one(void)
{
    static const struct { const char *bytes; int len; uint32_t cp; } valid[] = {
        {"\0", 1, 0}, {"\x7f", 1, 0x7f},
        {"\xc2\x80", 2, 0x80}, {"\xdf\xbf", 2, 0x7ff},
        {"\xe0\xa0\x80", 3, 0x800}, {"\xed\x9f\xbf", 3, 0xd7ff},
        {"\xee\x80\x80", 3, 0xe000}, {"\xef\xbf\xbf", 3, 0xffff},
        {"\xf0\x90\x80\x80", 4, 0x10000},
        {"\xf4\x8f\xbf\xbf", 4, 0x10ffff},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
        uint32_t cp = 0xdeadbeef;
        for (int n = 0; n < valid[i].len; n++) {
            int r = wsap_vtcore_utf8_decode_one(valid[i].bytes, n, &cp);
            assert(r == 0);
            assert(cp == 0xdeadbeef);
        }
        int r = wsap_vtcore_utf8_decode_one(valid[i].bytes, valid[i].len, &cp);
        assert(r == valid[i].len);
        assert(cp == valid[i].cp);
        char followed[8];
        memcpy(followed, valid[i].bytes, valid[i].len);
        memcpy(followed + valid[i].len, "next", 4);
        r = wsap_vtcore_utf8_decode_one(followed, valid[i].len + 4, &cp);
        assert(r == valid[i].len);
        assert(cp == valid[i].cp);
    }
    static const char *const invalid[] = {
        "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2X",
        "\xe0\x9f\xbf", "\xed\xa0\x80", "\xed\xbf\xbf", "\xe1\x80X",
        "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xf8\x88\x80\x80\x80", "\xff", "\xf1\x80\x80X",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        uint32_t cp = 0xdeadbeef;
        int r = wsap_vtcore_utf8_decode_one(invalid[i], strlen(invalid[i]), &cp);
        assert(r == 0);
        assert(cp == 0xdeadbeef);
    }
}

static void test_scalar_roundtrips(void)
{
    for (uint32_t cp = 0; cp <= 0x10ffff; cp++) {
        if (cp >= 0xd800 && cp <= 0xdfff) continue;
        char bytes[WSAP_VTCORE_UTF8_MAX_BYTES];
        int len = wsap_vtcore_utf8_encode(cp, bytes);
        uint32_t scalar = 0, streamed = 0, state = WSAP_VTCORE_UTF8_ACCEPT;
        int r = wsap_vtcore_utf8_decode_one(bytes, (size_t)len, &scalar);
        assert(r == len);
        assert(scalar == cp);
        for (int i = 0; i < len; i++) {
            wsap_vtcore_utf8_decode(&state, &streamed, (unsigned char)bytes[i]);
            if (i + 1 < len) assert(state != WSAP_VTCORE_UTF8_ACCEPT && state != WSAP_VTCORE_UTF8_REJECT);
        }
        assert(state == WSAP_VTCORE_UTF8_ACCEPT && streamed == cp);
    }
    static const uint32_t invalid[] = {0xd800, 0xdfff, 0x110000, UINT32_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        char bytes[WSAP_VTCORE_UTF8_MAX_BYTES];
        int len = wsap_vtcore_utf8_encode(invalid[i], bytes);
        uint32_t cp = 0;
        int r = wsap_vtcore_utf8_decode_one(bytes, len, &cp);
        assert(len == 3 && r == len && cp == 0xfffd);
    }
}

static void test_utf8_sanitize(void)
{
    char dst[64];

    wsap_vtcore_utf8_sanitize(dst, sizeof(dst), "hello \xe2\x82\xac\xf0\x9f\x98\x80");
    assert(!strcmp(dst, "hello \xe2\x82\xac\xf0\x9f\x98\x80"));

    wsap_vtcore_utf8_sanitize(dst, sizeof(dst), "a\xff\x80" "b\xed\xa0\x80" "c\xe2\x82");
    assert(!strcmp(dst, "abc"));

    wsap_vtcore_utf8_sanitize(dst, sizeof(dst), "");
    assert(!strcmp(dst, ""));
    wsap_vtcore_utf8_sanitize(dst, sizeof(dst), "\xff\xfe");
    assert(!strcmp(dst, ""));

    wsap_vtcore_utf8_sanitize(dst, 6, "ab\xe2\x82\xac" "cd");
    assert(!strcmp(dst, "ab\xe2\x82\xac"));
    wsap_vtcore_utf8_sanitize(dst, 5, "ab\xe2\x82\xac" "cd");
    assert(!strcmp(dst, "abcd"));
    memset(dst, 'x', sizeof(dst));
    wsap_vtcore_utf8_sanitize(dst, 4, "abcdef");
    assert(!strcmp(dst, "abc"));
    memset(dst, 'x', sizeof(dst));
    wsap_vtcore_utf8_sanitize(dst, 0, "abc");
    assert(dst[0] == 'x');
}

/* Each maximal invalid subpart gives U+FFFD. The byte that ends a subpart
 * stays unused. */
static void test_ground_invalid_subparts(void)
{
    uint32_t state = WSAP_VTCORE_UTF8_ACCEPT, codep = 0, cp = 0xdead;
    int n = wsap_vtcore_utf8_decode_ground(&state, &codep, "\xe2\x80\x1b\xac", 4, &cp);
    assert(n == 2);
    assert(cp == 0xFFFD);
    assert(state == WSAP_VTCORE_UTF8_ACCEPT);
    n = wsap_vtcore_utf8_decode_ground(&state, &codep, "\xac", 1, &cp);
    assert(n == 1);
    assert(cp == 0xFFFD);
    assert(state == WSAP_VTCORE_UTF8_ACCEPT);
    n = wsap_vtcore_utf8_decode_ground(&state, &codep, "\xe2\x82z", 3, &cp);
    assert(n == 2);
    assert(cp == 0xFFFD);
    assert(state == WSAP_VTCORE_UTF8_ACCEPT);
    n = wsap_vtcore_utf8_decode_ground(&state, &codep, "\xe2\x82", 2, &cp);
    assert(n == 2);
    assert(cp == 0);
    assert(state != WSAP_VTCORE_UTF8_ACCEPT);

    state = WSAP_VTCORE_UTF8_ACCEPT;
    codep = 0;
    n = wsap_vtcore_utf8_decode_ground(&state, &codep, "\xe2\x82\xac", 3, &cp);
    assert(n == 3);
    assert(cp == 0x20ac);
    assert(state == WSAP_VTCORE_UTF8_ACCEPT);
}

int main(void)
{
    test_utf8_decode_one();
    test_scalar_roundtrips();
    test_utf8_sanitize();
    test_ground_invalid_subparts();
    puts("PASS: Unicode codec tests");
    return 0;
}
