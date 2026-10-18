#include "render/cpu/isa.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const char *const isa_names[] = {
    [RENDER_ISA_AUTO] = "auto",
    [RENDER_ISA_SCALAR] = "scalar",
    [RENDER_ISA_AVX2] = "avx2",
    [RENDER_ISA_AVX512] = "avx512",
    [RENDER_ISA_NEON] = "neon",
};

static pthread_once_t best_once = PTHREAD_ONCE_INIT;
static int best_level;

static void detect_best(void)
{
#if defined(RENDER_KERNEL_X86)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("x86-64-v4"))
        best_level = RENDER_ISA_AVX512;
    else if (__builtin_cpu_supports("x86-64-v3"))
        best_level = RENDER_ISA_AVX2;
    else
        best_level = RENDER_ISA_SCALAR;
#elif defined(RENDER_KERNEL_NEON)
    best_level = RENDER_ISA_NEON;
#else
    best_level = RENDER_ISA_SCALAR;
#endif
}

int render_isa_best(void)
{
    pthread_once(&best_once, detect_best);
    return best_level;
}

/* The order of the levels that this architecture has, or -1. */
static int isa_rank(int isa)
{
    switch (isa) {
    case RENDER_ISA_SCALAR:
        return 0;
#if defined(RENDER_KERNEL_X86)
    case RENDER_ISA_AVX2:
        return 1;
    case RENDER_ISA_AVX512:
        return 2;
#elif defined(RENDER_KERNEL_NEON)
    case RENDER_ISA_NEON:
        return 1;
#endif
    default:
        return -1;
    }
}

static int env_level(void)
{
    const char *want = getenv("WSAP_RENDER_ISA");
    if (!want)
        return RENDER_ISA_AUTO;
    for (int i = RENDER_ISA_SCALAR; i <= RENDER_ISA_NEON; i++)
        if (!strcmp(want, isa_names[i]))
            return i;
    return RENDER_ISA_AUTO;
}

int render_isa_resolve(int requested)
{
    int best = render_isa_best();
    if (requested == RENDER_ISA_AUTO)
        requested = env_level();
    int rank = isa_rank(requested);
    if (rank < 0 || rank > isa_rank(best))
        return best;
    return requested;
}

const char *render_isa_name(int isa)
{
    if (isa < RENDER_ISA_AUTO || isa > RENDER_ISA_NEON)
        return "unknown";
    return isa_names[isa];
}
