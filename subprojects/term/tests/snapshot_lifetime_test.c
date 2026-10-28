#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void expect_cluster(const struct wsap_vtcore_snapshot *snapshot, uint32_t text)
{
    uint32_t cps[4];
    int count = wsap_vtcore_snapshot_cluster(snapshot, text, cps, 4);
    assert(count == 2);
    assert(cps[0] == 'e' && cps[1] == 0x301);
}

static void test_snapshot_owns_captured_data(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 8, .y = 2 } };
    wsap_vtcore_theme_default(&options.theme);
    options.theme.fg = 0xff123456u;
    struct wsap_vtcore *term = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(term && snapshot);

    const char text[] = "e\xcc\x81\xe4\xb8\xadX";
    assert(wsap_vtcore_feed(term, text, sizeof(text) - 1, 0));
    assert(wsap_vtcore_select_start(term,
           (struct wsap_vtcore_vector2i){0}, false, WSAP_VTCORE_SELECT_CELL));
    assert(wsap_vtcore_select_extend(term,
           (struct wsap_vtcore_vector2i){ .x = 3 }, true));
    assert(wsap_vtcore_snapshot_update(snapshot, term));

    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(snapshot);
    const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snapshot, 0);
    struct wsap_vtcore_span selection = wsap_vtcore_snapshot_row_selection(snapshot, 0);
    uint64_t key = wsap_vtcore_snapshot_row_key(snapshot, 0);
    uint64_t version = view->version;
    assert(row && row[0].width == 1 && row[1].width == 2 && row[2].width == 0);
    assert(row[3].text == 'X');
    expect_cluster(snapshot, row[0].text);
    assert(view->palette[WSAP_VTCORE_PALETTE_FG] == 0xff123456u);
    assert(selection.end > selection.begin);

    options.theme.fg = 0xffabcdefu;
    wsap_vtcore_set_theme(term, &options.theme);
    assert(wsap_vtcore_resize(term, (struct wsap_vtcore_vector2i){ .x = 4, .y = 3 }));
    assert(wsap_vtcore_feed(term, "\033[2J\033[HZ", 8, 0));
    wsap_vtcore_select_clear(term);
    wsap_vtcore_free(term);

    assert(view == wsap_vtcore_snapshot_view(snapshot));
    assert(row == wsap_vtcore_snapshot_row(snapshot, 0));
    assert(view->size.x == 8 && view->size.y == 2 && view->version == version);
    assert(view->palette[WSAP_VTCORE_PALETTE_FG] == 0xff123456u);
    assert(row[1].text == 0x4e2d && row[3].text == 'X');
    assert(wsap_vtcore_snapshot_row_key(snapshot, 0) == key);
    struct wsap_vtcore_span retained = wsap_vtcore_snapshot_row_selection(snapshot, 0);
    assert(retained.begin == selection.begin && retained.end == selection.end);
    expect_cluster(snapshot, row[0].text);

    wsap_vtcore_snapshot_free(snapshot);
}

static void test_snapshot_can_change_source(void)
{
    struct wsap_vtcore_options options = { .size = { .x = 8, .y = 2 } };
    struct wsap_vtcore *first = wsap_vtcore_new(&options);
    struct wsap_vtcore *second = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    assert(first && second && snapshot);

    assert(wsap_vtcore_feed(first, "e\xcc\x81", 3, 0));
    assert(wsap_vtcore_feed(second, "B", 1, 0));
    assert(wsap_vtcore_snapshot_update(snapshot, first));
    expect_cluster(snapshot, wsap_vtcore_snapshot_row(snapshot, 0)[0].text);
    assert(wsap_vtcore_snapshot_update(snapshot, second));
    assert(wsap_vtcore_snapshot_row(snapshot, 0)[0].text == 'B');
    assert(wsap_vtcore_snapshot_view(snapshot)->scrolled == 0);
    assert(wsap_vtcore_snapshot_update(snapshot, first));
    expect_cluster(snapshot, wsap_vtcore_snapshot_row(snapshot, 0)[0].text);

    wsap_vtcore_free(first);
    wsap_vtcore_free(second);
    wsap_vtcore_snapshot_free(snapshot);
}

int main(void)
{
    test_snapshot_owns_captured_data();
    test_snapshot_can_change_source();
    return 0;
}
