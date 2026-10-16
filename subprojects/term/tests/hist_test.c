#include "hist.h"
#include "hist_view.h"
#include "wsap_vtcore/unicode/grapheme.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t lines(const struct hist *h)
{
    return hist_end_id(h) - hist_first_id(h);
}

static struct cell dc(uint32_t c)
{
    return (struct cell){ .c = c, .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG };
}

static void test_init_free(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    assert(lines(&h) == 0);
    assert(hist_first_id(&h) == 0);
    assert(!hist_is_open(&h));
    wsap_vtcore_hist_free(&h);
}

static void test_ascii_roundtrip(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));

    const char *msg = "Hello, world!";
    wsap_vtcore_hist_push_ascii(&h, msg, (int)strlen(msg), NULL, false);
    assert(lines(&h) == 1);
    assert(wsap_vtcore_hist_line_rows(&h, 0, 80) == 1);
    assert(wsap_vtcore_hist_line_rows(&h, 0, 5) == 3);

    struct cell out[80];
    int n = wsap_vtcore_hist_decode_segment(&h, 0, 0, 80, out, NULL);
    assert(n == (int)strlen(msg));
    for (int i = 0; i < n; i++) {
        assert(out[i].c == (uint32_t)msg[i]);
        assert(out[i].fg == WSAP_VTCORE_COLOR_FG);
        assert(out[i].bg == WSAP_VTCORE_COLOR_BG);
        assert(out[i].flags == 0);
    }

    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 5, out, NULL) == 5);
    assert(out[0].c == 'H' && out[4].c == 'o');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 1, 5, out, NULL) == 5);
    assert(out[0].c == ',' && out[4].c == 'r');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 2, 5, out, NULL) == 3);
    assert(out[0].c == 'l' && out[2].c == '!');

    wsap_vtcore_hist_free(&h);
}

static void test_attributes_roundtrip(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));

    struct cell row[10];
    for (int i = 0; i < 10; i++) {
        row[i] = (struct cell){
            .c = 'A' + i,
            .fg = wsap_vtcore_color_rgb(i * 10, i * 20, i * 25),
            .bg = wsap_vtcore_color_rgb(255 - i * 10, 0, 0),
            .flags = (i % 2 == 0) ? CELL_BOLD : CELL_ITALIC,
            .pad = (uint16_t)(100 + i),
        };
    }
    wsap_vtcore_hist_push_row(&h, row, 10, false, NULL);
    assert(lines(&h) == 1);

    struct cell out[10];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 10, out, NULL) == 10);
    for (int i = 0; i < 10; i++) {
        assert(out[i].c == row[i].c);
        assert(out[i].fg == row[i].fg);
        assert(out[i].bg == row[i].bg);
        assert(out[i].flags == row[i].flags);
        assert(out[i].pad == row[i].pad);
    }
    wsap_vtcore_hist_free(&h);
}

static void test_wide_and_wrap(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));

    struct cell row[4] = { dc('A'), dc(0x4e16), dc(' '), dc('B') };
    row[1].flags = CELL_WIDE;
    row[2].flags = CELL_TRAIL;
    wsap_vtcore_hist_push_row(&h, row, 4, false, NULL);

    assert(wsap_vtcore_hist_line_rows(&h, 0, 4) == 1);
    /* Width 2: 'A' and a pad, then the wide cell, then 'B'. */
    assert(wsap_vtcore_hist_line_rows(&h, 0, 2) == 3);

    struct cell out[4];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 2, out, NULL) == 2);
    assert(out[0].c == 'A');
    assert(out[1].flags & CELL_WRAP_PAD);
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 1, 2, out, NULL) == 2);
    assert(out[0].c == 0x4e16 && (out[0].flags & CELL_WIDE));
    assert(out[1].flags & CELL_TRAIL);
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 2, 2, out, NULL) == 1);
    assert(out[0].c == 'B');
    wsap_vtcore_hist_free(&h);
}

static void test_clusters_roundtrip(void)
{
    struct gstore *g = wsap_vtcore_gstore_new(NULL);
    assert(g);
    uint32_t cps[3] = { 'e', 0x0301, 0x0300 };
    uint32_t handle = wsap_vtcore_gstore_intern(g, cps, 3);

    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    struct cell row[2] = { dc(handle), dc('X') };
    wsap_vtcore_hist_push_row(&h, row, 2, false, g);

    struct cell out[2];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 2, out, g) == 2);
    assert(wsap_vtcore_grapheme_is_handle(out[0].c));
    uint32_t got[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    assert(wsap_vtcore_gstore_expand(g, out[0].c, got, WSAP_VTCORE_GRAPHEME_MAX_LEN) == 3);
    assert(got[0] == cps[0] && got[1] == cps[1] && got[2] == cps[2]);
    assert(out[1].c == 'X');

    wsap_vtcore_hist_free(&h);
    wsap_vtcore_gstore_free(g);
}

