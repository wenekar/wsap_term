#ifndef UTIL_GROW_H
#define UTIL_GROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Grows *buf so that it holds at least need elements of size elem.
 * Capacity doubles. On failure *buf and *cap do not change. */
static inline bool util_grow(void *buf, int *cap, int need, size_t elem)
{
    if (need <= *cap)
        return true;
    if (need < 0)
        return false;
    int n = *cap > 0 ? *cap : 8;
    while (n < need) {
        if (n > INT32_MAX / 2)
            return false;
        n *= 2;
    }
    if ((size_t)n > SIZE_MAX / elem)
        return false;
    void *old;
    memcpy(&old, buf, sizeof(old));
    void *grown = realloc(old, (size_t)n * elem);
    if (!grown)
        return false;
    memcpy(buf, &grown, sizeof(grown));
    *cap = n;
    return true;
}

#define UTIL_GROW(arr, cap, need) \
    util_grow(&(arr), &(cap), (need), sizeof(*(arr)))

#endif
