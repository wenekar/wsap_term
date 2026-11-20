#ifndef WSAP_RENDER_ISA_H
#define WSAP_RENDER_ISA_H

/* CPU instruction set for the renderer. A zero option selects automatically. */
enum render_isa {
    RENDER_ISA_AUTO = 0,
    RENDER_ISA_SCALAR,
    RENDER_ISA_AVX2,
    RENDER_ISA_AVX512,
    RENDER_ISA_NEON,
};

/* Returns "auto", "scalar", "avx2", "avx512", "neon", or "unknown". */
const char *render_isa_name(int isa);

#endif