static void test_open_lines_and_pop(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));

    struct cell part1[3] = { dc('1'), dc('2'), dc('3') };
    wsap_vtcore_hist_push_row(&h, part1, 3, true, NULL);
    assert(lines(&h) == 1 && hist_is_open(&h));
    struct cell part2[2] = { dc('4'), dc('5') };
    wsap_vtcore_hist_push_row(&h, part2, 2, false, NULL);
    assert(lines(&h) == 1 && !hist_is_open(&h));

    struct cell out[5];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 10, out, NULL) == 5);
    for (int i = 0; i < 5; i++)
        assert(out[i].c == (uint32_t)('1' + i));

    struct cell popped[2];
    assert(wsap_vtcore_hist_pop_tail_cells(&h, 2, popped, NULL) == 2);
    assert(popped[0].c == '4' && popped[1].c == '5');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 10, out, NULL) == 3);
    assert(out[0].c == '1' && out[2].c == '3');

    /* Pop from an open line with a pen. */
    wsap_vtcore_hist_close_open_line(&h);
    struct cell color[2] = {
        { .c = 'A', .fg = 0xff0000, .bg = 0x00ff00, .flags = CELL_BOLD },
        { .c = 'B', .fg = 0xff0000, .bg = 0x00ff00, .flags = CELL_BOLD },
    };
    wsap_vtcore_hist_push_row(&h, color, 2, true, NULL);
    assert(lines(&h) == 2);
    struct cell pop_color[1];
    assert(wsap_vtcore_hist_pop_tail_cells(&h, 1, pop_color, NULL) == 1);
    assert(pop_color[0].c == 'B' && pop_color[0].fg == 0xff0000 && pop_color[0].bg == 0x00ff00);
    assert(pop_color[0].flags & CELL_BOLD);
    assert(wsap_vtcore_hist_decode_segment(&h, 1, 0, 10, out, NULL) == 1);
    assert(out[0].c == 'A' && out[0].fg == 0xff0000 && (out[0].flags & CELL_BOLD));

    /* Popping a whole line returns its cells and removes it. */
    struct cell all[4];
    assert(wsap_vtcore_hist_pop_line(&h, all, 4, NULL) == 1);
    assert(all[0].c == 'A');
    assert(lines(&h) == 1 && !hist_is_open(&h));
    wsap_vtcore_hist_free(&h);
}

static void test_open_line_joins(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));

    struct cell line1[2] = { dc('X'), dc('Y') };
    wsap_vtcore_hist_push_row(&h, line1, 2, true, NULL);
    wsap_vtcore_hist_close_open_line(&h);
    struct cell line2[2] = { dc('Z'), dc('W') };
    wsap_vtcore_hist_push_row(&h, line2, 2, false, NULL);
    assert(lines(&h) == 2);

    struct cell out[16];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 10, out, NULL) == 2);
    assert(out[0].c == 'X' && out[1].c == 'Y');
    assert(wsap_vtcore_hist_decode_segment(&h, 1, 0, 10, out, NULL) == 2);
    assert(out[0].c == 'Z' && out[1].c == 'W');

    /* ASCII joins a wide open line. */
    struct cell wide[2] = { { .c = 0x4e2d, .fg = 0x123456, .bg = WSAP_VTCORE_COLOR_BG, .flags = CELL_WIDE },
                            { .c = 0, .fg = 0x123456, .bg = WSAP_VTCORE_COLOR_BG, .flags = CELL_TRAIL } };
    wsap_vtcore_hist_push_row(&h, wide, 2, true, NULL);
    wsap_vtcore_hist_push_ascii(&h, "hello", 5, NULL, false);
    assert(lines(&h) == 3);
    assert(wsap_vtcore_hist_decode_segment(&h, 2, 0, 10, out, NULL) == 7);
    assert(out[0].c == 0x4e2d && out[0].fg == 0x123456);
    assert(out[2].c == 'h' && out[2].fg == WSAP_VTCORE_COLOR_FG);
    wsap_vtcore_hist_free(&h);
}

