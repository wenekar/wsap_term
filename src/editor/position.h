#ifndef EDITOR_POSITION_H
#define EDITOR_POSITION_H

#include <stdbool.h>
#include <stdint.h>

#include "editor/text.h"

/* A zero-based position in a text, as the Language Server Protocol uses it. */
struct editor_position {
    uint32_t line;
    uint32_t character;
};

/* The code unit that a position column counts. */
enum editor_column {
    /* A UTF-8 byte. */
    EDITOR_COLUMN_UTF8 = 0,
    /* A UTF-16 code unit: two units for a code point above U+FFFF. */
    EDITOR_COLUMN_UTF16,
    /* A Unicode code point. */
    EDITOR_COLUMN_UTF32,
};

/* Converts a byte offset to a position. Returns false past the end of the
 * text. A UTF-16 or UTF-32 position also fails inside a UTF-8 sequence. */
bool editor_text_position(const struct editor_text *text, uint64_t offset,
                          enum editor_column column,
                          struct editor_position *position);

/* Converts a position to a byte offset. Returns false past the end of the
 * line, or inside a code point. */
bool editor_text_offset_at(const struct editor_text *text,
                           struct editor_position position,
                           enum editor_column column, uint64_t *offset);

#endif
