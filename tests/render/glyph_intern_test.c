#include "font/glyph/intern.h"
#include <wsap_vtcore/unicode/grapheme.h>
#include <assert.h>
#include <stdio.h>

static void test_glyph_intern_expand_zero_cap(void)
{
    uint32_t out[1] = { 0xdeadbeef };
    assert(glyph_intern_expand('a', out, 0) == 0);
    assert(out[0] == 0xdeadbeef);

    uint32_t cps[2] = { 'x', 0x0301 };
    uint32_t h = glyph_intern(cps, 2);
    assert(wsap_vtcore_grapheme_is_handle(h));
    assert(glyph_intern_expand(h, out, 0) == 0);
    assert(out[0] == 0xdeadbeef);
    assert(glyph_intern_expand(WSAP_VTCORE_GRAPHEME_HANDLE_BASE + 0xffff, out, 0) == 0);
}

static void test_glyph_intern_epoch_changes_on_reuse(void)
{
    uint32_t before = glyph_intern_epoch();

    uint32_t same[2] = { 'q', 0x0302 };
    glyph_intern(same, 2);
    glyph_intern(same, 2);
    assert(glyph_intern_epoch() == before);

    /* More distinct clusters than the table holds forces a reset. */
    for (uint32_t i = 0; i < 70000; i++) {
        uint32_t cps[2] = { 0x41 + i % 26, 0x300 + i / 26 };
        glyph_intern(cps, 2);
    }
    assert(glyph_intern_epoch() == before + 1);
}

int main(void)
{
    test_glyph_intern_expand_zero_cap();
    test_glyph_intern_epoch_changes_on_reuse();
    puts("PASS: glyph intern tests");
    return 0;
}
