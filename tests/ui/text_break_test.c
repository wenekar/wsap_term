#include "ui/text_break.h"
#include "ui/text_edit.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

struct span {
    size_t start;
    size_t end;
    bool is_word;
};

#define MAX_SPANS 64

struct span_list {
    struct span items[MAX_SPANS];
    size_t count;
    size_t limit;
};

static bool collect_grapheme(size_t start, size_t end, void *userdata)
{
    struct span_list *list = userdata;
    assert(list->count < MAX_SPANS);
    list->items[list->count++] = (struct span){ .start = start, .end = end };
    return !list->limit || list->count < list->limit;
}

static bool collect_word(size_t start, size_t end, bool is_word, void *userdata)
{
    struct span_list *list = userdata;
    assert(list->count < MAX_SPANS);
    list->items[list->count++] = (struct span){
        .start = start, .end = end, .is_word = is_word,
    };
    return !list->limit || list->count < list->limit;
}

static void check_graphemes(const char *s, const size_t *bounds, size_t n_bounds)
{
    size_t len = strlen(s);
    struct span_list list = {0};
    ui_text_each_grapheme(s, len, collect_grapheme, &list);
    assert(list.count == n_bounds);

    size_t prev_end = 0;
    for (size_t i = 0; i < n_bounds; i++) {
        assert(list.items[i].start == prev_end);
        assert(list.items[i].end == bounds[i]);
        assert(ui_text_break_grapheme_next(s, len, prev_end) == bounds[i]);
        assert(ui_text_break_grapheme_prev(s, len, bounds[i]) == prev_end);
        for (size_t mid = prev_end + 1; mid < bounds[i]; mid++) {
            assert(ui_text_break_grapheme_next(s, len, mid) == bounds[i]);
            assert(ui_text_break_grapheme_prev(s, len, mid) == prev_end);
        }
        prev_end = bounds[i];
    }
    assert(prev_end == len);
    assert(ui_text_break_grapheme_next(s, len, len) == len);
    assert(ui_text_break_grapheme_next(s, len, len + 5) == len);
    assert(ui_text_break_grapheme_prev(s, len, 0) == 0);
    assert(ui_text_break_grapheme_prev(s, len, len + 5) ==
           list.items[n_bounds - 1].start);
}

static void check_words(const char *s, const struct span *expected, size_t n)
{
    size_t len = strlen(s);
    struct span_list list = {0};
    ui_text_each_word(s, len, collect_word, &list);
    assert(list.count == n);
    for (size_t i = 0; i < n; i++) {
        assert(list.items[i].start == expected[i].start);
        assert(list.items[i].end == expected[i].end);
        assert(list.items[i].is_word == expected[i].is_word);
    }
}

static void test_empty_and_null(void)
{
    struct span_list list = {0};
    ui_text_each_grapheme(NULL, 0, collect_grapheme, &list);
    ui_text_each_grapheme("abc", 0, collect_grapheme, &list);
    ui_text_each_word(NULL, 0, collect_word, &list);
    ui_text_each_word("abc", 0, collect_word, &list);
    assert(list.count == 0);

    assert(ui_text_break_grapheme_next(NULL, 0, 0) == 0);
    assert(ui_text_break_grapheme_next("abc", 0, 2) == 0);
    assert(ui_text_break_grapheme_prev(NULL, 0, 0) == 0);
    assert(ui_text_break_grapheme_prev("abc", 0, 2) == 0);
    assert(ui_text_edit_move(NULL, 0, 0, UI_EDIT_CHARACTER, false) == 0);
    assert(ui_text_edit_move("abc", 0, 0, UI_EDIT_WORD, true) == 0);
}

static void test_grapheme_ascii_and_crlf(void)
{
    check_graphemes("a\r\nb", (const size_t[]){ 1, 3, 4 }, 3);
    check_graphemes("\r\n\n\r", (const size_t[]){ 2, 3, 4 }, 3);

    const char nul_str[] = "a\0b";
    struct span_list list = {0};
    ui_text_each_grapheme(nul_str, 3, collect_grapheme, &list);
    assert(list.count == 3);
    assert(list.items[0].start == 0 && list.items[0].end == 1);
    assert(list.items[1].start == 1 && list.items[1].end == 2);
    assert(list.items[2].start == 2 && list.items[2].end == 3);
}