static void test_eviction(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 5, 64 * 1024 * 2));
    for (int i = 0; i < 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "line%d", i);
        wsap_vtcore_hist_push_ascii(&h, buf, (int)strlen(buf), NULL, false);
    }
    assert(lines(&h) == 5);
    assert(hist_first_id(&h) == 5 && hist_end_id(&h) == 10);

    struct cell out[16];
    assert(wsap_vtcore_hist_decode_segment(&h, 5, 0, 16, out, NULL) == 5);
    assert(out[0].c == 'l' && out[4].c == '5');
    assert(wsap_vtcore_hist_decode_segment(&h, 9, 0, 16, out, NULL) == 5);
    assert(out[4].c == '9');
    assert(wsap_vtcore_hist_decode_segment(&h, 4, 0, 16, out, NULL) == 0);
    wsap_vtcore_hist_free(&h);
}

static void test_emoji_and_text_presentation(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    struct cell row[3] = { dc(0x2764), dc(' '), dc(0x2600) };
    row[0].flags = CELL_WIDE | CELL_EMOJI;
    row[1].flags = CELL_TRAIL;
    row[2].flags = CELL_TEXT;
    wsap_vtcore_hist_push_row(&h, row, 3, false, NULL);

    struct cell out[3];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 10, out, NULL) == 3);
    assert(out[0].c == 0x2764 && (out[0].flags & CELL_WIDE) && (out[0].flags & CELL_EMOJI));
    assert(out[1].flags & CELL_TRAIL);
    assert(out[2].c == 0x2600 && (out[2].flags & CELL_TEXT));
    wsap_vtcore_hist_free(&h);
}

static void test_width_one(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    wsap_vtcore_hist_push_ascii(&h, "ABC", 3, NULL, false);
    assert(wsap_vtcore_hist_line_rows(&h, 0, 1) == 3);

    struct cell out[2];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 1, out, NULL) == 1 && out[0].c == 'A');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 1, 1, out, NULL) == 1 && out[0].c == 'B');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 2, 1, out, NULL) == 1 && out[0].c == 'C');

    struct cell wide[2] = { dc(0x4e16), dc(' ') };
    wide[0].flags = CELL_WIDE;
    wide[1].flags = CELL_TRAIL;
    wsap_vtcore_hist_push_row(&h, wide, 2, false, NULL);
    assert(wsap_vtcore_hist_line_rows(&h, 1, 1) == 1);
    assert(wsap_vtcore_hist_decode_segment(&h, 1, 0, 1, out, NULL) == 1);
    assert(out[0].c == 0x4e16 && (out[0].flags & CELL_WIDE));
    wsap_vtcore_hist_free(&h);
}

static void test_blank_lines(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 10, 64 * 1024 * 4));
    const char *first = "First line on page 0";
    wsap_vtcore_hist_push_ascii(&h, first, (int)strlen(first), NULL, false);
    wsap_vtcore_hist_push_blank(&h, 10);
    assert(lines(&h) == 10);
    assert(hist_first_id(&h) == 1);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "line %d", i);
        wsap_vtcore_hist_push_ascii(&h, buf, (int)strlen(buf), NULL, false);
    }
    struct cell out[32];
    assert(wsap_vtcore_hist_decode_segment(&h, hist_end_id(&h) - 1, 0, 32, out, NULL) == 7);
    assert(out[5].c == '1' && out[6].c == '9');

    /* A blank row closes an open line and adds no line. */
    wsap_vtcore_hist_push_ascii(&h, "open", 4, NULL, true);
    uint64_t before = hist_end_id(&h);
    wsap_vtcore_hist_push_blank(&h, 1);
    assert(!hist_is_open(&h) && hist_end_id(&h) == before);

    /* More blank rows than the limit keep the id count going. */
    wsap_vtcore_hist_push_blank(&h, 1000);
    assert(lines(&h) == 10 && hist_end_id(&h) == before + 1000);
    wsap_vtcore_hist_push_blank(&h, 5);
    assert(lines(&h) == 10 && hist_end_id(&h) == before + 1005);
    assert(wsap_vtcore_hist_decode_segment(&h, hist_end_id(&h) - 1, 0, 32, out, NULL) == 0);
    assert(wsap_vtcore_hist_line_rows(&h, hist_end_id(&h) - 1, 32) == 1);
    wsap_vtcore_hist_free(&h);
}

