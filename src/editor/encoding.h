#ifndef EDITOR_ENCODING_H
#define EDITOR_ENCODING_H

#include <stdbool.h>
#include <stddef.h>

/* The text encodings of the files that the editor reads and writes. The
 * document always holds UTF-8. A conversion happens at the file boundary. */
enum editor_encoding {
    EDITOR_ENCODING_UTF8 = 0,
    EDITOR_ENCODING_UTF16_LE,
    EDITOR_ENCODING_UTF16_BE,
    EDITOR_ENCODING_LATIN1,
    EDITOR_ENCODING_WINDOWS_1252,
};

/* What a file holds. */
struct editor_encoding_info {
    enum editor_encoding encoding;
    /* The file starts with a byte order mark. */
    bool bom;
};

/* Finds the encoding of `length` bytes. A byte order mark wins. A NUL byte in
 * every other position selects UTF-16 without a mark. Valid UTF-8 selects
 * UTF-8. A printable byte in 0x80 to 0x9F selects Windows-1252. All other
 * bytes select Latin-1. */
struct editor_encoding_info editor_encoding_detect(const char *bytes, size_t length);

/* True when the bytes are well-formed UTF-8. */
bool editor_encoding_utf8_valid(const char *bytes, size_t length);

/* Converts the file bytes to UTF-8. Returns a buffer with a NUL terminator, or
 * NULL with errno set. `length` is optional. An invalid byte becomes U+FFFD. */
char *editor_encoding_decode(const char *bytes, size_t length,
                             struct editor_encoding_info info, size_t *out_length);

/* Converts UTF-8 to the file encoding. `bom` adds a byte order mark. Returns a
 * buffer, or NULL with errno set. `length` is optional. A code point that the
 * encoding cannot hold becomes '?'. */
char *editor_encoding_encode(const char *utf8, size_t length,
                             struct editor_encoding_info info, size_t *out_length);

/* The name of an encoding. It is the file name extension that the editor
 * shows, such as "UTF-8". */
const char *editor_encoding_name(enum editor_encoding encoding);

#endif
