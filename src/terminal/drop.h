#ifndef TERMINAL_DROP_H
#define TERMINAL_DROP_H

#include <stddef.h>

char *terminal_quote_path(const char *path);
char *terminal_quote_uri_list(const char *data, size_t len);

#endif