static void test_open_line_attrs_continuation(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    struct cell r1[5], r2[10];
    for (int i = 0; i < 5; i++)
        r1[i] = (struct cell){ .c = '1' + i, .fg = wsap_vtcore_color_rgb(255, 0, 0), .bg = WSAP_VTCORE_COLOR_BG };
    for (int i = 0; i < 10; i++)
        r2[i] = (struct cell){ .c = 'a' + i, .fg = i < 5 ? wsap_vtcore_color_rgb(255, 0, 0) : wsap_vtcore_color_rgb(0, 0, 255),
                               .bg = WSAP_VTCORE_COLOR_BG };
    wsap_vtcore_hist_push_row(&h, r1, 5, true, NULL);
    wsap_vtcore_hist_push_row(&h, r2, 10, false, NULL);
    assert(lines(&h) == 1);

    struct cell out[15];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 15, out, NULL) == 15);
    for (int i = 0; i < 10; i++)
        assert(out[i].fg == wsap_vtcore_color_rgb(255, 0, 0));
    for (int i = 10; i < 15; i++)
        assert(out[i].fg == wsap_vtcore_color_rgb(0, 0, 255));
    wsap_vtcore_hist_free(&h);
}

static void mark_link(void *ud, uint16_t link)
{
    ((int *)ud)[link]++;
}

static void test_hyperlinks(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    struct pen_attr pen = pen_attr_default();
    pen.link = 42;
    wsap_vtcore_hist_push_ascii(&h, "Link text", 9, &pen, false);
    struct cell row[2] = { dc('a'), dc('b') };
    row[1].pad = 7;
    row[1].flags = CELL_LINK;
    wsap_vtcore_hist_push_row(&h, row, 2, true, NULL);

    struct cell out[16];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 16, out, NULL) == 9);
    for (int i = 0; i < 9; i++)
        assert(out[i].pad == 42);

    int marks[64] = { 0 };
    wsap_vtcore_hist_mark_links(&h, mark_link, marks);
    assert(marks[42] && marks[7]);
    wsap_vtcore_hist_free(&h);
}

static void test_long_lines(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));

    /* A line past the part limit keeps one id. */
    char chunk[1024];
    memset(chunk, 'X', sizeof(chunk));
    for (int i = 0; i < 64; i++)
        wsap_vtcore_hist_push_ascii(&h, chunk, sizeof(chunk), NULL, true);
    wsap_vtcore_hist_push_ascii(&h, chunk, 100, NULL, false);
    assert(lines(&h) == 1);
    assert(wsap_vtcore_hist_line_cells(&h, 0) == 65636);
    assert(wsap_vtcore_hist_line_rows(&h, 0, 1000) == 66);

    struct cell out[1000];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 64, 1000, out, NULL) == 1000);
    for (int i = 0; i < 1000; i++)
        assert(out[i].c == 'X');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 65, 1000, out, NULL) == 636);

    memset(chunk, 'Y', sizeof(chunk));
    for (int i = 0; i < 64; i++)
        wsap_vtcore_hist_push_ascii(&h, chunk, sizeof(chunk), NULL, true);
    wsap_vtcore_hist_push_ascii(&h, chunk, 100, NULL, false);
    wsap_vtcore_hist_push_ascii(&h, "tail", 4, NULL, false);
    assert(lines(&h) == 3);
    assert(wsap_vtcore_hist_line_rows(&h, 1, 1000) == 66);
    assert(wsap_vtcore_hist_decode_segment(&h, 1, 0, 1000, out, NULL) == 1000 && out[0].c == 'Y');
    assert(wsap_vtcore_hist_decode_segment(&h, 2, 0, 1000, out, NULL) == 4 && out[0].c == 't');
    assert(wsap_vtcore_hist_rows(&h, 1000) == 133);

    /* One long ASCII push splits into parts but reads back whole. */
    wsap_vtcore_hist_free(&h);
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 8));
    int big_len = 150000;
    char *big = malloc((size_t)big_len);
    assert(big);
    for (int i = 0; i < big_len; i++)
        big[i] = (char)('A' + (i % 26));
    wsap_vtcore_hist_push_ascii(&h, big, big_len, NULL, false);
    assert(lines(&h) == 1);
    assert(wsap_vtcore_hist_line_rows(&h, 0, 1000) == 150);
    for (int s = 0; s < 150; s++) {
        assert(wsap_vtcore_hist_decode_segment(&h, 0, s, 1000, out, NULL) == 1000);
        for (int x = 0; x < 1000; x++)
            assert(out[x].c == (uint32_t)big[s * 1000 + x]);
    }
    free(big);
    wsap_vtcore_hist_free(&h);
}

