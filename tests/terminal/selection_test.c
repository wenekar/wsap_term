#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal/selection.h"
#include "term_api.h"

static void feed(struct wsap_vtcore *t, const char *s) { tt_write(t, s, strlen(s)); }

static bool pointer_selects_text(struct wsap_vtcore *t)
{
    struct wsap_vtcore_modes modes = wsap_vtcore_modes(t);
    return modes.mouse == WSAP_VTCORE_MOUSE_OFF && !modes.alternate;
}
static void selected_text(struct wsap_vtcore *t, const char *expected)
{
    char *text = wsap_vtcore_selection_text(t);
    if ((expected && (!text || strcmp(text, expected))) || (!expected && text)) {
        fprintf(stderr, "selection_test: got '%s' expected '%s'\n", text ? text : "(null)", expected ? expected : "(null)");
    }
    assert(expected ? text && !strcmp(text, expected) : !text);
    wsap_vtcore_text_free(text);
}

static void selection_test(struct wsap_vtcore *t)
{
    feed(t, "hello world\r\nnext row");
    struct text_selection s = {0};
    tt_select(t, 0, 0, 4, 0);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1010);
    selected_text(t, NULL);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1100);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1110);
    selected_text(t, "world");
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1200);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1210);
    selected_text(t, "hello world");
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 4, .y = 0 }, false, PTR_PRESS, 2000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 0, .y = 0 }, false, PTR_MOTION, 2010);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 0, .y = 0 }, false, PTR_RELEASE, 2020);
    selected_text(t, "hell");

    selection_handle_pointer(&s, t, (struct vector2i){ .x = 1, .y = 0 }, false, PTR_PRESS, 3000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 1, .y = 0 }, true, PTR_RELEASE, 3010);
    selected_text(t, "e");
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 1, .y = 0 }, true, PTR_PRESS, 4000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 1, .y = 0 }, false, PTR_RELEASE, 4010);
    selected_text(t, "e");

}

static void selection_scroll_test(void)
{
    struct wsap_vtcore *t = tt_new(4, 3);
    feed(t, "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff");
    struct text_selection s = {0};
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 0, .y = 1 }, false, PTR_PRESS, 1000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 0, .y = 1 }, true, PTR_RELEASE, 1010);
    selected_text(t, "e");
    wsap_vtcore_scroll(t, 2);
    selected_text(t, "e");
    assert(!tt_selected(t, 0, 1));
    wsap_vtcore_scroll_to(t, 0);
    assert(tt_selected(t, 0, 1) && !tt_selected(t, 1, 1));

    selection_handle_pointer(&s, t, (struct vector2i){ .x = 0, .y = 1 }, true, PTR_PRESS, 2000);
    wsap_vtcore_scroll(t, 2);
    selection_extend_to_pointer(&s, t);
    selected_text(t, "ccc\ndddd\ne");
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 0, .y = 1 }, true, PTR_RELEASE, 2010);
    wsap_vtcore_scroll_to(t, 0);
    selected_text(t, "ccc\ndddd\ne");
    assert(tt_selected(t, 0, 1) && !tt_selected(t, 1, 1));
    wsap_vtcore_free(t);
}

static void selection_scrollback_test(void)
{
    struct wsap_vtcore *t = tt_new(8, 3);
    feed(t, "keepthis\r\nbbbbbbbb\r\ncccccccc");
    tt_select(t, 0, 0, 7, 0);
    selected_text(t, "keepthis");

    feed(t, "\r\ndddddddd\r\neeeeeeee\r\nffffffff\r\n");
    assert(tt_history(t) > 0);
    selected_text(t, "keepthis");
    assert(!tt_selected(t, 0, 0));
    wsap_vtcore_scroll(t, tt_history(t));
    assert(tt_selected(t, 0, 0) && tt_selected(t, 7, 0));
    assert(!tt_selected(t, 0, 1));

    wsap_vtcore_scroll_to(t, 0);
    tt_select(t, 0, 1, 7, 1);
    selected_text(t, "ffffffff");
    feed(t, "gggggggg\r\n");
    selected_text(t, "ffffffff");
    assert(tt_selected(t, 0, 0) && !tt_selected(t, 0, 1));

    tt_select(t, 0, 0, 7, 0);
    selected_text(t, "ffffffff");
    for (int i = 0; i < TEST_SCROLLBACK + 4; i++)
        feed(t, "hhhhhhhh\r\n");
    assert(!wsap_vtcore_has_selection(t));
    wsap_vtcore_free(t);
}

static void alternate_test(struct wsap_vtcore *t)
{
    feed(t, "\033[2J\033[Hhistory\n\n\n\n\033[Hshell\033[25Ghidden\033[2;3H\033[31m");
    int history = tt_history(t);
    assert(pointer_selects_text(t));
    feed(t, "\033[?1049h\033[?25l\033[?1000hAPP\033[?1049h");
    assert(!pointer_selects_text(t));
    assert(tt_cell(t, 0, 0)->text == 'A');
    feed(t, "\n\n\n\n\n\033[2J");
    assert(tt_history(t) == 0);
    wsap_vtcore_scroll(t, 10);
    assert(tt_view_offset(t) == 0);
    bool resized = tt_resize(t, 35, 5);
    assert(resized);
    feed(t, "\033[?1000l\033[?1049lZ");
    assert(pointer_selects_text(t) && wsap_vtcore_cursor(t).visible);

    assert(tt_cell(t, 0, 1)->text == 's' && tt_cell(t, 24, 1)->text == 'h');
    assert(tt_cell(t, 2, 2)->text == 'Z');
    assert(tt_cursor_x(t) == 3 && tt_cursor_y(t) == 2);
    assert(tt_history(t) == history - 1);
    feed(t, "\033[?1049h");
    assert(tt_cell(t, 0, 0)->text == ' ');
    feed(t, "\033[?1049l");
}

