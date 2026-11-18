#define _GNU_SOURCE

#include "editor/edit.h"

#include <string.h>

enum editor_bracket_kind editor_bracket_kind(char c)
{
    switch (c) {
    case '(':
    case ')':
        return EDITOR_BRACKET_PAREN;
    case '[':
    case ']':
        return EDITOR_BRACKET_SQUARE;
    case '{':
    case '}':
        return EDITOR_BRACKET_BRACE;
    case '<':
    case '>':
        return EDITOR_BRACKET_ANGLE;
    default:
        break;
    }
    return EDITOR_BRACKET_NONE;
}

char editor_bracket_open(enum editor_bracket_kind kind)
{
    switch (kind) {
    case EDITOR_BRACKET_PAREN:
        return '(';
    case EDITOR_BRACKET_SQUARE:
        return '[';
    case EDITOR_BRACKET_BRACE:
        return '{';
    case EDITOR_BRACKET_ANGLE:
        return '<';
    case EDITOR_BRACKET_NONE:
        break;
    }
    return '\0';
}

char editor_bracket_close(enum editor_bracket_kind kind)
{
    switch (kind) {
    case EDITOR_BRACKET_PAREN:
        return ')';
    case EDITOR_BRACKET_SQUARE:
        return ']';
    case EDITOR_BRACKET_BRACE:
        return '}';
    case EDITOR_BRACKET_ANGLE:
        return '>';
    case EDITOR_BRACKET_NONE:
        break;
    }
    return '\0';
}

bool editor_bracket_closes(enum editor_bracket_kind kind, char c)
{
    return kind != EDITOR_BRACKET_NONE && editor_bracket_close(kind) == c;
}

static bool is_opening(char c)
{
    return c == '(' || c == '[' || c == '{' || c == '<';
}

/* True when the byte at `offset` is inside a quoted string of the line. */
static bool inside_quote(const char *text, const struct editor_language *language,
                         size_t offset)
{
    if (!language || !language->quotes || language->quotes[0] == '\0')
        return false;
    size_t start = offset;
    while (start > 0 && text[start - 1] != '\n')
        start--;
    char quote = '\0';
    for (size_t i = start; i < offset; i++) {
        char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                i++;
                continue;
            }
            if (c == quote)
                quote = '\0';
            continue;
        }
        if (strchr(language->quotes, c) != NULL)
            quote = c;
    }
    return quote != '\0';
}

bool editor_bracket_match(const char *text, size_t length,
                          const struct editor_language *language, size_t offset,
                          size_t *match)
{
    if (!text || offset >= length)
        return false;
    char target = text[offset];
    enum editor_bracket_kind kind = editor_bracket_kind(target);
    if (kind == EDITOR_BRACKET_NONE)
        return false;
    bool forward = is_opening(target);
    char want = forward ? editor_bracket_close(kind) : editor_bracket_open(kind);
    char skip = forward ? editor_bracket_open(kind) : editor_bracket_close(kind);
    int depth = 0;
    if (forward) {
        for (size_t i = offset + 1; i < length; i++) {
            if (inside_quote(text, language, i))
                continue;
            if (text[i] == skip)
                depth++;
            else if (text[i] == want) {
                if (depth == 0) {
                    if (match)
                        *match = i;
                    return true;
                }
                depth--;
            }
        }
        return false;
    }
    for (size_t i = offset; i > 0;) {
        i--;
        if (inside_quote(text, language, i))
            continue;
        if (text[i] == skip)
            depth++;
        else if (text[i] == want) {
            if (depth == 0) {
                if (match)
                    *match = i;
                return true;
            }
            depth--;
        }
    }
    return false;
}

static bool quotes_hold(const struct editor_language *language, char close)
{
    if (!language || !language->quotes)
        return false;
    return strchr(language->quotes, close) != NULL;
}