static void test_page_limits(void)
{
    struct hist h;
    struct cell out[1000];

    /* One page: old lines leave to make room. */
    assert(wsap_vtcore_hist_init(&h, 10000, HIST_PAGE_SIZE));
    char line64[64];
    memset(line64, 'M', sizeof(line64));
    for (int i = 0; i < 1200; i++)
        wsap_vtcore_hist_push_ascii(&h, line64, sizeof(line64), NULL, false);
    assert(lines(&h) > 0 && lines(&h) < 1200);
    assert(wsap_vtcore_hist_decode_segment(&h, hist_end_id(&h) - 1, 0, 64, out, NULL) == 64);
    assert(out[0].c == 'M');
    wsap_vtcore_hist_free(&h);

    /* A line limit of one keeps the long line as one line. */
    assert(wsap_vtcore_hist_init(&h, 1, 64 * 1024 * 4));
    char chunk[1024];
    memset(chunk, 'Z', sizeof(chunk));
    for (int i = 0; i < 65; i++)
        wsap_vtcore_hist_push_ascii(&h, chunk, sizeof(chunk), NULL, true);
    assert(lines(&h) == 1 && hist_is_open(&h));
    wsap_vtcore_hist_push_ascii(&h, "new", 3, NULL, false);
    wsap_vtcore_hist_push_ascii(&h, "next", 4, NULL, false);
    assert(lines(&h) == 1);
    assert(wsap_vtcore_hist_decode_segment(&h, hist_first_id(&h), 0, 10, out, NULL) == 4);
    wsap_vtcore_hist_free(&h);

    /* A line longer than the arena drops its head but keeps its id. */
    assert(wsap_vtcore_hist_init(&h, 10, HIST_PAGE_SIZE));
    wsap_vtcore_hist_push_ascii(&h, "PREFIX", 6, NULL, true);
    for (int i = 0; i < 100; i++)
        wsap_vtcore_hist_push_ascii(&h, chunk, sizeof(chunk), NULL, true);
    wsap_vtcore_hist_close_open_line(&h);
    assert(lines(&h) == 1 && hist_first_id(&h) == 0);
    assert(wsap_vtcore_hist_line_cells(&h, 0) > 0 && wsap_vtcore_hist_line_cells(&h, 0) < 100 * 1024);
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 6, out, NULL) == 6 && out[0].c == 'Z');
    wsap_vtcore_hist_free(&h);

    /* A small open line moves to a new page instead of splitting. */
    assert(wsap_vtcore_hist_init(&h, 10, HIST_PAGE_SIZE));
    for (int i = 0; i < 60; i++)
        wsap_vtcore_hist_push_ascii(&h, chunk, sizeof(chunk), NULL, false);
    wsap_vtcore_hist_push_ascii(&h, "PREFIX", 6, NULL, true);
    for (int i = 0; i < 10; i++)
        wsap_vtcore_hist_push_ascii(&h, chunk, sizeof(chunk), NULL, true);
    wsap_vtcore_hist_close_open_line(&h);
    uint64_t id = hist_end_id(&h) - 1;
    assert(wsap_vtcore_hist_decode_segment(&h, id, 0, 6, out, NULL) == 6);
    assert(out[0].c == 'P' && out[5].c == 'X');
    assert(wsap_vtcore_hist_line_rows(&h, id, 1000) == 11);
    wsap_vtcore_hist_free(&h);
}

static void test_nul_and_wide_pop(void)
{
    struct hist h;
    struct cell out[8];
    assert(wsap_vtcore_hist_init(&h, 10, 64 * 1024));
    struct cell nul_row[4] = { dc('A'), dc(0), dc('B'), dc('C') };
    wsap_vtcore_hist_push_row(&h, nul_row, 4, false, NULL);
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 4, out, NULL) == 4);
    assert(out[0].c == 'A' && out[1].c == ' ' && out[2].c == 'B' && out[3].c == 'C');
    wsap_vtcore_hist_free(&h);

    /* A pop does not split a wide cell. */
    assert(wsap_vtcore_hist_init(&h, 10, 64 * 1024 * 4));
    struct cell wide[4] = { dc('A'), dc(0x4E2D), dc(0), dc('B') };
    wide[1].flags = CELL_WIDE;
    wide[2].flags = CELL_TRAIL;
    wsap_vtcore_hist_push_row(&h, wide, 4, true, NULL);
    struct cell popped[2];
    assert(wsap_vtcore_hist_pop_tail_cells(&h, 2, popped, NULL) == 1);
    assert(popped[0].c == 'B');
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 4, out, NULL) == 3);
    assert(out[0].c == 'A' && out[1].c == 0x4E2D && (out[2].flags & CELL_TRAIL));
    wsap_vtcore_hist_free(&h);
}

