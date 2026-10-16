#include "util/text_limit.h"
#include "terminal/drop.h"

#include <stdlib.h>
#include <string.h>

char *terminal_quote_path(const char *path)
{
    if (!path || !*path) return NULL;
    size_t len = strlen(path), quotes = 0;
    if (len > INPUT_TEXT_MAX / 4) return NULL;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)path[i] < 0x20 || path[i] == 0x7f) return NULL;
        if (path[i] == '\'') quotes++;
    }
    if (len + quotes * 3 + 3 > INPUT_TEXT_MAX) return NULL;
    char *out = malloc(len + quotes * 3 + 3);
    if (!out) return NULL;
    size_t n = 0;
    out[n++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = path[i];
        }
    }
    out[n++] = '\'';
    out[n] = 0;
    return out;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *terminal_quote_uri_list(const char *data, size_t len)
{
    if (!data || len > INPUT_TEXT_MAX) return NULL;
    char *out = calloc(len * 4 + 1, 1);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t start = 0; start < len;) {
        size_t end = start;
        while (end < len && data[end] != '\r' && data[end] != '\n') end++;
        size_t path = start + 7;
        if (end >= path && !memcmp(data + start, "file://", 7)) {
            if (end >= path + 9 && !memcmp(data + path, "localhost", 9)) path += 9;
            if (path < end && data[path] == '/') {
                char *decoded = malloc(end - path + 1);
                if (!decoded) break;
                size_t n = 0;
                for (size_t i = path; i < end; i++) {
                    unsigned char c = (unsigned char)data[i];
                    if (c == '%' && i + 2 < end) {
                        int hi = hex(data[i + 1]), lo = hex(data[i + 2]);
                        if (hi >= 0 && lo >= 0) { c = (unsigned char)(hi * 16 + lo); i += 2; }
                    }
                    decoded[n++] = (char)c;
                }
                decoded[n] = 0;
                if (strlen(decoded) == n) {
                    char *quoted = terminal_quote_path(decoded);
                    if (quoted) {
                        size_t qlen = strlen(quoted);
                        if (used) out[used++] = ' ';
                        memcpy(out + used, quoted, qlen);
                        used += qlen;
                        free(quoted);
                    }
                }
                free(decoded);
            }
        }
        start = end;
        while (start < len && (data[start] == '\r' || data[start] == '\n')) start++;
    }
    if (!used) { free(out); return NULL; }
    return out;
}