static void test_grapheme_combining_and_hangul(void)
{
    /* e + U+0301 + U+0327 (1 + 2 + 2 bytes), then x */
    check_graphemes("e\xCC\x81\xCC\xA7x", (const size_t[]){ 5, 6 }, 2);

    /* Hangul Jamo L + V + T (3 + 3 + 3 bytes) and LV (U+AC00) + T (U+11A8) */
    check_graphemes("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8"
                    "\xEA\xB0\x80\xE1\x86\xA8",
                    (const size_t[]){ 9, 15 }, 2);
}

static void test_grapheme_emoji_zwj_and_flags(void)
{
    /* Thumbs up + medium skin tone: U+1F44D (4B) + U+1F3FD (4B) */
    check_graphemes("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD!",
                    (const size_t[]){ 8, 9 }, 2);

    /* Family ZWJ sequence: Man + ZWJ + Woman + ZWJ + Girl */
    const char *family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7";
    check_graphemes(family, (const size_t[]){ 18 }, 1);

    /* ExtPict + Extend + ZWJ + ExtPict stays together */
    check_graphemes("\xF0\x9F\x91\xA8\xCC\x80\xE2\x80\x8D\xF0\x9F\x91\xA9",
                    (const size_t[]){ 13 }, 1);

    /* ExtPict + ZWJ + Extend + ExtPict breaks before second ExtPict */
    check_graphemes("\xF0\x9F\x91\xA8\xE2\x80\x8D\xCC\x80\xF0\x9F\x91\xA9",
                    (const size_t[]){ 9, 13 }, 2);

    /* Regional indicator flag pairs: JP + US + unpaired FR-first */
    const char *flags =
        "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"
        "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"
        "\xF0\x9F\x87\xAB";
    check_graphemes(flags, (const size_t[]){ 8, 16, 20 }, 3);

    /* Extend resets RI parity in grapheme breaking */
    check_graphemes("\xF0\x9F\x87\xAF\xCC\x80\xF0\x9F\x87\xB5\xF0\x9F\x87\xBA",
                    (const size_t[]){ 6, 14 }, 2);
}

static void test_grapheme_indic_conjuncts(void)
{
    /* Devanagari Ka (U+0915) + Virama (U+094D) + Ssa (U+0937) */
    check_graphemes("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7",
                    (const size_t[]){ 9 }, 1);

    /* Ka + Nukta (U+093C, InCB=Extend) + Virama + Ssa + Virama + Ta (U+0924) */
    check_graphemes("\xE0\xA4\x95\xE0\xA4\xBC\xE0\xA5\x8D"
                    "\xE0\xA4\xB7\xE0\xA5\x8D\xE0\xA4\xA4",
                    (const size_t[]){ 18 }, 1);

    /* Ka + Virama + ZWJ (U+200D, InCB=Extend) + Ssa */
    check_graphemes("\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8D\xE0\xA4\xB7",
                    (const size_t[]){ 12 }, 1);

    /* Ka + Virama + Space + Ssa breaks after Virama and Space */
    check_graphemes("\xE0\xA4\x95\xE0\xA5\x8D \xE0\xA4\xB7",
                    (const size_t[]){ 6, 7, 10 }, 3);
}

