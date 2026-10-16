#define _POSIX_C_SOURCE 200809L
#include "common/types.h"

#include <string.h>
#include <strings.h>
#include <unistd.h>

/* decode_path() result for a line that is not a local file URI. */
#define NOT_LOCAL SIZE_MAX

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool local_host(const char *host, size_t len)
{
    if (len == 9 && strncasecmp(host, "localhost", 9) == 0) return true;
    char name[256];
    if (gethostname(name, sizeof(name)) != 0) return false;
    name[sizeof(name) - 1] = '\0';
    return strlen(name) == len && strncasecmp(host, name, len) == 0;
}

/* Decode the path of one uri-list line. Writes to out only when out is not
 * NULL; the caller checks the length first. */
static size_t decode_path(const char *line, size_t len, char *out)
{
    const char *p = line, *end = line + len;
    if (len < 5 || strncasecmp(p, "file:", 5) != 0) return NOT_LOCAL;
    p += 5;
    if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
        const char *host = p + 2;
        p = host;
        while (p < end && *p != '/') p++;
        if (p > host && !local_host(host, (size_t)(p - host))) return NOT_LOCAL;
    }
    if (p == end || *p != '/') return NOT_LOCAL;
    size_t n = 0;
    while (p < end && *p != '?' && *p != '#') {
        char c = *p++;
        if (c == '%') {
            if (end - p < 2) return NOT_LOCAL;
            int hi = hex_digit(p[0]), lo = hex_digit(p[1]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return NOT_LOCAL;
            c = (char)(hi << 4 | lo);
            p += 2;
        }
        if (out) out[n] = c;
        n++;
    }
    return n;
}

size_t yasl_private_drop_next_path(const char **cursor, char *out, size_t cap)
{
    if (!cursor || !*cursor || (!out && cap)) return 0;
    const char *p = *cursor;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        size_t len = (size_t)(p - line);
        while (*p == '\r' || *p == '\n') p++;
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
        if (!len || line[0] == '#') continue;
        size_t n = decode_path(line, len, NULL);
        if (n == NOT_LOCAL) continue;
        if (n >= cap) return n;
        decode_path(line, len, out);
        out[n] = '\0';
        *cursor = p;
        return n;
    }
    *cursor = p;
    return 0;
}
