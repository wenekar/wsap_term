#ifndef WSAP_VTCORE_PLATFORM_WINDOWS_BITOPS_H
#define WSAP_VTCORE_PLATFORM_WINDOWS_BITOPS_H

#include <intrin.h>
#include <stdint.h>

static inline int wsap_vtcore_ctz32(uint32_t x)
{
    unsigned long i;
    return _BitScanForward(&i, x) ? (int)i : 32;
}

static inline int wsap_vtcore_clz32(uint32_t x)
{
    unsigned long i;
    return _BitScanReverse(&i, x) ? 31 - (int)i : 32;
}

static inline int wsap_vtcore_popcount32(uint32_t x)
{
    return (int)__popcnt(x);
}

#endif
