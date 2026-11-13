#ifndef WSAP_UI_TEXT_BREAK_H
#define WSAP_UI_TEXT_BREAK_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*ui_text_grapheme_fn)(size_t start, size_t end, void *userdata);
typedef bool (*ui_text_word_fn)(size_t start, size_t end, bool is_word,
                                void *userdata);

/* Calls `fn` for each UAX #29 extended grapheme cluster `[start, end)` in
 * `text[0..len)`. Stops early when `fn` returns `false`. */
void ui_text_each_grapheme(const char *text, size_t len,
                           ui_text_grapheme_fn fn, void *userdata);

/* Calls `fn` for each UAX #29 word segment `[start, end)` in `text[0..len)`.
 * `is_word` is `true` for word tokens and `false` for spaces and punctuation.
 * Stops early when `fn` returns `false`. */
void ui_text_each_word(const char *text, size_t len,
                       ui_text_word_fn fn, void *userdata);

/* Returns the first grapheme cluster boundary strictly after `pos`, or `len`
 * when `pos >= len`. */
size_t ui_text_break_grapheme_next(const char *text, size_t len, size_t pos);

/* Returns the last grapheme cluster boundary strictly before `pos`, or `0`
 * when `pos == 0`. */
size_t ui_text_break_grapheme_prev(const char *text, size_t len, size_t pos);

#endif
