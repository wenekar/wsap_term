#ifndef UTIL_STR_H
#define UTIL_STR_H

#include <stdbool.h>
#include <string.h>
#include <strings.h>

static inline char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = 0;
    return s;
}

static inline bool str_url_scheme_allowed(const char *url)
{
    static const char *const schemes[] = {
        "http://", "https://", "ftp://", "mailto:"
    };
    if (!url || url[0] == '-')
        return false;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        size_t n = strlen(schemes[i]);
        if (strncasecmp(url, schemes[i], n) == 0)
            return true;
    }
    return false;
}

#endif
