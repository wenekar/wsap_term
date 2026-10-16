/*
 * The instruction set level. See simd.h. The level is picked once, before
 * the first terminal exists, so every call in a process uses the same
 * kernels.
 */
#include "wsap_vtcore.h"
#include "simd.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

int wsap_vtcore_isa_level;
static atomic_int isa_state;

static const char *const level_names[] = { "base", "avx2", "avx512" };

#if defined(WSAP_VTCORE_ISA_RUNTIME)
/* The AVX-512 kernels also need VBMI and VBMI2. Every CPU with VBMI except
 * Cannon Lake has VBMI2. */
static int cpu_level(void)
{
    __builtin_cpu_init();
    if (__builtin_cpu_supports("x86-64-v4") && __builtin_cpu_supports("avx512vbmi") &&
        __builtin_cpu_supports("avx512vbmi2"))
        return WSAP_VTCORE_ISA_AVX512;
    if (__builtin_cpu_supports("x86-64-v3"))
        return WSAP_VTCORE_ISA_AVX2;
    return WSAP_VTCORE_ISA_BASE;
}
#else
static int cpu_level(void)
{
    return WSAP_VTCORE_ISA_FLOOR;
}
#endif

static int pick_level(void)
{
    int level = cpu_level();
    /* WSAP_VTCORE_ISA can only lower the level, never below the floor the
     * compiler flags fix. */
    const char *want = getenv("WSAP_VTCORE_ISA");
    for (int i = WSAP_VTCORE_ISA_FLOOR; want && i < level; i++)
        if (!strcmp(want, level_names[i]))
            level = i;
    return level;
}

/* The first caller picks the level. A second caller waits until it is set. */
void wsap_vtcore_isa_init(void)
{
    if (atomic_load_explicit(&isa_state, memory_order_acquire) == 2)
        return;
    int expected = 0;
    if (atomic_compare_exchange_strong(&isa_state, &expected, 1)) {
        wsap_vtcore_isa_level = pick_level();
        atomic_store_explicit(&isa_state, 2, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&isa_state, memory_order_acquire) != 2)
        wsap_vtcore_cpu_relax();
}

const char *wsap_vtcore_isa(void)
{
    wsap_vtcore_isa_init();
#if defined(__aarch64__)
    return "neon";
#else
    int level = WSAP_VTCORE_ISA_FLOOR;
    if (wsap_vtcore_use_avx2())
        level = WSAP_VTCORE_ISA_AVX2;
    if (wsap_vtcore_use_avx512())
        level = WSAP_VTCORE_ISA_AVX512;
    return level_names[level];
#endif
}
