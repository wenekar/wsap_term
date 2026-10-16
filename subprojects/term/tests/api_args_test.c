#include "term_api.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>

/* A feed that returns false must not move the cursor or change a cell. */
static void expect_unchanged(struct wsap_vtcore *t, int x, int y, uint32_t cp)
{
    assert(tt_cursor_x(t) == x);
    assert(tt_cursor_y(t) == y);
    assert(tt_cell(t, 0, 0)->text == cp);
}

static void test_feed_unknown_flag(void)
{
    struct wsap_vtcore *t = tt_new(10, 3);
    assert(t);
    assert(wsap_vtcore_feed(t, "abc", 3, 0));
    expect_unchanged(t, 3, 0, 'a');

    assert(!wsap_vtcore_feed(t, "xy", 2, 1u << 1));
    expect_unchanged(t, 3, 0, 'a');
    assert(!wsap_vtcore_feed(t, "xy", 2, WSAP_VTCORE_FEED_ONLCR | (1u << 4)));
    expect_unchanged(t, 3, 0, 'a');
    assert(!wsap_vtcore_feed(t, "xy", 2, 1u << 31));
    expect_unchanged(t, 3, 0, 'a');
    /* Unknown bits are rejected even when there are no bytes. */
    assert(!wsap_vtcore_feed(t, "", 0, 1u << 1));

    assert(wsap_vtcore_feed(t, "\nz", 2, WSAP_VTCORE_FEED_ONLCR));
    assert(tt_cursor_x(t) == 1);
    assert(tt_cursor_y(t) == 1);
    wsap_vtcore_free(t);
}

static void test_feed_null_buffer(void)
{
    struct wsap_vtcore *t = tt_new(10, 3);
    assert(t);
    assert(wsap_vtcore_feed(t, "abc", 3, 0));

    assert(!wsap_vtcore_feed(t, NULL, 1, 0));
    expect_unchanged(t, 3, 0, 'a');
    assert(!wsap_vtcore_feed(t, NULL, 4096, WSAP_VTCORE_FEED_ONLCR));
    expect_unchanged(t, 3, 0, 'a');

    /* A zero length is a no-op, so a NULL buffer is fine. */
    assert(wsap_vtcore_feed(t, NULL, 0, 0));
    assert(wsap_vtcore_feed(t, "", 0, WSAP_VTCORE_FEED_ONLCR));
    expect_unchanged(t, 3, 0, 'a');

    assert(!wsap_vtcore_feed(NULL, "a", 1, 0));
    assert(!wsap_vtcore_feed(NULL, NULL, 0, 0));
    wsap_vtcore_free(t);
}

static void test_new_rejects_bad_options(void)
{
    struct wsap_vtcore_options opt;

    memset(&opt, 0, sizeof(opt));
    opt.size.x = -1;
    assert(!wsap_vtcore_new(&opt));

    memset(&opt, 0, sizeof(opt));
    opt.size.y = -1;
    assert(!wsap_vtcore_new(&opt));

    memset(&opt, 0, sizeof(opt));
    opt.size.x = WSAP_VTCORE_MAX_COLS + 1;
    assert(!wsap_vtcore_new(&opt));

    memset(&opt, 0, sizeof(opt));
    opt.scrollback = -1;
    assert(!wsap_vtcore_new(&opt));

    memset(&opt, 0, sizeof(opt));
    opt.scrollback = WSAP_VTCORE_SCROLLBACK_MAX + 1;
    assert(!wsap_vtcore_new(&opt));

    memset(&opt, 0, sizeof(opt));
    opt.scrollback = WSAP_VTCORE_SCROLLBACK_MAX;
    struct wsap_vtcore *max = wsap_vtcore_new(&opt);
    assert(max);
    wsap_vtcore_free(max);

    memset(&opt, 0, sizeof(opt));
    opt.cursor_style = (enum wsap_vtcore_cursor_style)3;
    assert(!wsap_vtcore_new(&opt));

    memset(&opt, 0, sizeof(opt));
    opt.cursor_style = (enum wsap_vtcore_cursor_style)-1;
    assert(!wsap_vtcore_new(&opt));
}

static void test_new_accepts_defaults(void)
{
    struct wsap_vtcore_options opt;

    memset(&opt, 0, sizeof(opt));
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    assert(t);
    assert(tt_cols(t) == 80);
    assert(tt_rows(t) == 24);
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BLOCK);
    wsap_vtcore_free(t);

    memset(&opt, 0, sizeof(opt));
    opt.size.x = WSAP_VTCORE_MAX_COLS;
    opt.size.y = 1;
    opt.cursor_style = WSAP_VTCORE_CURSOR_BAR;
    t = wsap_vtcore_new(&opt);
    assert(t);
    assert(tt_cols(t) == WSAP_VTCORE_MAX_COLS);
    assert(tt_rows(t) == 1);
    assert(wsap_vtcore_cursor(t).style == WSAP_VTCORE_CURSOR_BAR);
    wsap_vtcore_free(t);
}

static void test_resize_rejects_too_wide(void)
{
    struct wsap_vtcore *t = tt_new(10, 3);
    assert(t);
    assert(wsap_vtcore_feed(t, "abc", 3, 0));

    assert(!tt_resize(t, WSAP_VTCORE_MAX_COLS + 1, 3));
    assert(tt_cols(t) == 10);
    assert(tt_rows(t) == 3);
    expect_unchanged(t, 3, 0, 'a');

    assert(!tt_resize(t, 10, 0));
    assert(!tt_resize(t, 0, 3));
    assert(tt_cols(t) == 10);
    assert(tt_rows(t) == 3);

    assert(tt_resize(t, WSAP_VTCORE_MAX_COLS, 3));
    assert(tt_cols(t) == WSAP_VTCORE_MAX_COLS);
    assert(tt_rows(t) == 3);
    wsap_vtcore_free(t);
}

int main(void)
{
    test_feed_unknown_flag();
    test_feed_null_buffer();
    test_new_rejects_bad_options();
    test_new_accepts_defaults();
    test_resize_rejects_too_wide();
    return 0;
}