static void test_word_breaks(void)
{
    check_words("foo  bar_baz qux", (const struct span[]){
        { 0, 3, true },
        { 3, 5, false },
        { 5, 12, true },
        { 12, 13, false },
        { 13, 16, true },
    }, 5);

    check_words("3.14 1,000 don't 'hi'", (const struct span[]){
        { 0, 4, true },
        { 4, 5, false },
        { 5, 10, true },
        { 10, 11, false },
        { 11, 16, true },
        { 16, 17, false },
        { 17, 18, false },
        { 18, 20, true },
        { 20, 21, false },
    }, 9);

    /* Standalone underscore is not a word, attached underscore is a word */
    check_words("_ _a1", (const struct span[]){
        { 0, 1, false },
        { 1, 2, false },
        { 2, 5, true },
    }, 3);

    /* Hebrew WB7a (HL x SQ) and WB7b/7c (HL x DQ HL) */
    const char *heb = "\xD7\x90' \xD7\x90\"\xD7\x91 \xD7\x90\"";
    check_words(heb, (const struct span[]){
        { 0, 3, true },
        { 3, 4, false },
        { 4, 9, true },
        { 9, 10, false },
        { 10, 12, true },
        { 12, 13, false },
    }, 6);

    /* Katakana stays joined; Han/Hiragana are word characters; emoji is non-word */
    const char *cjk = "\xE3\x82\xAB\xE3\x82\xBF \xE6\xBC\xA2\xE3\x81\x82 \xF0\x9F\x98\x80";
    check_words(cjk, (const struct span[]){
        { 0, 6, true },
        { 6, 7, false },
        { 7, 10, true },
        { 10, 13, true },
        { 13, 14, false },
        { 14, 18, false },
    }, 6);

    /* RI pairs in word breaking ignore Extend; WSegSpace does not ignore Extend */
    check_words("\xF0\x9F\x87\xAF\xCC\x80\xF0\x9F\x87\xB5 \xCC\x80 ",
                (const struct span[]){
        { 0, 10, false },
        { 10, 13, false },
        { 13, 14, false },
    }, 3);
}

static void test_invalid_utf8_and_early_stop(void)
{
    const char bad[] = { (char)0xFF, (char)0x80, 'a', (char)0xC3, 'b' };
    struct span_list g = {0};
    ui_text_each_grapheme(bad, sizeof(bad), collect_grapheme, &g);
    assert(g.count == 5);
    assert(ui_text_break_grapheme_next(bad, sizeof(bad), 0) == 1);
    assert(ui_text_break_grapheme_prev(bad, sizeof(bad), 5) == 4);

    struct span_list w = { .limit = 1 };
    ui_text_each_word("one two three", 13, collect_word, &w);
    assert(w.count == 1 && w.items[0].start == 0 && w.items[0].end == 3);

    struct span_list gl = { .limit = 2 };
    ui_text_each_grapheme("abcd", 4, collect_grapheme, &gl);
    assert(gl.count == 2 && gl.items[1].end == 2);
}

static void test_text_edit_move(void)
{
    const char *line = "don't 3.14 \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9 end";
    int len = (int)strlen(line);

    assert(ui_text_edit_move(line, len, 0, UI_EDIT_LINE, false) == len);
    assert(ui_text_edit_move(line, len, len, UI_EDIT_LINE, true) == 0);

    /* Word forward jumps to next word start */
    int p = ui_text_edit_move(line, len, 0, UI_EDIT_WORD, false);
    assert(p == 6);
    p = ui_text_edit_move(line, len, p, UI_EDIT_WORD, false);
    assert(p == len - 3);
    p = ui_text_edit_move(line, len, p, UI_EDIT_WORD, false);
    assert(p == len);

    /* Word end forward jumps to end of word */
    assert(ui_text_edit_move(line, len, 0, UI_EDIT_WORD_END, false) == 5);
    assert(ui_text_edit_move(line, len, 5, UI_EDIT_WORD_END, false) == 10);

    /* Word backward jumps to previous word start */
    p = ui_text_edit_move(line, len, len, UI_EDIT_WORD, true);
    assert(p == len - 3);
    p = ui_text_edit_move(line, len, p, UI_EDIT_WORD, true);
    assert(p == 6);
    p = ui_text_edit_move(line, len, p, UI_EDIT_WORD, true);
    assert(p == 0);

    /* Character move treats ZWJ emoji sequence as one grapheme cluster */
    int emoji_start = 11;
    int emoji_end = 11 + 11;
    assert(ui_text_edit_move(line, len, emoji_start, UI_EDIT_CHARACTER, false) == emoji_end);
    assert(ui_text_edit_move(line, len, emoji_end, UI_EDIT_CHARACTER, true) == emoji_start);
}

int main(void)
{
    test_empty_and_null();
    test_grapheme_ascii_and_crlf();
    test_grapheme_combining_and_hangul();
    test_grapheme_emoji_zwj_and_flags();
    test_grapheme_indic_conjuncts();
    test_word_breaks();
    test_invalid_utf8_and_early_stop();
    test_text_edit_move();
    return 0;
}
