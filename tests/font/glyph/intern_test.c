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

int main(void)
{
    test_glyph_intern_expand_zero_cap();
    puts("PASS: glyph intern tests");
    return 0;
}
