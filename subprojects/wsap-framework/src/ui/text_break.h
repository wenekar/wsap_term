#ifndef WSAP_UI_TEXT_BREAK_H
#define WSAP_UI_TEXT_BREAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/keys.h"

/* Caret movement, click mapping, selection, word wrapping, and measurement
 * for UI text. All offsets are byte offsets into UTF-8 text. */

typedef bool (*ui_text_grapheme_fn)(size_t start, size_t end, void *userdata);
typedef bool (*ui_text_word_fn)(size_t start, size_t end, bool is_word,
                                void *userdata);

/* Calls `fn` with the range of each grapheme cluster. `fn` returns false to
 * stop. Call it to build a list of cluster boundaries, as accessibility
 * does. */
void ui_text_each_grapheme(const char *text, size_t len,
                           ui_text_grapheme_fn fn, void *userdata);

/* Calls `fn` with the range of each word, space run, and punctuation mark.
 * `is_word` is true only for words. `fn` returns false to stop.
 * Call it to build a list of word boundaries, as accessibility does. */
void ui_text_each_word(const char *text, size_t len,
                       ui_text_word_fn fn, void *userdata);

/* Returns the caret position after one `unit` move from `pos`.
 * Call it for arrow keys, word jumps, and the range of a delete key. */
size_t ui_text_move(const char *text, size_t len, size_t pos,
                    enum ui_edit_unit unit, bool backward);

/* Sets `[*start, *end)` to the character, word, or line at `pos`. A line
 * includes its '\n'. At `pos == len` it uses the last one.
 * Call it to select on a double-click (word) or a triple-click (line). */
void ui_text_unit_range(const char *text, size_t len, size_t pos,
                        enum ui_edit_unit unit, size_t *start, size_t *end);

/* Returns the end of the last word-wrap opportunity in (`start`, `limit`],
 * or `start` when there is none. A wrap opportunity comes after a space run
 * or after a hyphen.
 * Call it to choose where a wrapped line ends. */
size_t ui_text_break_before(const char *text, size_t len, size_t start,
                            size_t limit);

/* Returns the end of the character at `pos` and sets `*cells` to its width.
 * The width is the width that the painter draws.
 * Call it to lay out text one character at a time, as line wrapping does. */
size_t ui_text_cluster_next(const char *text, size_t len, size_t pos,
                            int *cells);

/* Returns the caret position nearest to `x` pixels from the text start.
 * Call it to put the caret where the user clicks. */
size_t ui_text_offset_at_x(const char *text, size_t len, int64_t x,
                           int advance);

/* Returns how many bytes of `text` fit in `cap` cells. It never cuts a
 * character in two.
 * Call it before you paint a title or a label that can be too long. */
size_t ui_text_fit(const char *text, size_t len, int cap);

/* Returns `offset` moved back to the start of its code point, or `len`
 * when it is past the end.
 * Call it on offsets that come from outside, such as accessibility. */
size_t ui_text_clamp_offset(const char *text, size_t len, size_t offset);

#endif