static void test_default_prefix_attributes(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 100, 64 * 1024 * 4));
    struct cell row[8];
    for (int i = 0; i < 8; i++) {
        row[i] = dc('a' + i);
        if (i >= 4) {
            row[i].fg = wsap_vtcore_color_rgb(10, 20, 30);
            row[i].bg = wsap_vtcore_color_rgb(40, 50, 60);
            row[i].flags = CELL_BOLD;
        }
    }
    wsap_vtcore_hist_push_row(&h, row, 8, false, NULL);
    struct cell out[8];
    assert(wsap_vtcore_hist_decode_segment(&h, 0, 0, 8, out, NULL) == 8);
    for (int i = 0; i < 8; i++) {
        assert(out[i].c == (uint32_t)('a' + i));
        assert(out[i].fg == row[i].fg && out[i].bg == row[i].bg && out[i].flags == row[i].flags);
    }

    struct pen_attr colored = pen_attr_default();
    colored.fg = wsap_vtcore_color_rgb(200, 100, 50);
    wsap_vtcore_hist_push_ascii(&h, "0123", 4, NULL, true);
    wsap_vtcore_hist_push_ascii(&h, "4567", 4, &colored, false);
    assert(wsap_vtcore_hist_decode_segment(&h, 1, 0, 8, out, NULL) == 8);
    for (int i = 0; i < 8; i++)
        assert(out[i].fg == (i < 4 ? WSAP_VTCORE_COLOR_FG : wsap_vtcore_color_rgb(200, 100, 50)));
    wsap_vtcore_hist_free(&h);
}

static void test_utf8_and_disabled(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 2, 64 * 1024 * 2));
    wsap_vtcore_hist_push_ascii(&h, "first", 5, NULL, false);
    struct cell p1[2] = { dc('A'), dc('B') };
    struct cell p2[2] = { dc(0x00e9), dc('C') };
    wsap_vtcore_hist_push_row(&h, p1, 2, true, NULL);
    wsap_vtcore_hist_push_row(&h, p2, 2, false, NULL);
    assert(lines(&h) == 2 && hist_first_id(&h) == 0);
    struct cell out[8];
    assert(wsap_vtcore_hist_decode_segment(&h, 1, 0, 8, out, NULL) == 4);
    assert(out[0].c == 'A' && out[1].c == 'B' && out[2].c == 0x00e9 && out[3].c == 'C');
    wsap_vtcore_hist_free(&h);

    assert(wsap_vtcore_hist_init(&h, 0, 0));
    wsap_vtcore_hist_push_ascii(&h, "ignored", 7, NULL, false);
    wsap_vtcore_hist_push_row(&h, p1, 2, false, NULL);
    wsap_vtcore_hist_push_blank(&h, 3);
    assert(lines(&h) == 0);
    wsap_vtcore_hist_free(&h);
}

