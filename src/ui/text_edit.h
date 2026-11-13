#ifndef UI_TEXT_EDIT_H
#define UI_TEXT_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include "ui/keys.h"

int ui_text_edit_move(const char *text, int len, int pos,
                       enum ui_edit_unit unit, bool backward);

/* Returns `len` when `offset` is at or past the end of `text`.
 * Otherwise, moves `offset` back over UTF-8 continuation bytes.
 * Returns a character boundary when `text` holds valid UTF-8.
 * The function does not read `text[len]`. */
size_t ui_text_edit_clamp_offset(const char *text, size_t len, size_t offset);

#endif
