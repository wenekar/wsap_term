#include "editor/position.h"

#include <stdlib.h>

/* Decodes one code point. Invalid or truncated input gives U+FFFD with a
 * width of one byte, so a scan always moves forward. */
static uint32_t utf8_decode(const char *bytes, uint64_t length, uint64_t *width)
{
    *width = 1;
    if (length == 0)
        return 0xFFFD;
    unsigned char lead = (unsigned char)bytes[0];
    if (lead < 0x80)
        return lead;
    uint64_t need;
    uint32_t code_point;
    uint32_t minimum;
    if ((lead & 0xE0) == 0xC0) {
        need = 1;
        code_point = lead & 0x1Fu;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 2;
        code_point = lead & 0x0Fu;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 3;
        code_point = lead & 0x07u;
        minimum = 0x10000;
    } else {
        return 0xFFFD;
    }
    if (length < need + 1)
        return 0xFFFD;
    for (uint64_t i = 1; i <= need; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if ((byte & 0xC0) != 0x80)
            return 0xFFFD;
        code_point = (code_point << 6) | (byte & 0x3Fu);
    }
    if (code_point < minimum || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF))
        return 0xFFFD;
    *width = need + 1;
    return code_point;
}

static uint32_t code_point_units(uint32_t code_point, enum editor_column column)
{
    if (column == EDITOR_COLUMN_UTF16 && code_point > 0xFFFF)
        return 2;
    return 1;
}

/* Copies the line content into a fresh buffer. */
static char *line_copy(const struct editor_text *text, const struct editor_line *line)
{
    if (line->length > SIZE_MAX)
        return NULL;
    char *bytes = malloc(line->length ? (size_t)line->length : 1);
    if (!bytes)
        return NULL;
    editor_text_copy(text, line->start, line->length, bytes);
    return bytes;
}

bool editor_text_position(const struct editor_text *text, uint64_t offset,
                          enum editor_column column,
                          struct editor_position *position)
{
    if (!text || !position)
        return false;
    if (column != EDITOR_COLUMN_UTF8 && column != EDITOR_COLUMN_UTF16 &&
        column != EDITOR_COLUMN_UTF32)
        return false;
    uint32_t line;
    uint64_t byte_column;
    if (!editor_text_locate(text, offset, &line, &byte_column))
        return false;
    if (byte_column > UINT32_MAX)
        return false;
    if (column == EDITOR_COLUMN_UTF8) {
        position->line = line;
        position->character = (uint32_t)byte_column;
        return true;
    }
    struct editor_line span;
    if (!editor_text_line(text, line, &span))
        return false;
    if (byte_column > span.length)
        return false;
    if (byte_column < span.length) {
        uint8_t next = 0;
        editor_text_copy(text, span.start + byte_column, 1, (char *)&next);
        if ((next & 0xC0) == 0x80)
            return false;
    }
    char *bytes = line_copy(text, &span);
    if (!bytes)
        return false;
    uint64_t index = 0;
    uint64_t units = 0;
    while (index < byte_column) {
        uint64_t width;
        uint32_t code_point = utf8_decode(bytes + index, byte_column - index, &width);
        units += code_point_units(code_point, column);
        index += width;
    }
    free(bytes);
    if (units > UINT32_MAX)
        return false;
    position->line = line;
    position->character = (uint32_t)units;
    return true;
}

bool editor_text_offset_at(const struct editor_text *text,
                           struct editor_position position,
                           enum editor_column column, uint64_t *offset)
{
    if (!text || !offset)
        return false;
    if (column != EDITOR_COLUMN_UTF8 && column != EDITOR_COLUMN_UTF16 &&
        column != EDITOR_COLUMN_UTF32)
        return false;
    struct editor_line line;
    if (!editor_text_line(text, position.line, &line))
        return false;
    if (column == EDITOR_COLUMN_UTF8) {
        if (position.character > line.length)
            return false;
        *offset = line.start + position.character;
        return true;
    }
    char *bytes = line_copy(text, &line);
    if (!bytes)
        return false;
    uint64_t index = 0;
    uint32_t units = 0;
    while (index < line.length) {
        if (units == position.character) {
            *offset = line.start + index;
            free(bytes);
            return true;
        }
        uint64_t width;
        uint32_t code_point = utf8_decode(bytes + index, line.length - index, &width);
        uint32_t count = code_point_units(code_point, column);
        if (units + count > position.character) {
            free(bytes);
            return false;
        }
        units += count;
        index += width;
    }
    free(bytes);
    if (units != position.character)
        return false;
    *offset = line.start + line.length;
    return true;
}
