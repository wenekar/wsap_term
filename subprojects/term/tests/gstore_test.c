/*
 * Tests for the per-terminal grapheme store (gstore).
 *
 * Verifies:
 * - Two terminals do not share clusters: freeing one leaves the other intact.
 * - Clusters that remain visible on screen survive GC sweeps.
 * - More than 65,536 distinct clusters can be interned as old ones scroll
 *   out of history, without reaching any cap or exhausting memory.
 * - Direct unit tests of intern, expand, deduplication, and sweep.
 */
#include "gstore.h"
#include "internal.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_direct_unit(void)
{
    struct gstore *g = wsap_vtcore_gstore_new(NULL);
    assert(g != NULL);
    assert(wsap_vtcore_gstore_count(g) == 0);

    /* Single code point returns the code point itself. */
    uint32_t single = 'A';
    assert(wsap_vtcore_gstore_intern(g, &single, 1) == 'A');
    assert(wsap_vtcore_gstore_count(g) == 0);

    /* Cluster returns a handle. */
    uint32_t c1[2] = { 'e', 0x0301 };
    uint32_t h1 = wsap_vtcore_gstore_intern(g, c1, 2);
    assert(wsap_vtcore_grapheme_is_handle(h1));
    assert(wsap_vtcore_gstore_count(g) == 1);

    /* Identical cluster deduplicates to the same handle. */
    uint32_t h1_dup = wsap_vtcore_gstore_intern(g, c1, 2);
    assert(h1_dup == h1);
    assert(wsap_vtcore_gstore_count(g) == 1);

    /* Different cluster gets a distinct handle. */
    uint32_t c2[2] = { 'a', 0x0300 };
    uint32_t h2 = wsap_vtcore_gstore_intern(g, c2, 2);
    assert(wsap_vtcore_grapheme_is_handle(h2));
    assert(h2 != h1);
    assert(wsap_vtcore_gstore_count(g) == 2);

    /* Expand both handles. */
    uint32_t out[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
    int n = wsap_vtcore_gstore_expand(g, h1, out, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(out[0] == 'e' && out[1] == 0x0301);

    n = wsap_vtcore_gstore_expand(g, h2, out, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(out[0] == 'a' && out[1] == 0x0300);

    /* Sweep with h1 marked and h2 unmarked. */
    wsap_vtcore_gstore_mark(g, h1);
    wsap_vtcore_gstore_sweep(g, NULL);
    assert(wsap_vtcore_gstore_count(g) == 1);

    /* h1 is still valid. */
    n = wsap_vtcore_gstore_expand(g, h1, out, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(out[0] == 'e' && out[1] == 0x0301);

    /* h2 was reclaimed. */
    n = wsap_vtcore_gstore_expand(g, h2, out, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 1 && out[0] == 0xFFFD);

    /* New cluster can reuse the reclaimed slot. */
    uint32_t c3[3] = { 'o', 0x0308, 0x0301 };
    uint32_t h3 = wsap_vtcore_gstore_intern(g, c3, 3);
    assert(wsap_vtcore_grapheme_is_handle(h3));
    assert(wsap_vtcore_gstore_count(g) == 2);
    n = wsap_vtcore_gstore_expand(g, h3, out, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 3);
    assert(out[0] == 'o' && out[1] == 0x0308 && out[2] == 0x0301);

    wsap_vtcore_gstore_free(g);
}

static void test_two_terminals_isolation(void)
{
    struct wsap_vtcore_options opt = { .size = { .x = 20, .y = 4 } };
    struct wsap_vtcore *t1 = wsap_vtcore_new(&opt);
    struct wsap_vtcore *t2 = wsap_vtcore_new(&opt);
    assert(t1 && t2);

    const char *cluster_text = "e\xcc\x81"; /* 'e' with acute */
    wsap_vtcore_feed(t1, cluster_text, strlen(cluster_text), 0);
    wsap_vtcore_feed(t2, cluster_text, strlen(cluster_text), 0);

    /* Free terminal 1. */
    wsap_vtcore_free(t1);

    /* Terminal 2 must still expand its cluster correctly. */
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap);
    assert(wsap_vtcore_snapshot_update(snap, t2));

    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, 0);
    assert(row[0].text & WSAP_VTCORE_TEXT_CLUSTER);

    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
    int n = wsap_vtcore_snapshot_cluster(snap, row[0].text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(cps[0] == 'e' && cps[1] == 0x0301);

    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t2);
}

static void test_screen_clusters_survive_sweep(void)
{
    struct wsap_vtcore_options opt = { .size = { .x = 40, .y = 10 }, .scrollback = 10 };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);

    /* Write 5 clusters on the top row. */
    wsap_vtcore_feed(t, "a\xcc\x80" "b\xcc\x81" "c\xcc\x82" "d\xcc\x83" "e\xcc\x84", 15, 0);

    /* Write ephemeral clusters on row 1, then overwrite them. */
    for (int i = 0; i < 50; i++) {
        wsap_vtcore_feed(t, "\033[2;1H\033[2K", 8, 0);
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "z\xcc\x80\xcc\x81%d", i);
        wsap_vtcore_feed(t, buf, (size_t)len, 0);
    }
    wsap_vtcore_feed(t, "\033[2;1H\033[2K", 8, 0);

    /* Explicit sweep. The clusters on row 0 are on screen and must survive. */
    wsap_vtcore_gstore_sweep(t->gstore, t);

    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap);
    assert(wsap_vtcore_snapshot_update(snap, t));

    /* Row 0 of screen has "a\xcc\x80...". */
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, 0);
    assert(row[0].text & WSAP_VTCORE_TEXT_CLUSTER);

    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
    int n = wsap_vtcore_snapshot_cluster(snap, row[0].text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(cps[0] == 'a' && cps[1] == 0x0300);

    n = wsap_vtcore_snapshot_cluster(snap, row[1].text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(cps[0] == 'b' && cps[1] == 0x0301);

    n = wsap_vtcore_snapshot_cluster(snap, row[2].text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 2);
    assert(cps[0] == 'c' && cps[1] == 0x0302);

    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
}

static void test_more_than_65536_clusters(void)
{
    /* Bounded history so old lines scroll out. */
    struct wsap_vtcore_options opt = { .size = { .x = 80, .y = 24 }, .scrollback = 10 };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);

    /*
     * Generate 66,000 distinct clusters.
     * Base 'a' (0x61) + mark1 (0x0300..0x034F, 80 options) + mark2 (0x0350..0x036F + 0xFE00..0xFE0F, 830 options)
     * 256 * 260 = 66,560 distinct clusters.
     */
    char buf[64];
    uint32_t total = 0;
    const uint32_t target = 66000;

    for (uint32_t i = 0; total < target; i++) {
        char base = (char)('a' + (i % 26));
        uint32_t cp1 = 0x0300 + ((i / 26) % 55);
        uint32_t cp2 = 0x0340 + ((i / (26 * 55)) % 47);

        size_t len = 0;
        buf[len++] = base;
        len += (size_t)wsap_vtcore_utf8_encode(cp1, buf + len);
        len += (size_t)wsap_vtcore_utf8_encode(cp2, buf + len);
        buf[len++] = '\r';
        buf[len++] = '\n';

        wsap_vtcore_feed(t, buf, len, 0);
        total++;
    }

    assert(total >= 65536);

    /* Verify the latest cluster on screen reads back correctly. */
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap);
    assert(wsap_vtcore_snapshot_update(snap, t));

    struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t);
    int y = c.pos.y > 0 ? c.pos.y - 1 : 0;
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, y);
    assert(row[0].text & WSAP_VTCORE_TEXT_CLUSTER);

    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
    int n = wsap_vtcore_snapshot_cluster(snap, row[0].text, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    assert(n == 3);

    /* Live cluster count must be small (only bounded screen and history). */
    uint32_t count = wsap_vtcore_gstore_count(t->gstore);
    assert(count < 2000);

    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
}

int main(void)
{
    test_direct_unit();
    test_two_terminals_isolation();
    test_screen_clusters_survive_sweep();
    test_more_than_65536_clusters();
    puts("PASS: gstore tests");
    return 0;
}
