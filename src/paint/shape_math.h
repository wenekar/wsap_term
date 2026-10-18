#ifndef WSAP_PAINT_SHAPE_MATH_H
#define WSAP_PAINT_SHAPE_MATH_H

/*
 * Shape coverage math. It is shared by the CPU renderer and the GPU one.
 *
 * This header is a subset of C that is also valid Metal Shading Language.
 * Keep it that way:
 * - The functions take and return only float and int scalars.
 * - There are no pointers, no structs, and no libc calls. Math goes
 *   through the SHAPE_* macros. A GPU build defines them before it
 *   includes this file. C maps them to the libm float functions.
 * - Every function is SHAPE_FN.
 * - Build every C file that includes this header with -ffp-contract=off.
 *   Then a fused multiply-add cannot change a result.
 *
 * Pixel coordinates are pixel indexes. A coverage is an int from 0 to 255.
 */

#ifndef SHAPE_FN
#define SHAPE_FN static inline
#endif

#ifndef SHAPE_SQRT
#include <math.h>
#define SHAPE_SQRT(x) sqrtf(x)
#define SHAPE_EXP(x) expf(x)
#endif

/* A float from 0 to 1 as an int from 0 to 255. A NaN gives 0. */
SHAPE_FN int shape_quantize(float v)
{
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return 255;
    return (int)(v * 255.0f + 0.5f);
}

/*
 * Coverage of the edge ring of a disc with radius r. (dx, dy) is the
 * offset of the pixel from the disc center. The coverage is r + 0.5 - d,
 * clamped to 0..255.
 */
SHAPE_FN int shape_corner_cov(float dx, float dy, float r)
{
    float d2 = dx * dx + dy * dy;
    float in = r - 1.0f;
    float out = r + 1.0f;
    if (in > 0.0f && d2 <= in * in)
        return 255;
    if (d2 >= out * out)
        return 0;
    return shape_quantize(r + 0.5f - SHAPE_SQRT(d2));
}

/*
 * Coverage of a rounded rect. The rect starts at pixel (x0, y0) and is
 * w by h pixels. The radius r is already clamped to w / 2 and h / 2 with
 * integer division. A pixel in the straight band of a row or column is
 * full. The band is the span between the two corner centers, also when
 * they cross. Other pixels use the nearest corner center.
 */
SHAPE_FN int shape_rrect_cov(float x, float y, float x0, float y0,
                             float w, float h, float r)
{
    float ctl_x = x0 + r, ctl_y = y0 + r;
    float ctr_x = x0 + w - r - 1.0f, cbl_y = y0 + h - r - 1.0f;
    float lo_x = ctl_x <= ctr_x ? ctl_x : ctr_x;
    float hi_x = ctl_x <= ctr_x ? ctr_x : ctl_x;
    float lo_y = ctl_y <= cbl_y ? ctl_y : cbl_y;
    float hi_y = ctl_y <= cbl_y ? cbl_y : ctl_y;
    /* When w or h is 2r, the centers cross. The span between them is
     * still a straight band. */
    if ((y >= lo_y && y <= hi_y) || (x >= lo_x && x <= hi_x))
        return 255;
    float cx = x < lo_x ? lo_x : hi_x;
    float cy = y < lo_y ? lo_y : hi_y;
    return shape_corner_cov(x - cx, y - cy, r);
}

/*
 * Coverage of a circle with center (cx, cy) in pixel index space. A
 * circle in a box of 2r by 2r pixels at (bx, by) has its center at
 * (bx + r - 0.5, by + r - 0.5). Then it is symmetric.
 */
SHAPE_FN int shape_circle_cov(float x, float y, float cx, float cy, float r)
{
    return shape_corner_cov(x - cx, y - cy, r);
}

/*
 * The standard normal CDF. It uses the Abramowitz and Stegun 7.1.26
 * form of erfc. The error is below 1.5e-7, far below one step of 8 bits.
 */
SHAPE_FN float shape_phi(float z)
{
    float x = (z < 0.0f ? -z : z) * 0.70710678f;
    float t = 1.0f / (1.0f + 0.3275911f * x);
    float poly = ((((1.061405429f * t - 1.453152027f) * t + 1.421413741f) * t
                   - 0.284496736f) * t + 0.254829592f) * t;
    float half_erfc = 0.5f * poly * SHAPE_EXP(-x * x);
    return z < 0.0f ? half_erfc : 1.0f - half_erfc;
}

/* The shadow of a straight edge. p is the pixel offset from the edge. */
SHAPE_FN int shape_shadow_edge(int p, float sigma)
{
    return shape_quantize(shape_phi(((float)p + 0.5f) / sigma));
}

/*
 * The shadow of a rounded corner is a sum of n slices of the quarter
 * disc. These helpers give the terms of the sum. A caller adds
 * row_term(i, k) * col_term(j, k) for k = 0..n-1 in order, then calls
 * shape_shadow_tile. p is the pixel offset (index - extent) plus 0.5.
 */
SHAPE_FN float shape_shadow_slice(float r, int k, int n)
{
    return ((float)k + 0.5f) * (r / (float)n);
}

SHAPE_FN float shape_shadow_row_term(float p, float v, float r, int n, float sigma)
{
    float t = (p - v) / sigma;
    float norm = 1.0f / (sigma * SHAPE_SQRT(6.28318530717958647692f));
    return norm * SHAPE_EXP(-0.5f * t * t) * (r / (float)n);
}

SHAPE_FN float shape_shadow_col_term(float p, float v, float r, float sigma)
{
    float l = r - SHAPE_SQRT(v * (2.0f * r - v));
    return shape_phi((p - l) / sigma);
}

/* The coverage of one tile cell. sum is the slice sum; x and y are the offsets. */
SHAPE_FN int shape_shadow_tile(float sum, float x, float y, float r, float sigma)
{
    float tail = shape_phi((y - r) / sigma);
    return shape_quantize(sum + shape_phi(x / sigma) * tail);
}

#endif
