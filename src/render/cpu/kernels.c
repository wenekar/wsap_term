#include "render/cpu/kernels.h"

/*
 * One out-of-line wrapper per kernel per level. Each wrapper inlines the
 * dispatcher with a constant level, so only that level's variant remains.
 */
#define RENDER_KERNEL_WRAPPERS(sfx, level, target)                                        \
    target static RENDER_NOINLINE void fill_##sfx(uint32_t *dst, int n, uint32_t color)  \
    {                                                                                     \
        render_kernel_fill(level, dst, n, color);                                         \
    }                                                                                     \
    target static RENDER_NOINLINE void blit_tile_##sfx(uint32_t *dst, size_t stride,      \
                                                       const uint32_t *tile, int cw,      \
                                                       int ch)                            \
    {                                                                                     \
        render_kernel_blit_tile(level, dst, stride, tile, cw, ch);                        \
    }                                                                                     \
    target static RENDER_NOINLINE void compose_alpha_lut_##sfx(                           \
        uint32_t *dst, const uint8_t *cov, int n, const uint32_t *lut256)                 \
    {                                                                                     \
        render_kernel_compose_alpha_lut(level, dst, cov, n, lut256);                      \
    }                                                                                     \
    target static RENDER_NOINLINE void compose_subpx_lut_##sfx(                           \
        uint32_t *dst, const uint8_t *cov, int n, const struct subpx_lut *lut,            \
        uint32_t bg)                                                                      \
    {                                                                                     \
        render_kernel_compose_subpx_lut(level, dst, cov, n, lut, bg);                     \
    }                                                                                     \
    target static RENDER_NOINLINE void compose_color_##sfx(                               \
        uint32_t *dst, const uint32_t *premul, int n, bool faint)                         \
    {                                                                                     \
        render_kernel_compose_color(level, dst, premul, n, faint);                        \
    }                                                                                     \
    target static RENDER_NOINLINE void blend_any_##sfx(uint32_t *dst, const uint8_t *cov, \
                                                       int n, uint32_t fg)                \
    {                                                                                     \
        render_kernel_blend_any(level, dst, cov, n, fg);                                  \
    }                                                                                     \
    target static RENDER_NOINLINE void blend_any_subpx_##sfx(                             \
        uint32_t *dst, const uint8_t *cov, int n, uint32_t fg)                            \
    {                                                                                     \
        render_kernel_blend_any_subpx(level, dst, cov, n, fg);                            \
    }                                                                                     \
    target static RENDER_NOINLINE void blend_rect_##sfx(uint32_t *dst, int n,             \
                                                        uint32_t color, uint8_t a)        \
    {                                                                                     \
        render_kernel_blend_rect(level, dst, n, color, a);                                \
    }                                                                                     \
    static const struct render_kernels kernels_##sfx = {                                  \
        .isa = level,                                                                     \
        .fill = fill_##sfx,                                                               \
        .blit_tile = blit_tile_##sfx,                                                     \
        .compose_alpha_lut = compose_alpha_lut_##sfx,                                     \
        .compose_subpx_lut = compose_subpx_lut_##sfx,                                     \
        .compose_color = compose_color_##sfx,                                             \
        .blend_any = blend_any_##sfx,                                                     \
        .blend_any_subpx = blend_any_subpx_##sfx,                                         \
        .blend_rect = blend_rect_##sfx,                                                   \
    };

#define RENDER_NO_TARGET

RENDER_KERNEL_WRAPPERS(scalar, RENDER_ISA_SCALAR, RENDER_NO_TARGET)
#if defined(RENDER_KERNEL_X86)
RENDER_KERNEL_WRAPPERS(avx2, RENDER_ISA_AVX2, RENDER_TARGET_AVX2)
RENDER_KERNEL_WRAPPERS(avx512, RENDER_ISA_AVX512, RENDER_TARGET_AVX512)
#endif
#if defined(RENDER_KERNEL_NEON)
RENDER_KERNEL_WRAPPERS(neon, RENDER_ISA_NEON, RENDER_NO_TARGET)
#endif

const struct render_kernels *render_kernels_for(int isa)
{
    switch (isa) {
#if defined(RENDER_KERNEL_X86)
    case RENDER_ISA_AVX512:
        return &kernels_avx512;
    case RENDER_ISA_AVX2:
        return &kernels_avx2;
#endif
#if defined(RENDER_KERNEL_NEON)
    case RENDER_ISA_NEON:
        return &kernels_neon;
#endif
    default:
        return &kernels_scalar;
    }
}
