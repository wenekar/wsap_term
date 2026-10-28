#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static void test_palette_bounds(void)
{
    uint32_t palette[WSAP_VTCORE_PALETTE_SIZE] = {
        [0] = 0xff123456u,
        [WSAP_VTCORE_PALETTE_FG] = 0xffabcdefu,
        [WSAP_VTCORE_PALETTE_CURSOR] = 0xff654321u,
    };
    struct wsap_vtcore_view view = { .palette = palette };

    assert(wsap_vtcore_view_color(&view, wsap_vtcore_color_index(0)) == 0xff123456u);
    assert(wsap_vtcore_view_color(&view, WSAP_VTCORE_COLOR_FG) == 0xffabcdefu);
    assert(wsap_vtcore_view_color(&view,
           WSAP_VTCORE_COLOR_SLOT | WSAP_VTCORE_PALETTE_CURSOR) == 0xff654321u);
    assert(wsap_vtcore_view_color(&view,
           WSAP_VTCORE_COLOR_SLOT | WSAP_VTCORE_PALETTE_SIZE) == 0);
    assert(wsap_vtcore_view_color(&view,
           WSAP_VTCORE_COLOR_SLOT | WSAP_VTCORE_COLOR_SLOT_INDEX) == 0);
}

static void test_missing_palette(void)
{
    struct wsap_vtcore_view empty = {0};
    wsap_vtcore_color rgb = wsap_vtcore_color_rgb(0x12, 0x34, 0x56);

    assert(wsap_vtcore_view_color(NULL, rgb) == 0xff123456u);
    assert(wsap_vtcore_view_color(&empty, rgb) == 0xff123456u);
    assert(wsap_vtcore_view_color(NULL, WSAP_VTCORE_COLOR_FG) == 0);
    assert(wsap_vtcore_view_color(&empty, WSAP_VTCORE_COLOR_FG) == 0);
}

static void test_malformed_cluster_handles(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 4, .y = 1 } };
    struct wsap_vtcore *term = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(term && snapshot);
    assert(wsap_vtcore_feed(term, "e\xcc\x81", 3, 0));
    assert(wsap_vtcore_snapshot_update(snapshot, term));

    uint32_t text = wsap_vtcore_snapshot_row(snapshot, 0)[0].text;
    uint32_t cps[1024];
    assert(wsap_vtcore_snapshot_cluster(snapshot, text, cps, 1024) == 2);
    assert(cps[0] == 'e' && cps[1] == 0x301);
    /* Malformed tokens must not read past captured grapheme data. */
    for (uint32_t offset = 1; offset <= 32; offset++)
        (void)wsap_vtcore_snapshot_cluster(snapshot, text + offset, cps, 1024);
    assert(wsap_vtcore_snapshot_cluster(snapshot, UINT32_MAX, cps, 1024) == 0);

    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
}

static size_t printable_span(const unsigned char *buf, size_t len, unsigned char hi)
{
    size_t i = 0;
    while (i < len && buf[i] >= 0x20 && buf[i] < hi)
        i++;
    return i;
}

/* The ISA tests run this at each level. The answer must not depend on it. */
static void test_printable_span(void)
{
    enum { MAX = 160 };
    static const unsigned char edges[] = { 0x00, 0x1f, 0x20, 0x21, 0x7e, 0x7f, 0x80, 0xfe, 0xff };
    unsigned char buf[MAX + 64];
    uint32_t seed = 1;
    for (unsigned hi = 0; hi < 256; hi++) {
        for (size_t len = 0; len <= MAX; len += len < 70 ? 1 : 13) {
            for (int trial = 0; trial < 3; trial++) {
                size_t offset = (size_t)trial * 7;
                for (size_t i = 0; i < len; i++) {
                    seed = seed * 1103515245u + 12345u;
                    buf[offset + i] = (seed >> 16) % 8 ? (unsigned char)(0x20 + (seed >> 8) % 0x5f)
                                                       : edges[(seed >> 20) % sizeof(edges)];
                }
                const unsigned char *p = buf + offset;
                size_t want = printable_span(p, len, (unsigned char)hi);
                size_t got = wsap_vtcore_scan_printable_span((const char *)p, len, (unsigned char)hi);
                if (got != want)
                    fprintf(stderr, "%s: hi %#x len %zu: got %zu want %zu\n",
                            wsap_vtcore_isa(), hi, len, got, want);
                assert(got == want);
            }
        }
    }
}

int main(void)
{
    test_printable_span();
    test_palette_bounds();
    test_missing_palette();
    test_malformed_cluster_handles();
    return 0;
}
