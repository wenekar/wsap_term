#include "ui/text_break.h"
#include "paint/text_run.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static size_t gnext(const char *s, size_t len, size_t pos)
{
    return ui_text_move(s, len, pos, UI_EDIT_CHARACTER, false);
}

static size_t gprev(const char *s, size_t len, size_t pos)
{
    return ui_text_move(s, len, pos, UI_EDIT_CHARACTER, true);
}

/* Family ZWJ sequence: Man + ZWJ + Woman + ZWJ + Girl (18 bytes) */
static const char family[] =
    "\xF0\x9F\x91\xA8\xE2\x80\x8D"
    "\xF0\x9F\x91\xA9\xE2\x80\x8D"
    "\xF0\x9F\x91\xA7";

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
        assert(gnext(s, len, prev_end) == bounds[i]);
        assert(gprev(s, len, bounds[i]) == prev_end);
        for (size_t mid = prev_end + 1; mid < bounds[i]; mid++) {
            assert(gnext(s, len, mid) == bounds[i]);
            assert(gprev(s, len, mid) == prev_end);
        }
        prev_end = bounds[i];
    }
    assert(prev_end == len);
    assert(gnext(s, len, len) == len);
    assert(gnext(s, len, len + 5) == len);
    assert(gprev(s, len, 0) == 0);
    assert(gprev(s, len, len + 5) == list.items[n_bounds - 1].start);
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

    assert(gnext(NULL, 0, 0) == 0);
    assert(gnext("abc", 0, 2) == 0);
    assert(gprev(NULL, 0, 0) == 0);
    assert(gprev("abc", 0, 2) == 0);
    assert(ui_text_move(NULL, 0, 0, UI_EDIT_CHARACTER, false) == 0);
    assert(ui_text_move("abc", 0, 0, UI_EDIT_WORD, true) == 0);
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
    assert(gnext(bad, sizeof(bad), 0) == 1);
    assert(gprev(bad, sizeof(bad), 5) == 4);

    struct span_list w = { .limit = 1 };
    ui_text_each_word("one two three", 13, collect_word, &w);
    assert(w.count == 1 && w.items[0].start == 0 && w.items[0].end == 3);

    struct span_list gl = { .limit = 2 };
    ui_text_each_grapheme("abcd", 4, collect_grapheme, &gl);
    assert(gl.count == 2 && gl.items[1].end == 2);
}

static void test_cluster_next_and_fit(void)
{
    int cells = -1;

    size_t next = ui_text_cluster_next("ab", 2, 0, &cells);
    assert(next == 1 && cells == 1);

    /* A combining mark stays with its base letter */
    next = ui_text_cluster_next("e\xCC\x81x", 4, 0, &cells);
    assert(next == 3 && cells == 1);

    /* A ZWJ sequence is one cluster, with the width of its painted run */
    next = ui_text_cluster_next(family, 18, 0, &cells);
    assert(next == 18);
    assert(cells == ui_text_run_cells(family, 18));

    const char flag[] = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";
    next = ui_text_cluster_next(flag, 8, 0, &cells);
    assert(next == 8);

    /* At or past the end, the result is len with zero cells */
    next = ui_text_cluster_next("ab", 2, 2, &cells);
    assert(next == 2 && cells == 0);
    next = ui_text_cluster_next("ab", 2, 9, &cells);
    assert(next == 2 && cells == 0);

    assert(ui_text_fit("hello", 5, 3) == 3);
    assert(ui_text_fit("hello", 5, 0) == 0);
    assert(ui_text_fit("e\xCC\x81x", 4, 1) == 3);

    /* A flag pair is kept whole: it fits at its width, not one cell less */
    const char flag_a[] = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5" "a";
    int flag_w = ui_text_run_cells(flag, 8);
    assert(ui_text_fit(flag_a, 9, flag_w - 1) == 0);
    assert(ui_text_fit(flag_a, 9, flag_w) == 8);

    /* A ZWJ sequence is never split */
    assert(ui_text_fit(family, 18, ui_text_run_cells(family, 18) - 1) == 0);
}

/* A wrap opportunity comes after a space run or a hyphen. */
static void test_break_before(void)
{
    const char *word = "alpha beta";
    size_t len = strlen(word);
    /* The space run ends at 6. */
    assert(ui_text_break_before(word, len, 0, 6) == 6);
    assert(ui_text_break_before(word, len, 0, 5) == 0);
    assert(ui_text_break_before(word, len, 6, len) == 6);
    assert(ui_text_break_before(word, len, 6, 6) == 6);

    const char *hyphen = "well-known";
    len = strlen(hyphen);
    assert(ui_text_break_before(hyphen, len, 0, 5) == 5);

    /* A full stop is not a wrap opportunity. */
    const char *dots = "foo.bar";
    len = strlen(dots);
    assert(ui_text_break_before(dots, len, 0, 3) == 0);

    /* An ideographic space is a wrap opportunity. */
    const char *ideographic = "ab\xE3\x80\x80" "cd";
    len = strlen(ideographic);
    assert(ui_text_break_before(ideographic, len, 0, 5) == 5);

    /* A no-break space is not. */
    const char *no_break = "ab\xC2\xA0" "cd";
    len = strlen(no_break);
    assert(ui_text_break_before(no_break, len, 0, 4) == 0);

    /* A limit past the end stops at the end of the text. */
    assert(ui_text_break_before(word, strlen(word), 0, 99) == 6);
}

