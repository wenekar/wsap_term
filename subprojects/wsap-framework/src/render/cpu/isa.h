#ifndef WSAP_RENDER_CPU_ISA_H
#define WSAP_RENDER_CPU_ISA_H

/*
 * Run-time ISA selection for the CPU pixel kernels.
 *
 * The kernels have no compile-time floor. Each SIMD variant has its own
 * target attribute. The renderer picks a level at run time and calls the
 * matching variant. SCALAR is always built, so every level stays
 * selectable in any build.
 */

#if defined(__GNUC__) || defined(__clang__)
#define RENDER_ALWAYS_INLINE __attribute__((always_inline))
#define RENDER_NOINLINE __attribute__((noinline))
#else
#define RENDER_ALWAYS_INLINE
#define RENDER_NOINLINE
#endif

#if defined(__clang__) && ((defined(__apple_build_version__) && __clang_major__ >= 17) || \
    (!defined(__apple_build_version__) && __clang_major__ >= 18 && __clang_major__ <= 21))
#define RENDER_EVEX512 ",evex512"
#else
#define RENDER_EVEX512 ""
#endif

/* The AVX-512 feature set is x86-64-v4. render_isa_best() checks the same set. */
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#define RENDER_KERNEL_X86 1
#define RENDER_TARGET_AVX2 \
    __attribute__((target("avx2,bmi,bmi2,fma,lzcnt,popcnt,movbe,f16c")))
#define RENDER_TARGET_AVX512 \
    __attribute__((target("avx2,bmi,bmi2,fma,lzcnt,popcnt,movbe,f16c,avx512f,avx512bw," \
                          "avx512cd,avx512dq,avx512vl" RENDER_EVEX512)))
#else
#define RENDER_TARGET_AVX2
#define RENDER_TARGET_AVX512
#endif

#if defined(__aarch64__)
#define RENDER_KERNEL_NEON 1
#endif

/* AUTO is the zero value: a zeroed option means "use the default". */
enum render_isa {
    RENDER_ISA_AUTO = 0,
    RENDER_ISA_SCALAR,
    RENDER_ISA_AVX2,
    RENDER_ISA_AVX512,
    RENDER_ISA_NEON,
};

/* The best level this CPU supports. Thread-safe. Cheap after the first call. */
int render_isa_best(void);

/*
 * Turn a request into a level that this CPU can run.
 * AUTO reads WSAP_RENDER_ISA (scalar, avx2, avx512, neon). If the variable
 * is not set or has an unknown value, AUTO gives render_isa_best().
 * A level above the CPU's best drops to the best supported level below it.
 * A level of another architecture, or a value outside the enum, gives
 * render_isa_best(). SCALAR is always valid.
 */
int render_isa_resolve(int requested);

/* "auto", "scalar", "avx2", "avx512", "neon", or "unknown". */
const char *render_isa_name(int isa);

#endif