/* Random rows of every kind, pushed and compared with a plain copy. */
static void test_fuzz_rows(void)
{
    enum { LIMIT = 40, COLS = 24, ITER = 4000 };
    struct gstore *g = wsap_vtcore_gstore_new(NULL);
    uint32_t cps[2] = { 'a', 0x0301 };
    uint32_t cluster = wsap_vtcore_gstore_intern(g, cps, 2);
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, LIMIT, 64 * 1024 * 2));

    static struct cell ref[LIMIT + 1][COLS * 8];
    static int ref_len[LIMIT + 1];
    uint64_t ref_first = 0, ref_end = 0;
    bool open = false;
    unsigned state = 7;
    for (int it = 0; it < ITER; it++) {
        state = state * 1103515245u + 12345u;
        int len = (int)((state >> 16) % (COLS + 1));
        bool wrapped = ((state >> 8) & 3) == 0;
        struct cell row[COLS];
        int n = 0;
        while (n < len) {
            state = state * 1103515245u + 12345u;
            unsigned k = (state >> 16) % 10;
            struct cell c = dc('a' + (state >> 20) % 26);
            if (k == 1)
                c.fg = wsap_vtcore_color_index((int)(state >> 24) % 16);
            if (k == 2)
                c.flags = CELL_BOLD;
            if (k == 3)
                c.c = 0xe9;
            if (k == 4)
                c.c = cluster;
            if (k == 5 && n + 1 < len) {
                c.c = 0x4e00 + (state >> 20) % 100;
                c.flags = CELL_WIDE;
                row[n++] = c;
                c.c = 0;
                c.flags = CELL_TRAIL;
            }
            row[n++] = c;
        }
        if (!open) {
            if (ref_end - ref_first == LIMIT)
                ref_first++;
            ref_len[ref_end % (LIMIT + 1)] = 0;
            ref_end++;
        }
        int slot = (int)((ref_end - 1) % (LIMIT + 1));
        if (!open && len == 0 && !wrapped) {
            /* A blank line. */
        } else {
            memcpy(&ref[slot][ref_len[slot]], row, (size_t)n * sizeof(row[0]));
            ref_len[slot] += n;
        }
        wsap_vtcore_hist_push_row(&h, row, n, wrapped, g);
        open = wrapped;
        assert(hist_first_id(&h) == ref_first && hist_end_id(&h) == ref_end);
    }

    struct cell out[COLS * 8];
    for (uint64_t id = ref_first; id < ref_end; id++) {
        int slot = (int)(id % (LIMIT + 1));
        int want = ref_len[slot];
        assert((int)wsap_vtcore_hist_line_cells(&h, id) == want);
        if (!want)
            continue;
        assert(wsap_vtcore_hist_decode_segment(&h, id, 0, COLS * 8, out, g) == want);
        for (int x = 0; x < want; x++) {
            const struct cell *a = &ref[slot][x], *b = &out[x];
            if (a->flags & CELL_TRAIL) {
                assert(b->flags & CELL_TRAIL);
                continue;
            }
            assert(a->fg == b->fg && a->flags == b->flags);
            if (wsap_vtcore_grapheme_is_handle(a->c)) {
                uint32_t got[WSAP_VTCORE_GRAPHEME_MAX_LEN];
                assert(wsap_vtcore_gstore_expand(g, b->c, got, WSAP_VTCORE_GRAPHEME_MAX_LEN) == 2 && got[1] == 0x0301);
            } else {
                assert(a->c == b->c);
            }
        }
    }
    wsap_vtcore_hist_free(&h);
    wsap_vtcore_gstore_free(g);
}

static void test_large_wide_count(void)
{
    struct hist h;
    struct hist_view view = {0};
    assert(wsap_vtcore_hist_init(&h, 600, 4 * HIST_PAGE_SIZE));
    struct cell row[4] = { dc('A'), dc(0x65e5), dc(0), dc('B') };
    row[1].flags = CELL_WIDE;
    row[2].flags = CELL_TRAIL;
    for (int i = 0; i < 400; i++)
        wsap_vtcore_hist_push_row(&h, row, 4, true, NULL);
    wsap_vtcore_hist_close_open_line(&h);
    uint64_t exact = wsap_vtcore_hist_rows(&h, 3);
    wsap_vtcore_hist_prepare_rows(&h, 3);
    assert(wsap_vtcore_hist_estimated_rows(&h, 3) < exact);
    assert(wsap_vtcore_hist_view_sync(&view, &h, 3));
    wsap_vtcore_hist_view_scroll_to(&view, &h, 3, (struct line_pos){0}, INT_MAX);
    wsap_vtcore_hist_view_refine(&view, &h, 3, 3);
    assert(exact > 254);
    assert(wsap_vtcore_hist_estimated_rows(&h, 3) == exact);
    for (int i = 0; i < 300; i++)
        wsap_vtcore_hist_push_ascii(&h, "x", 1, NULL, false);
    assert(wsap_vtcore_hist_estimated_rows(&h, 3) == exact + 300);
    assert(wsap_vtcore_hist_view_sync(&view, &h, 3));
    for (int i = 0; i < 300; i++)
        wsap_vtcore_hist_push_ascii(&h, "x", 1, NULL, false);
    assert(wsap_vtcore_hist_estimated_rows(&h, 3) == 600);
    wsap_vtcore_hist_view_free(&view);
    wsap_vtcore_hist_free(&h);
}

