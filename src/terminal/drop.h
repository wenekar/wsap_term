#ifndef TERMINAL_DROP_H
#define TERMINAL_DROP_H

#include <stddef.h>

/* Return owned shell text, or NULL for an invalid path or allocation failure. */
char *terminal_quote_path(const char *path);
/* Decode local file URIs and quote each path. Return owned shell text. */
char *terminal_quote_uri_list(const char *data, size_t len);

#endif