static void test_offset_at_x(void)
{
    assert(ui_text_offset_at_x(NULL, 0, 0, 10) == 0);
    assert(ui_text_offset_at_x("abc", 3, -5, 10) == 0);
    assert(ui_text_offset_at_x("abc", 3, 4, 10) == 0);
    assert(ui_text_offset_at_x("abc", 3, 5, 10) == 1);
    assert(ui_text_offset_at_x("abc", 3, 14, 10) == 1);
    assert(ui_text_offset_at_x("abc", 3, 15, 10) == 2);
    assert(ui_text_offset_at_x("abc", 3, 1000, 10) == 3);

    /* A wide cluster is never split: the result is its start or its end */
    int w = ui_text_run_cells(family, 18);
    int64_t half = ((int64_t)w * 10 + 1) / 2;
    assert(ui_text_offset_at_x(family, 18, half - 1, 10) == 0);
    assert(ui_text_offset_at_x(family, 18, half, 10) == 18);
    for (int64_t x = -10; x <= (int64_t)w * 10 + 10; x++) {
        size_t off = ui_text_offset_at_x(family, 18, x, 10);
        assert(off == 0 || off == 18);
    }
}

static void test_unit_range(void)
{
    const char *text = "Hello caf\xC3\xA9 world\nSecond";
    size_t len = strlen(text);
    size_t s = 0;
    size_t e = 0;

    /* A word holds the whole accented word */
    ui_text_unit_range(text, len, 8, UI_EDIT_WORD, &s, &e);
    assert(s == 6 && e == 11);
    ui_text_unit_range(text, len, 8, UI_EDIT_WORD_END, &s, &e);
    assert(s == 6 && e == 11);

    /* A space run is its own word segment */
    ui_text_unit_range(text, len, 11, UI_EDIT_WORD, &s, &e);
    assert(s == 11 && e == 12);

    /* A line includes its trailing newline */
    ui_text_unit_range(text, len, 2, UI_EDIT_LINE, &s, &e);
    assert(s == 0 && e == 18);
    ui_text_unit_range(text, len, 18, UI_EDIT_LINE, &s, &e);
    assert(s == 18 && e == len);

    /* At len, the last line is used */
    ui_text_unit_range(text, len, len, UI_EDIT_LINE, &s, &e);
    assert(s == 18 && e == len);

    /* A character holds a base letter with its combining mark */
    ui_text_unit_range("e\xCC\x81x", 4, 1, UI_EDIT_CHARACTER, &s, &e);
    assert(s == 0 && e == 3);

    /* A character inside a ZWJ sequence holds the whole sequence */
    ui_text_unit_range(family, 18, 5, UI_EDIT_CHARACTER, &s, &e);
    assert(s == 0 && e == 18);

    /* Punctuation inside a word stays with the word */
    ui_text_unit_range("foo.bar baz", 11, 1, UI_EDIT_WORD, &s, &e);
    assert(s == 0 && e == 7);

    ui_text_unit_range(NULL, 0, 0, UI_EDIT_WORD, &s, &e);
    assert(s == 0 && e == 0);
    ui_text_unit_range("abc", 0, 0, UI_EDIT_LINE, &s, &e);
    assert(s == 0 && e == 0);
}

static void test_move(void)
{
    const char *line = "don't 3.14 \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9 end";
    size_t len = strlen(line);

    assert(ui_text_move(line, len, 0, UI_EDIT_LINE, false) == len);
    assert(ui_text_move(line, len, len, UI_EDIT_LINE, true) == 0);

    /* Word forward jumps to next word start */
    size_t p = ui_text_move(line, len, 0, UI_EDIT_WORD, false);
    assert(p == 6);
    p = ui_text_move(line, len, p, UI_EDIT_WORD, false);
    assert(p == len - 3);
    p = ui_text_move(line, len, p, UI_EDIT_WORD, false);
    assert(p == len);

    /* Word end forward jumps to end of word */
    assert(ui_text_move(line, len, 0, UI_EDIT_WORD_END, false) == 5);
    assert(ui_text_move(line, len, 5, UI_EDIT_WORD_END, false) == 10);

    /* Word backward jumps to previous word start */
    p = ui_text_move(line, len, len, UI_EDIT_WORD, true);
    assert(p == len - 3);
    p = ui_text_move(line, len, p, UI_EDIT_WORD, true);
    assert(p == 6);
    p = ui_text_move(line, len, p, UI_EDIT_WORD, true);
    assert(p == 0);

    /* Character move treats ZWJ emoji sequence as one grapheme cluster */
    size_t emoji_start = 11;
    size_t emoji_end = 11 + 11;
    assert(ui_text_move(line, len, emoji_start, UI_EDIT_CHARACTER, false) == emoji_end);
    assert(ui_text_move(line, len, emoji_end, UI_EDIT_CHARACTER, true) == emoji_start);
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
    test_cluster_next_and_fit();
    test_break_before();
    test_offset_at_x();
    test_unit_range();
    test_move();
    return 0;
}