static void test_view_after_prefix_eviction(void)
{
    struct hist h;
    struct hist_view view = {0};
    assert(wsap_vtcore_hist_init(&h, 10, 2 * HIST_PAGE_SIZE));
    char bytes[1000];
    for (int i = 0; i < 100; i++) {
        memset(bytes, 'A' + i % 26, sizeof(bytes));
        wsap_vtcore_hist_push_ascii(&h, bytes, sizeof(bytes), NULL, true);
    }
    wsap_vtcore_hist_view_scroll_to(&view, &h, 1000, (struct line_pos){0}, 10);
    struct hist_view_pos pos;
    struct cell before[1000], after[1000];
    assert(wsap_vtcore_hist_view_row_pos(&view, &h, 1000, 3, (struct line_pos){0}, 0, &pos));
    assert(wsap_vtcore_hist_decode_segment(&h, pos.id, pos.seg, 1000, before, NULL) == 1000);
    for (int i = 100; i < 140; i++) {
        memset(bytes, 'A' + i % 26, sizeof(bytes));
        wsap_vtcore_hist_push_ascii(&h, bytes, sizeof(bytes), NULL, true);
        wsap_vtcore_hist_view_note_push(&view, &h, 1000);
    }
    assert(wsap_vtcore_hist_line_cells(&h, 0) < 140000);
    assert(wsap_vtcore_hist_view_row_pos(&view, &h, 1000, 3, (struct line_pos){0}, 0, &pos));
    assert(wsap_vtcore_hist_decode_segment(&h, pos.id, pos.seg, 1000, after, NULL) == 1000);
    for (int i = 0; i < 1000; i++)
        assert(before[i].c == after[i].c);
    wsap_vtcore_hist_view_rewrap(&view, &h, 500);
    assert(wsap_vtcore_hist_view_row_pos(&view, &h, 500, 3, (struct line_pos){0}, 0, &pos));
    assert(wsap_vtcore_hist_decode_segment(&h, pos.id, pos.seg, 500, after, NULL) == 500);
    for (int i = 0; i < 500; i++)
        assert(before[i].c == after[i].c);
    wsap_vtcore_hist_view_free(&view);
    wsap_vtcore_hist_free(&h);
}

static void test_short_lines_after_eviction(void)
{
    struct hist h;
    assert(wsap_vtcore_hist_init(&h, 64, 64 * 1024 * 4));
    wsap_vtcore_hist_prepare_rows(&h, 80);
    const char text[] = "abc\n\nx\npqrs\n\nz\n";
    const int offsets[] = {0, 4, 5, 7, 12, 13};
    const int lengths[] = {3, 0, 1, 4, 0, 1};
    struct hist_ascii_line batch[6];
    for (int i = 0; i < 6; i++)
        batch[i] = (struct hist_ascii_line){
            .ptr = text + offsets[i], .len = (uint16_t)lengths[i],
        };
    for (int pass = 0; pass < 20000; pass++)
        wsap_vtcore_hist_push_ascii_lines(&h, batch, 6, NULL);
    assert(lines(&h) == 64);
    assert(wsap_vtcore_hist_rows(&h, 80) == 64);
    for (uint64_t id = hist_first_id(&h); id < hist_end_id(&h); id++) {
        int k = (int)(id % 6);
        struct cell out[80];
        assert(wsap_vtcore_hist_decode_segment(&h, id, 0, 80, out, NULL) == lengths[k]);
        for (int x = 0; x < lengths[k]; x++) {
            assert(out[x].c == (uint32_t)text[offsets[k] + x]);
            assert(out[x].fg == WSAP_VTCORE_COLOR_FG);
            assert(out[x].bg == WSAP_VTCORE_COLOR_BG);
        }
    }
    wsap_vtcore_hist_free(&h);
}

int main(void)
{
    test_short_lines_after_eviction();
    test_large_wide_count();
    test_view_after_prefix_eviction();
    test_init_free();
    test_ascii_roundtrip();
    test_attributes_roundtrip();
    test_wide_and_wrap();
    test_clusters_roundtrip();
    test_open_lines_and_pop();
    test_open_line_joins();
    test_eviction();
    test_width_one();
    test_emoji_and_text_presentation();
    test_blank_lines();
    test_open_line_attrs_continuation();
    test_hyperlinks();
    test_long_lines();
    test_page_limits();
    test_nul_and_wide_pop();
    test_default_prefix_attributes();
    test_utf8_and_disabled();
    test_fuzz_rows();

    printf("PASS: hist tests\n");
    return 0;
}
