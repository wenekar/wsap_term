#ifndef EDITOR_EDIT_H
#define EDITOR_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The bracket characters that the editor matches and closes. */
enum editor_bracket_kind {
    EDITOR_BRACKET_NONE = 0,
    EDITOR_BRACKET_PAREN,
    EDITOR_BRACKET_SQUARE,
    EDITOR_BRACKET_BRACE,
    EDITOR_BRACKET_ANGLE,
};

/* The comment tokens of a language. An empty token turns the feature off. */
struct editor_comment_style {
    /* The token that starts a comment at any place of a line, such as "//". */
    const char *line;
    /* The tokens that wrap a block comment. A C comment uses a slash-star
     * start and a star-slash end. */
    const char *block_start;
    const char *block_end;
};

/* The language rules that the edit engine uses. */
struct editor_language {
    const char *name;
    struct editor_comment_style comments;
    /* The characters of a quote pair. An empty string turns the rule off. */
    const char *quotes;
    /* One level of indentation, as a byte string. */
    const char *indent;
};

/* A direction of a bracket match. */
enum editor_bracket_search {
    EDITOR_BRACKET_FORWARD = 0,
    EDITOR_BRACKET_BACKWARD,
};

/* The bracket kind of `c`, or `EDITOR_BRACKET_NONE`. */
enum editor_bracket_kind editor_bracket_kind(char c);
/* True when `c` closes a bracket of the kind. */
bool editor_bracket_closes(enum editor_bracket_kind kind, char c);
/* The character that closes a bracket of the kind. */
char editor_bracket_close(enum editor_bracket_kind kind);
/* The character that opens a bracket of the kind. */
char editor_bracket_open(enum editor_bracket_kind kind);

/* Finds the bracket that pairs with the bracket at `offset`. The search skips a
 * bracket inside a quoted string when the language has quotes. It returns
 * false when the text holds no pair. */
bool editor_bracket_match(const char *text, size_t length,
                          const struct editor_language *language, size_t offset,
                          size_t *match);

/* What an inserted character does to the text. */
enum editor_auto_close_action {
    /* The character stays as typed. */
    EDITOR_AUTO_CLOSE_NONE = 0,
    /* The editor also inserts the closing character. */
    EDITOR_AUTO_CLOSE_PAIR,
    /* The character is the closing character that is already at the caret, so
     * the editor moves the caret and inserts nothing. */
    EDITOR_AUTO_CLOSE_SKIP,
};

struct editor_auto_close_result {
    enum editor_auto_close_action action;
    /* The bytes to insert after the typed character. */
    char close;
};

/* Decides what the editor does when `typed` arrives at `offset` of the text.
 * A closing character skips when it is already there. An opening character
 * pairs when the language holds its closing character in `quotes`. */
struct editor_auto_close_result editor_auto_close(const char *text, size_t length,
                                                  const struct editor_language *language,
                                                  size_t offset, char typed);

/* The indentation in bytes that the editor writes at the start of the next line
 * after a line that ends at `offset`. It holds the indentation of the current
 * line, plus one level after an opening bracket, and minus one level after a
 * closing bracket. `indent` and `length` are optional. */
size_t editor_smart_indent(const char *text, size_t length,
                           const struct editor_language *language, size_t offset,
                           char *indent, size_t capacity);

/* The byte column of the first comment token of the line that holds `offset`,
 * or the line length when the line has none. A token inside a quoted string
 * does not count. */
size_t editor_comment_start(const char *text, size_t length,
                            const struct editor_language *language, size_t offset);

#endif
