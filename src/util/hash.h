#ifndef UTIL_HASH_H
#define UTIL_HASH_H

#include <stdint.h>

#define HASH64_BASIS UINT64_C(0x9E3779B97F4A7C15)
#define KNUTH_HASH_MULT UINT32_C(2654435761)

static inline uint64_t hash64_step(uint64_t hash, uint32_t value)
{
    hash ^= value;
    hash *= HASH64_BASIS;
    return hash ^ (hash >> 29);
}

static inline uint64_t hash64_finish(uint64_t hash)
{
    hash ^= hash >> 32;
    hash *= UINT64_C(0xD6E8FEB86659FD93);
    hash ^= hash >> 32;
    return hash;
}

#endif
