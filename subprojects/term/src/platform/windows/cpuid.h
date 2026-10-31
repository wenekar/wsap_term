#ifndef WSAP_VTCORE_PLATFORM_WINDOWS_CPUID_H
#define WSAP_VTCORE_PLATFORM_WINDOWS_CPUID_H

#include <intrin.h>
#include <stdint.h>

static inline void wsap_vtcore_cpuid(int info[4], int func)
{
    __cpuid(info, func);
}

static inline void wsap_vtcore_cpuidex(int info[4], int func, int subfunc)
{
    __cpuidex(info, func, subfunc);
}

static inline uint64_t wsap_vtcore_xgetbv(uint32_t xcr)
{
    return _xgetbv(xcr);
}

#endif