struct editor_auto_close_result editor_auto_close(const char *text, size_t length,
                                                  const struct editor_language *language,
                                                  size_t offset, char typed)
{
    struct editor_auto_close_result result = {EDITOR_AUTO_CLOSE_NONE, '\0'};
    if (!text || offset > length)
        return result;
    if (quotes_hold(language, typed)) {
        /* The quote that is already at the caret moves the caret. */
        if (offset < length && text[offset] == typed) {
            result.action = EDITOR_AUTO_CLOSE_SKIP;
            return result;
        }
        result.action = EDITOR_AUTO_CLOSE_PAIR;
        result.close = typed;
        return result;
    }
    enum editor_bracket_kind kind = editor_bracket_kind(typed);
    if (kind == EDITOR_BRACKET_NONE)
        return result;
    if (!is_opening(typed)) {
        if (offset < length && text[offset] == typed)
            result.action = EDITOR_AUTO_CLOSE_SKIP;
        return result;
    }
    result.action = EDITOR_AUTO_CLOSE_PAIR;
    result.close = editor_bracket_close(kind);
    return result;
}

/* The byte length of the indentation of the line that ends at `offset`. A
 * break at `offset` ends the line before it. */
static size_t line_indent(const char *text, size_t offset, size_t *start)
{
    size_t begin = offset;
    while (begin > 0 && (text[begin - 1] == '\n' || text[begin - 1] == '\r'))
        begin--;
    while (begin > 0 && text[begin - 1] != '\n')
        begin--;
    if (start)
        *start = begin;
    size_t indent = 0;
    while (begin + indent < offset) {
        char c = text[begin + indent];
        if (c != ' ' && c != '\t')
            break;
        indent++;
    }
    return indent;
}

size_t editor_smart_indent(const char *text, size_t length,
                           const struct editor_language *language, size_t offset,
                           char *indent, size_t capacity)
{
    if (!text || offset > length)
        return 0;
    size_t start = 0;
    size_t base = line_indent(text, offset, &start);
    /* The last byte of the line before the break selects the level. */
    size_t last = offset;
    while (last > start && (text[last - 1] == '\n' || text[last - 1] == '\r' ||
                            text[last - 1] == ' ' || text[last - 1] == '\t'))
        last--;
    char previous = last > start ? text[last - 1] : '\0';
    size_t unit = language && language->indent ? strlen(language->indent) : 0;
    /* An opening bracket adds one level. A closing bracket removes one level
     * from the line that it starts. */
    bool deeper = previous != '\0' && is_opening(previous) &&
                  editor_bracket_kind(previous) != EDITOR_BRACKET_ANGLE;
    bool shallower = previous != '\0' && !is_opening(previous) &&
                     editor_bracket_kind(previous) != EDITOR_BRACKET_NONE &&
                     base >= unit;
    size_t keep = shallower ? base - unit : base;
    size_t total = keep + (deeper ? unit : 0);
    if (!indent || capacity == 0)
        return total;
    size_t done = 0;
    for (size_t i = 0; i < keep && done < capacity; i++)
        indent[done++] = text[start + i];
    if (deeper) {
        for (size_t i = 0; i < unit && done < capacity; i++)
            indent[done++] = language->indent[i];
    }
    return done;
}

size_t editor_comment_start(const char *text, size_t length,
                            const struct editor_language *language, size_t offset)
{
    if (!text || offset > length)
        return 0;
    size_t start = offset;
    while (start > 0 && text[start - 1] != '\n')
        start--;
    size_t end = start;
    while (end < length && text[end] != '\n')
        end++;
    const char *token = language && language->comments.line ? language->comments.line : NULL;
    size_t token_length = token ? strlen(token) : 0;
    if (token_length == 0)
        return end - start;
    for (size_t i = start; i + token_length <= end; i++) {
        if (memcmp(text + i, token, token_length) != 0)
            continue;
        if (inside_quote(text, language, i))
            continue;
        return i - start;
    }
    return end - start;
}