static void selection_wide_line_columns(void)
{
    enum { COLS = 40, ROWS = 10, GLYPHS = 400 };
    struct wsap_vtcore *t = tt_new(COLS, ROWS);
    for (int i = 0; i < GLYPHS; i++)
        feed(t, "\xE6\x97\xA5");
    feed(t, "\r\n");
    feed(t, "x\r\ny\r\n");
    int total = tt_history(t);
    assert(total > 0);
    wsap_vtcore_scroll_to(t, total - 3);
    wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 0 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = COLS - 1, .y = ROWS - 1 }, false);
    assert(wsap_vtcore_has_selection(t));

    char *text = wsap_vtcore_selection_text(t);
    assert(text);
    size_t len = strlen(text);
    assert(len % 3 == 0);
    for (size_t i = 0; i + 2 < len; i += 3)
        assert((unsigned char)text[i] == 0xE6 &&
               (unsigned char)text[i + 1] == 0x97 &&
               (unsigned char)text[i + 2] == 0xA5);
    wsap_vtcore_text_free(text);
    wsap_vtcore_free(t);
}

static void selection_cluster_test(void)
{
    struct wsap_vtcore *t = tt_new(80, 30);
    for (int row = 0; row < 30; row++)
        for (int col = 0; col < 80; col++)
            feed(t, "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
                    "\xE2\x80\x8D\xF0\x9F\x91\xA6\xE2\x80\x8D\xF0\x9F\x91\xA5");
    tt_select(t, 0, 0, 79, 29);
    char *text = wsap_vtcore_selection_text(t);
    assert(text);
    assert(strlen(text) == (size_t)(80u * 30u / 2u) * 25u);
    wsap_vtcore_text_free(text);
    wsap_vtcore_free(t);
}

static void selection_massive_line_test(void)
{
    enum { COLS = 80, ROWS = 24, GLYPHS = 10000 };
    struct wsap_vtcore *t = tt_new(COLS, ROWS);
    assert(t);

    for (int i = 0; i < GLYPHS; i++)
        feed(t, (i % 2 == 0) ? "\xE6\x97\xA5" : "a");
    feed(t, "\r\n");

    feed(t, "done\r\n");
    int hist = tt_history(t);
    assert(hist > 0);

    wsap_vtcore_scroll_to(t, hist / 2);
    struct text_selection s = {0};
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 10, .y = 5 }, false, PTR_PRESS, 1000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 20, .y = 5 }, false, PTR_MOTION, 1010);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 20, .y = 5 }, false, PTR_RELEASE, 1020);
    assert(wsap_vtcore_has_selection(t));

    char *text = wsap_vtcore_selection_text(t);
    assert(text);
    assert(strlen(text) > 0);
    wsap_vtcore_text_free(text);

    selection_handle_pointer(&s, t, (struct vector2i){ .x = 10, .y = 5 }, false, PTR_PRESS, 1100);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 10, .y = 5 }, false, PTR_RELEASE, 1110);
    assert(wsap_vtcore_has_selection(t));
    text = wsap_vtcore_selection_text(t);
    assert(text);
    assert(strlen(text) > 0);
    wsap_vtcore_text_free(text);

    wsap_vtcore_scroll_to(t, hist);
    wsap_vtcore_select_start(t, (struct wsap_vtcore_vector2i){ .x = 0, .y = 0 }, false, WSAP_VTCORE_SELECT_CELL);
    wsap_vtcore_scroll_to(t, 0);
    wsap_vtcore_select_extend(t, (struct wsap_vtcore_vector2i){ .x = COLS - 1, .y = ROWS - 1 }, true);
    assert(wsap_vtcore_has_selection(t));
    text = wsap_vtcore_selection_text(t);
    assert(text);
    assert(strlen(text) >= GLYPHS);
    wsap_vtcore_text_free(text);

    wsap_vtcore_free(t);
}

static void double_click_interval_test(struct wsap_vtcore *t)
{
    feed(t, "hello world\r\nnext row");
    struct text_selection s = { .double_click_ms = 1000 };
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1000);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1010);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1500);
    selection_handle_pointer(&s, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1510);
    selected_text(t, "world");

    struct text_selection slow = { .double_click_ms = 400 };
    selection_handle_pointer(&slow, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1000);
    selection_handle_pointer(&slow, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1010);
    selection_handle_pointer(&slow, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_PRESS, 1500);
    selection_handle_pointer(&slow, t, (struct vector2i){ .x = 7, .y = 0 }, false, PTR_RELEASE, 1510);
    selected_text(t, NULL);
}

int main(void)
{
    struct wsap_vtcore *t = tt_new(30, 4);
    assert(t);
    selection_test(t);
    double_click_interval_test(t);
    selection_scroll_test();
    selection_scrollback_test();
    selection_cluster_test();
    selection_wide_line_columns();
    selection_massive_line_test();
    alternate_test(t);
    wsap_vtcore_free(t);
    puts("PASS: selection and alternate-screen tests");
    return 0;
}
