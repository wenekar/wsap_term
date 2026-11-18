#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>

int main(void)
{
    struct wsap_vtcore_options options = { .size = { 8, 24 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s && wsap_vtcore_snapshot_update(s, t));

    const char line[] = "e\xcc\x81\r\n";
    for (int i = 0; i < 10000; i++) {
        wsap_vtcore_feed(t, line, sizeof(line) - 1, 0);
        assert(wsap_vtcore_snapshot_update(s, t));
    }

    /* Observable behavior: history is bounded by scrollback limit. */
    assert(wsap_vtcore_history_rows(t) == 5);

    /* Observable behavior: snapshot view size matches screen dimensions. */
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(s);
    assert(view && view->size.x == 8 && view->size.y == 24);

    /* Observable behavior: each visible row holds the fed text.
     * The line is "e" followed by U+0301, and that is one grapheme. The newline
     * after the line scrolls, so the cursor row is the bottom row. Every row
     * above the cursor row starts with the grapheme. The cursor row is blank. */
    for (int y = 0; y < view->size.y; y++) {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(s, y);
        assert(row);
        uint32_t cps[2];
        int len = wsap_vtcore_snapshot_cluster(s, row[0].text, cps, 2);
        if (y + 1 < view->size.y) {
            assert(len == 2 && cps[0] == 'e' && cps[1] == 0x0301);
        } else {
            assert(len == 1 && cps[0] == ' ');
        }
    }

    /* Observable behavior: row keys are distinct for different logical rows. */
    uint64_t first_key = wsap_vtcore_snapshot_row_key(s, 0);
    uint64_t last_key = wsap_vtcore_snapshot_row_key(s, view->size.y - 1);
    assert(first_key != last_key);

    /* Observable behavior: snapshot remains valid after many updates.
     * The same scrollback produces consistent snapshots. */
    struct wsap_vtcore_snapshot *s2 = wsap_vtcore_snapshot_new();
    assert(s2 && wsap_vtcore_snapshot_update(s2, t));
    const struct wsap_vtcore_view *view2 = wsap_vtcore_snapshot_view(s2);
    assert(view2 && view2->size.x == view->size.x && view2->size.y == view->size.y);
    wsap_vtcore_snapshot_free(s2);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return 0;
}
