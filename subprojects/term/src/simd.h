#ifndef WSAP_VTCORE_SIMD_H
#define WSAP_VTCORE_SIMD_H

#include "compat.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/*
 * x86-64 kernels come in three levels: base (SSE2), AVX2 (x86-64-v3), and
 * AVX-512 (x86-64-v4 with VBMI and VBMI2). GCC and Clang compile every
 * level with target attributes, and wsap_vtcore_isa_init() picks one at run time.
 * Other compilers compile the levels that their flags enable.
 *
 * A kernel at a level is a function marked WSAP_VTCORE_TARGET_AVX2 or
 * WSAP_VTCORE_TARGET_AVX512. It exists when WSAP_VTCORE_KERNEL_AVX2 or WSAP_VTCORE_KERNEL_AVX512
 * is defined. Call it only when wsap_vtcore_use_avx2() or wsap_vtcore_use_avx512() is
 * true. When the compiler flags already enable a level, these checks are
 * constants and the kernels inline into their callers.
 */
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512CD__) && \
    defined(__AVX512DQ__) && defined(__AVX512VL__) && defined(__AVX512VBMI__) && \
    defined(__AVX512VBMI2__) && defined(__AVX2__) && defined(__BMI2__)
#define WSAP_VTCORE_ISA_FLOOR 2
#elif defined(__AVX2__) && defined(__BMI__) && defined(__BMI2__) && defined(__FMA__) && \
    defined(__LZCNT__)
#define WSAP_VTCORE_ISA_FLOOR 1
#else
#define WSAP_VTCORE_ISA_FLOOR 0
#endif

/* Clang 18 to 21 also need evex512 for 512-bit vectors. GCC and Clang 22
 * reject the name. Apple Clang 17 is LLVM 19. */
#if defined(__clang__) && ((defined(__apple_build_version__) && __clang_major__ >= 17) || \
    (!defined(__apple_build_version__) && __clang_major__ >= 18 && __clang_major__ <= 21))
#define WSAP_VTCORE_EVEX512 ",evex512"
#else
#define WSAP_VTCORE_EVEX512 ""
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && defined(WSAP_VTCORE_COMPILER_GNU)
#define WSAP_VTCORE_TARGET_AVX2 \
    __attribute__((target("avx2,bmi,bmi2,fma,lzcnt,popcnt,movbe,f16c")))
#define WSAP_VTCORE_TARGET_AVX512 \
    __attribute__((target("avx2,bmi,bmi2,fma,lzcnt,popcnt,movbe,f16c,avx512f,avx512bw," \
                          "avx512cd,avx512dq,avx512vl,avx512vbmi,avx512vbmi2" WSAP_VTCORE_EVEX512)))
#define WSAP_VTCORE_KERNEL_AVX2 1
#define WSAP_VTCORE_KERNEL_AVX512 1
#define WSAP_VTCORE_ISA_RUNTIME 1
#else
#define WSAP_VTCORE_TARGET_AVX2
#define WSAP_VTCORE_TARGET_AVX512
#if WSAP_VTCORE_ISA_FLOOR >= 1
#define WSAP_VTCORE_KERNEL_AVX2 1
#endif
#if WSAP_VTCORE_ISA_FLOOR >= 2
#define WSAP_VTCORE_KERNEL_AVX512 1
#endif
#endif

enum { WSAP_VTCORE_ISA_BASE, WSAP_VTCORE_ISA_AVX2, WSAP_VTCORE_ISA_AVX512 };

/* The level in use. Zero, the base level, until wsap_vtcore_isa_init() runs. It
 * is a plain int, so the compiler can hoist the checks out of loops: it is
 * written once, before the release that publishes it, and read only after
 * that. */
extern int wsap_vtcore_isa_level;

/* Pick the level once. Any thread can call it. wsap_vtcore_new() calls it before
 * the first terminal exists. */
void wsap_vtcore_isa_init(void);

static inline bool wsap_vtcore_use_avx2(void)
{
#if WSAP_VTCORE_ISA_FLOOR >= 1
    return true;
#elif defined(WSAP_VTCORE_ISA_RUNTIME)
    return wsap_vtcore_isa_level >= WSAP_VTCORE_ISA_AVX2;
#else
    return false;
#endif
}

static inline bool wsap_vtcore_use_avx512(void)
{
#if WSAP_VTCORE_ISA_FLOOR >= 2
    return true;
#elif defined(WSAP_VTCORE_ISA_RUNTIME)
    return wsap_vtcore_isa_level >= WSAP_VTCORE_ISA_AVX512;
#else
    return false;
#endif
}

#if defined(__aarch64__)
static inline uint16_t wsap_vtcore_lane_bits(uint8x16_t m)
{
    const uint8x16_t idx = {1, 2, 4, 8, 16, 32, 64, 128,
                            1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t s = vandq_u8(m, idx);
    s = vpaddq_u8(s, s);
    s = vpaddq_u8(s, s);
    s = vpaddq_u8(s, s);
    return (uint16_t)(vgetq_lane_u8(s, 0) | (vgetq_lane_u8(s, 1) << 8));
}
#endif

#endif
