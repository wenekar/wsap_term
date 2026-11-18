/*
 * The base Unicode module is a copy of the wsap_vtcore one.
 * The copies are separate because the term subproject builds alone.
 * This test compares the two copies over a corpus that covers the code point
 * range, the byte sequences at the UTF-8 boundaries, and random buffers.
 */

#include "util/unicode/grapheme.h"
#include "util/unicode/utf8.h"
#include "util/unicode/width.h"

#include <wsap_vtcore/unicode/grapheme.h>
#include <wsap_vtcore/unicode/unicode.h>
#include <wsap_vtcore/unicode/utf8.h>
#include <wsap_vtcore/wsap_vtcore.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CORPUS_BUFFER 64
#define CORPUS_ROUNDS 200000

static uint32_t corpus_seed = 0x9E3779B9u;

static uint32_t corpus_next(void)
{
    corpus_seed ^= corpus_seed << 13;
    corpus_seed ^= corpus_seed >> 17;
    corpus_seed ^= corpus_seed << 5;
    return corpus_seed;
}

static void fail_cp(const char *what, uint32_t cp)
{
    fprintf(stderr, "unicode parity: %s differs at U+%04X\n", what, cp);
    abort();
}

static void fail_buffer(const char *what, size_t len, unsigned char hi)
{
    fprintf(stderr, "unicode parity: %s differs at length %zu with limit 0x%02X\n",
            what, len, hi);
    abort();
}

static void check_same_width(uint32_t cp)
{
    if (unicode_width(cp) != wsap_vtcore_unicode_width(cp))
        fail_cp("width", cp);
}

static void check_same_predicates(uint32_t cp)
{
    if (unicode_emoji_presentation(cp) != wsap_vtcore_unicode_emoji_presentation(cp))
        fail_cp("emoji presentation", cp);
    if (unicode_extended_pictographic(cp) != wsap_vtcore_unicode_extended_pictographic(cp))
        fail_cp("extended pictographic", cp);
    if (unicode_spacing_mark(cp) != wsap_vtcore_unicode_spacing_mark(cp))
        fail_cp("spacing mark", cp);
}

static void check_same_grapheme(uint32_t cp)
{
    if (unicode_grapheme_is_handle(cp) != wsap_vtcore_grapheme_is_handle(cp))
        fail_cp("grapheme handle", cp);
    if (unicode_grapheme_regional_indicator(cp) != wsap_vtcore_grapheme_regional_indicator(cp))
        fail_cp("regional indicator", cp);
    if (unicode_grapheme_emoji_modifier(cp) != wsap_vtcore_grapheme_emoji_modifier(cp))
        fail_cp("emoji modifier", cp);
}

