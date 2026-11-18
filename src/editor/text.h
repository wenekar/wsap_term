#ifndef EDITOR_TEXT_H
#define EDITOR_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct editor_text;

/* The line ending that a text uses. */
enum editor_line_ending {
    EDITOR_LINE_LF = 0,
    EDITOR_LINE_CRLF,
};

/* One line of the line index. The offsets count bytes from the text start. */
struct editor_line {
    /* The first byte of the line. */
    uint64_t start;
    /* The byte length of the line, without its terminator. */
    uint64_t length;
    /* The first byte after the terminator. It is the text length for the
     * last line. */
    uint64_t end;
};

struct editor_text *editor_text_create(void);
void editor_text_destroy(struct editor_text *text);

/* Replaces all text with `length` bytes from `bytes`. */
bool editor_text_set(struct editor_text *text, const char *bytes, size_t length);

uint64_t editor_text_length(const struct editor_text *text);

/* The number of line breaks. A CRLF pair is one line break. */
uint64_t editor_text_newline_count(const struct editor_text *text);
/* The number of CRLF pairs. A CRLF pair that a piece boundary splits still
 * counts. */
uint64_t editor_text_crlf_count(const struct editor_text *text);
/* CRLF when the text holds at least one CRLF pair, or LF. */
enum editor_line_ending editor_text_line_ending(const struct editor_text *text);
/* The number of lines. A final line break starts an empty last line. */
uint32_t editor_text_line_count(const struct editor_text *text);

/* Inserts `length` bytes at `offset`. */
bool editor_text_insert(struct editor_text *text, uint64_t offset, const char *bytes, size_t length);
/* Removes `length` bytes at `offset`. */
bool editor_text_delete(struct editor_text *text, uint64_t offset, uint64_t length);
/* Replaces `remove` bytes at `offset` with `length` bytes from `bytes`. */
bool editor_text_replace(struct editor_text *text, uint64_t offset, uint64_t remove,
                         const char *bytes, size_t length);

/* Copies up to `length` bytes of the text from `offset` into `bytes`.
 * Returns the byte count that it copied. */
uint64_t editor_text_copy(const struct editor_text *text, uint64_t offset,
                          uint64_t length, char *bytes);

/* Returns a NUL-terminated copy of the whole text, or NULL on failure. */
char *editor_text_dup(const struct editor_text *text, size_t *length);

/* Reads one line of the line index. Returns false past the last line. */
bool editor_text_line(const struct editor_text *text, uint32_t index,
                      struct editor_line *line);

/* Finds the line and the byte column of `offset`. A column counts bytes from
 * the line start. `line` and `column` are optional. Returns false when the
 * offset is past the end of the text. */
bool editor_text_locate(const struct editor_text *text, uint64_t offset,
                        uint32_t *line, uint64_t *column);

#endif
