#ifndef YASL_PRIVATE_COMMON_UTF8_H
#define YASL_PRIVATE_COMMON_UTF8_H

#include <stdbool.h>
#include <stddef.h>

bool yasl_private_utf8_validate(const char *str, size_t len);
char *yasl_private_utf8_sanitize_dup(const char *str);
/* Owned conversions for X11 STRING interoperability. to_latin1 rejects text
 * outside Latin-1; byte lengths exclude the allocated NUL terminator. */
char *yasl_private_utf8_from_latin1(const char *str, size_t len, size_t *out_len);
char *yasl_private_utf8_to_latin1(const char *str, size_t *out_len);

#endif