static void test_code_point_tables(void)
{
    assert(strcmp(UNICODE_DATA_VERSION, WSAP_VTCORE_UNICODE_DATA_VERSION) == 0);
    assert(UNICODE_GRAPHEME_MAX_LEN == WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(UNICODE_GRAPHEME_HANDLE_BASE == WSAP_VTCORE_GRAPHEME_HANDLE_BASE);
    assert(UNICODE_UTF8_MAX_BYTES == WSAP_VTCORE_UTF8_MAX_BYTES);

    for (uint32_t cp = 0; cp <= 0x110000u; cp++) {
        check_same_width(cp);
        check_same_predicates(cp);
        check_same_grapheme(cp);
    }
}

static void check_same_decode(const char *bytes, size_t len)
{
    uint32_t want = 0;
    uint32_t got = 0;
    int want_len = wsap_vtcore_utf8_decode_one(bytes, len, &want);
    int got_len = unicode_utf8_decode_one(bytes, len, &got);
    if (got_len != want_len)
        fail_buffer("decode length", len, 0);
    if (want_len > 0 && got != want)
        fail_buffer("decoded code point", len, 0);
}

static void check_same_encode(uint32_t cp)
{
    char want[WSAP_VTCORE_UTF8_MAX_BYTES];
    char got[UNICODE_UTF8_MAX_BYTES];
    int want_len = wsap_vtcore_utf8_encode(cp, want);
    int got_len = unicode_utf8_encode(cp, got);
    if (got_len != want_len)
        fail_cp("encode length", cp);
    if (memcmp(got, want, (size_t)want_len) != 0)
        fail_cp("encoded bytes", cp);
    check_same_decode(want, (size_t)want_len);
}

static void test_decode_and_encode(void)
{
    for (uint32_t cp = 0; cp <= 0x110000u; cp++)
        check_same_encode(cp);

    /* Every one- and two-byte sequence, valid or not. */
    char seq[WSAP_VTCORE_UTF8_MAX_BYTES];
    for (int b0 = 0; b0 < 256; b0++) {
        seq[0] = (char)b0;
        check_same_decode(seq, 1);
        for (int b1 = 0; b1 < 256; b1++) {
            seq[1] = (char)b1;
            check_same_decode(seq, 2);
        }
    }

    /* Boundary bytes in the longer forms, then random forms. */
    static const unsigned char edges[] = {
        0x00, 0x20, 0x7f, 0x80, 0x8f, 0x9f, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf,
        0xe0, 0xed, 0xef, 0xf0, 0xf4, 0xf5, 0xff,
    };
    for (size_t i = 0; i < sizeof(edges); i++) {
        for (size_t j = 0; j < sizeof(edges); j++) {
            for (size_t k = 0; k < sizeof(edges); k++) {
                seq[0] = (char)edges[i];
                seq[1] = (char)edges[j];
                seq[2] = (char)edges[k];
                check_same_decode(seq, 3);
                seq[3] = (char)edges[(i + j + k) % sizeof(edges)];
                check_same_decode(seq, 4);
            }
        }
    }
    for (int round = 0; round < CORPUS_ROUNDS; round++) {
        size_t len = 3 + corpus_next() % 2;
        for (size_t i = 0; i < len; i++)
            seq[i] = (char)corpus_next();
        check_same_decode(seq, len);
    }
}

static void test_sanitize(void)
{
    for (int round = 0; round < CORPUS_ROUNDS / 4; round++) {
        char src[CORPUS_BUFFER];
        size_t len = corpus_next() % (CORPUS_BUFFER - 1);
        for (size_t i = 0; i < len; i++) {
            /* Mostly valid UTF-8, with invalid bytes mixed in. */
            src[i] = (corpus_next() % 8 == 0) ? (char)corpus_next()
                                              : (char)(' ' + corpus_next() % 0x5e);
        }
        src[len] = '\0';

        char want[CORPUS_BUFFER];
        char got[CORPUS_BUFFER];
        wsap_vtcore_utf8_sanitize(want, sizeof(want), src);
        unicode_utf8_sanitize(got, sizeof(got), src);
        if (strcmp(got, want) != 0)
            fail_buffer("sanitize", len, 0);

        char *want_dup = wsap_vtcore_utf8_sanitize_dup(src);
        char *got_dup = unicode_utf8_sanitize_dup(src);
        assert(want_dup && got_dup);
        if (strcmp(got_dup, want_dup) != 0)
            fail_buffer("sanitize_dup", len, 0);
        free(want_dup);
        free(got_dup);
    }
}

static void test_streaming_decode(void)
{
    for (int round = 0; round < CORPUS_ROUNDS / 4; round++) {
        char src[CORPUS_BUFFER];
        size_t len = 1 + corpus_next() % (CORPUS_BUFFER - 1);
        for (size_t i = 0; i < len; i++)
            src[i] = (char)corpus_next();

        uint32_t want_state = WSAP_VTCORE_UTF8_ACCEPT, want_cp = 0;
        uint32_t got_state = UNICODE_UTF8_ACCEPT, got_cp = 0;
        for (size_t i = 0; i < len; i++) {
            uint32_t want_rc = wsap_vtcore_utf8_decode(&want_state, &want_cp, (unsigned char)src[i]);
            uint32_t got_rc = unicode_utf8_decode(&got_state, &got_cp, (unsigned char)src[i]);
            assert(got_rc == want_rc);
            assert(got_state == want_state);
            assert(got_cp == want_cp);
        }

        size_t off = 0;
        while (off < len) {
            size_t chunk = 1 + corpus_next() % 5;
            if (off + chunk > len)
                chunk = len - off;
            uint32_t want_value = 0;
            uint32_t got_value = 0;
            int want_n = wsap_vtcore_utf8_decode_ground(&want_state, &want_cp,
                                                       src + off, chunk, &want_value);
            int got_n = unicode_utf8_decode_ground(&got_state, &got_cp,
                                                   src + off, chunk, &got_value);
            assert(got_n == want_n);
            assert(got_value == want_value);
            assert(got_state == want_state);
            assert(got_cp == want_cp);
            off += chunk;
        }
    }
}

static void test_printable_span(void)
{
    assert(unicode_utf8_printable_span(NULL, 10, 0x7f) == 0);
    assert(unicode_utf8_printable_span("abc", 3, 0x20) == 0);

    char buf[CORPUS_BUFFER];
    for (int hi = 0; hi < 256; hi++) {
        for (size_t len = 0; len <= CORPUS_BUFFER; len += 7) {
            for (int round = 0; round < 40; round++) {
                for (size_t i = 0; i < len; i++) {
                    unsigned kind = corpus_next() % 4;
                    if (kind == 0)
                        buf[i] = (char)corpus_next();
                    else if (kind == 1)
                        buf[i] = (char)(' ' + corpus_next() % 0x5f);
                    else if (kind == 2)
                        buf[i] = (char)('a' + corpus_next() % 26);
                    else
                        buf[i] = (char)(0x20 + (int)(corpus_next() % 3) + hi - 1);
                }
                size_t want = wsap_vtcore_scan_printable_span(buf, len, (unsigned char)hi);
                size_t got = unicode_utf8_printable_span(buf, len, (unsigned char)hi);
                if (got != want)
                    fail_buffer("printable span", len, (unsigned char)hi);
            }
        }
    }
}

int main(void)
{
    test_code_point_tables();
    test_decode_and_encode();
    test_sanitize();
    test_streaming_decode();
    test_printable_span();

    printf("PASS: unicode parity tests\n");
    return 0;
}
