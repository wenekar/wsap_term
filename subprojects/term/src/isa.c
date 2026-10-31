#include "wsap_vtcore.h"
#include "simd.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

int wsap_vtcore_isa_level;
static atomic_int isa_state;

static const char *const level_names[] = { "base", "avx2", "avx512" };

#if defined(WSAP_VTCORE_ISA_RUNTIME) && defined(_MSC_VER)
#include "platform/windows/cpuid.h"

static int cpu_level(void)
{
    /* Clang's CPU builtins need libgcc symbols on the MSVC target. */
    /* Read CPU features and OS vector state through native intrinsics. */
    int regs[4];
    wsap_vtcore_cpuid(regs, 0);
    if (regs[0] < 7)
        return WSAP_VTCORE_ISA_BASE;
    wsap_vtcore_cpuidex(regs, 1, 0);
    /* SSE3, SSSE3, FMA, CX16, SSE4, MOVBE, POPCNT, XSAVE, OSXSAVE, AVX, F16C. */
    const uint32_t leaf1 = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 13) |
        (1u << 19) | (1u << 20) | (1u << 22) | (1u << 23) | (1u << 26) |
        (1u << 27) | (1u << 28) | (1u << 29);
    if (((uint32_t)regs[2] & leaf1) != leaf1)
        return WSAP_VTCORE_ISA_BASE;
    const uint64_t xcr0 = wsap_vtcore_xgetbv(0);
    if ((xcr0 & 0x6u) != 0x6u)
        return WSAP_VTCORE_ISA_BASE;
    wsap_vtcore_cpuid(regs, (int)0x80000000u);
    if ((uint32_t)regs[0] < 0x80000001u)
        return WSAP_VTCORE_ISA_BASE;
    wsap_vtcore_cpuid(regs, (int)0x80000001u);
    /* LAHF and LZCNT complete the x86-64-v3 feature set. */
    if (((uint32_t)regs[2] & 0x21u) != 0x21u)
        return WSAP_VTCORE_ISA_BASE;
    wsap_vtcore_cpuidex(regs, 7, 0);
    const uint32_t leaf7 = (1u << 3) | (1u << 5) | (1u << 8); /* BMI1, AVX2, BMI2. */
    if (((uint32_t)regs[1] & leaf7) != leaf7)
        return WSAP_VTCORE_ISA_BASE;
    const uint32_t avx512 = (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31);
    const uint32_t vbmi = (1u << 1) | (1u << 6);
    if (((uint32_t)regs[1] & avx512) == avx512 &&
        ((uint32_t)regs[2] & vbmi) == vbmi && (xcr0 & 0xe6u) == 0xe6u)
        return WSAP_VTCORE_ISA_AVX512;
    return WSAP_VTCORE_ISA_AVX2;
}
#elif defined(WSAP_VTCORE_ISA_RUNTIME)
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
    const char *want = getenv("WSAP_VTCORE_ISA");
    for (int i = WSAP_VTCORE_ISA_FLOOR; want && i < level; i++)
        if (!strcmp(want, level_names[i]))
            level = i;
    return level;
}

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
