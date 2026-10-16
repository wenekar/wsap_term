
#include "font/cell_draw.h"
#include "font/internal.h"
#include "font/font5x7.h"

#include <math.h>
#include <string.h>

static void glyph_paint_rect(struct glyph *g, int y0, int y1, int x0, int x1, uint8_t a)
{
    if (!g->alpha && !glyph_prepare(g, g->width ? g->width : GLYPH_CELL_W))
        return;
    if (x1 > (int)g->width) x1 = g->width;
    if (y1 > glyph_rows(g)) y1 = glyph_rows(g);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (int y = y0; y < y1; y++) {
        uint64_t *m = glyph_mask(g, y);
        for (int w = x0 >> 6; w <= (x1 - 1) >> 6; w++) {
            int lo = w * 64 > x0 ? 0 : x0 - w * 64;
            int hi = (w + 1) * 64 < x1 ? 64 : x1 - w * 64;
            m[w] |= (hi - lo == 64 ? ~UINT64_C(0)
                                   : ((UINT64_C(1) << (hi - lo)) - 1)) << lo;
        }
        memset(&g->alpha[y][x0], a, (size_t)(x1 - x0));
    }
}

static int cut(int extent, int k, int n)
{
    return (int)(((long)extent * k + n / 2) / n);
}

static int thin_stroke(void)
{
    int t = GLYPH_CELL_H / 16;
    return t < 1 ? 1 : t;
}

static double arm_centre(int extent, int thin)
{
    return (extent - thin) / 2 + thin / 2.0;
}

static bool on_stroke(double d, int thin)
{
    return 2 * d <= thin;
}

static void fill_mosaic(struct glyph *g, unsigned mask, int cols, int rows, int inset)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            if (!(mask & 1u << (r * cols + c)))
                continue;
            glyph_paint_rect(g, cut(GLYPH_CELL_H, r, rows) + inset,
                             cut(GLYPH_CELL_H, r + 1, rows) - inset,
                             cut(GLYPH_CELL_W, c, cols) + inset,
                             cut(GLYPH_CELL_W, c + 1, cols) - inset, 255);
        }
}

static int mosaic_inset(int cols, int rows)
{
    return GLYPH_CELL_W / cols >= 3 && GLYPH_CELL_H / rows >= 3 ? 1 : 0;
}

static unsigned nth_mosaic_mask(unsigned index, const uint8_t *absent, int n_absent)
{
    for (unsigned mask = 0, seen = 0; mask < 256; mask++) {
        bool skip = false;
        for (int i = 0; i < n_absent; i++)
            if (absent[i] == mask)
                skip = true;
        if (skip)
            continue;
        if (seen++ == index)
            return mask;
    }
    return 0;
}

static const uint8_t sextants_absent[] = { 0, 21, 42, 63 };

static const uint8_t octants_absent[] = {
    0, 1, 2, 3, 5, 10, 15, 20, 40, 63, 64, 80, 85, 90,
    95, 128, 160, 165, 170, 175, 192, 240, 245, 250, 252, 255,
};

enum { QUAD_UL = 1, QUAD_UR = 2, QUAD_LL = 4, QUAD_LR = 8 };

enum { EDGE_TOP, EDGE_RIGHT, EDGE_BOTTOM, EDGE_LEFT };

static void fill_edge_triangle(struct glyph *g, int edge, uint8_t a)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;

    if (edge == EDGE_LEFT || edge == EDGE_RIGHT) {
        for (int y = 0; y < H; y++) {
            int off = 2 * y + 1 - H;
            if (off < 0) off = -off;
            int w = (W * (H - off) + 2 * H - 1) / (2 * H);
            if (edge == EDGE_LEFT) glyph_paint_rect(g, y, y + 1, 0, w, a);
            else                   glyph_paint_rect(g, y, y + 1, W - w, W, a);
        }
    } else {
        for (int x = 0; x < W; x++) {
            int off = 2 * x + 1 - W;
            if (off < 0) off = -off;
            int h = (H * (W - off) + 2 * W - 1) / (2 * W);
            if (edge == EDGE_TOP) glyph_paint_rect(g, 0, h, x, x + 1, a);
            else                  glyph_paint_rect(g, H - h, H, x, x + 1, a);
        }
    }
}

static void fill_corner_triangle(struct glyph *g, int corner, uint8_t a)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;

    for (int y = 0; y < H; y++) {
        bool upper = corner == QUAD_UL || corner == QUAD_UR;
        int w = (W * (upper ? H - y : y + 1) + H - 1) / H;
        if (corner == QUAD_UL || corner == QUAD_LL) glyph_paint_rect(g, y, y + 1, 0, w, a);
        else                                        glyph_paint_rect(g, y, y + 1, W - w, W, a);
    }
}

static void fill_hatch(struct glyph *g, bool falling)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;
    int period = W / 2 < 2 ? 2 : W / 2;

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int along = falling ? x - y * W / H : x + y * W / H;
            int band = (along % (2 * period) + 2 * period) % (2 * period);
            if (band < period)
                glyph_paint_rect(g, y, y + 1, x, x + 1, 255);
        }
}

static bool draw_block(uint32_t cp, struct glyph *g)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;

    switch (cp) {
    case 0x2580: glyph_paint_rect(g, 0, cut(H, 1, 2), 0, W, 255); return true;
    case 0x2588: glyph_paint_rect(g, 0, H, 0, W, 255); return true;
    case 0x2590: glyph_paint_rect(g, 0, H, cut(W, 1, 2), W, 255); return true;
    case 0x2594: glyph_paint_rect(g, 0, cut(H, 1, 8), 0, W, 255); return true;
    case 0x2595: glyph_paint_rect(g, 0, H, cut(W, 7, 8), W, 255); return true;
    }

    if (cp >= 0x2581 && cp <= 0x2587) {
        glyph_paint_rect(g, cut(H, 8 - (int)(cp - 0x2580), 8), H, 0, W, 255);
        return true;
    }

    if (cp >= 0x2589 && cp <= 0x258F) {
        glyph_paint_rect(g, 0, H, 0, cut(W, 8 - (int)(cp - 0x2588), 8), 255);
        return true;
    }

    if (cp >= 0x2591 && cp <= 0x2593) {
        glyph_paint_rect(g, 0, H, 0, W, (uint8_t)((cp - 0x2590) * 64));
        return true;
    }
    if (cp >= 0x2596 && cp <= 0x259F) {
        static const uint8_t quadrants[] = {
            QUAD_LL, QUAD_LR, QUAD_UL,
            QUAD_UL | QUAD_LL | QUAD_LR, QUAD_UL | QUAD_LR,
            QUAD_UL | QUAD_UR | QUAD_LL, QUAD_UL | QUAD_UR | QUAD_LR,
            QUAD_UR, QUAD_UR | QUAD_LL, QUAD_UR | QUAD_LL | QUAD_LR,
        };
        fill_mosaic(g, quadrants[cp - 0x2596], 2, 2, 0);
        return true;
    }
    return false;
}

static void eighth_col(struct glyph *g, int k)
{
    glyph_paint_rect(g, 0, GLYPH_CELL_H,
                     cut(GLYPH_CELL_W, k - 1, 8), cut(GLYPH_CELL_W, k, 8), 255);
}

static void eighth_row(struct glyph *g, int k)
{
    glyph_paint_rect(g, cut(GLYPH_CELL_H, k - 1, 8), cut(GLYPH_CELL_H, k, 8),
                     0, GLYPH_CELL_W, 255);
}

static bool draw_legacy_computing_symbol(uint32_t cp, struct glyph *g)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;

    static const int part_eighths[] = { 2, 3, 5, 6, 7 };

    if (cp >= 0x1FB00 && cp <= 0x1FB3B) {
        fill_mosaic(g, nth_mosaic_mask(cp - 0x1FB00, sextants_absent,
                                  (int)sizeof(sextants_absent)), 2, 3, 0);
        return true;
    }

    if (cp >= 0x1FB68 && cp <= 0x1FB6B) {
        static const int omitted[] = { EDGE_LEFT, EDGE_TOP, EDGE_RIGHT, EDGE_BOTTOM };
        for (int edge = EDGE_TOP; edge <= EDGE_LEFT; edge++)
            if (edge != omitted[cp - 0x1FB68])
                fill_edge_triangle(g, edge, 255);
        return true;
    }

    if (cp >= 0x1FB6C && cp <= 0x1FB6F) {
        static const int edges[] = { EDGE_LEFT, EDGE_TOP, EDGE_RIGHT, EDGE_BOTTOM };
        fill_edge_triangle(g, edges[cp - 0x1FB6C], 255);
        return true;
    }

    if (cp >= 0x1FB70 && cp <= 0x1FB75) {
        eighth_col(g, (int)(cp - 0x1FB70) + 2);
        return true;
    }

    if (cp >= 0x1FB76 && cp <= 0x1FB7B) {
        eighth_row(g, (int)(cp - 0x1FB76) + 2);
        return true;
    }

    if (cp >= 0x1FB82 && cp <= 0x1FB86) {
        glyph_paint_rect(g, 0, cut(H, part_eighths[cp - 0x1FB82], 8), 0, W, 255);
        return true;
    }
    if (cp >= 0x1FB87 && cp <= 0x1FB8B) {
        glyph_paint_rect(g, 0, H, cut(W, 8 - part_eighths[cp - 0x1FB87], 8), W, 255);
        return true;
    }

    if (cp >= 0x1FB9C && cp <= 0x1FB9F) {
        static const int corners[] = { QUAD_UL, QUAD_UR, QUAD_LR, QUAD_LL };
        fill_corner_triangle(g, corners[cp - 0x1FB9C], 128);
        return true;
    }

    switch (cp) {
    case 0x1FB7C: eighth_col(g, 1); eighth_row(g, 8); return true;
    case 0x1FB7D: eighth_col(g, 1); eighth_row(g, 1); return true;
    case 0x1FB7E: eighth_col(g, 8); eighth_row(g, 1); return true;
    case 0x1FB7F: eighth_col(g, 8); eighth_row(g, 8); return true;
    case 0x1FB80: eighth_row(g, 1); eighth_row(g, 8); return true;
    case 0x1FB81:
        eighth_row(g, 1); eighth_row(g, 3);
        eighth_row(g, 5); eighth_row(g, 8);
        return true;
    case 0x1FB8C: glyph_paint_rect(g, 0, H, 0, cut(W, 1, 2), 128); return true;
    case 0x1FB8D: glyph_paint_rect(g, 0, H, cut(W, 1, 2), W, 128); return true;
    case 0x1FB8E: glyph_paint_rect(g, 0, cut(H, 1, 2), 0, W, 128); return true;
    case 0x1FB8F: glyph_paint_rect(g, cut(H, 1, 2), H, 0, W, 128); return true;

    case 0x1FB90: glyph_paint_rect(g, 0, H, 0, W, 128); return true;
    case 0x1FB91:
        glyph_paint_rect(g, 0, cut(H, 1, 2), 0, W, 255);
        glyph_paint_rect(g, cut(H, 1, 2), H, 0, W, 128);
        return true;
    case 0x1FB92:
        glyph_paint_rect(g, 0, cut(H, 1, 2), 0, W, 128);
        glyph_paint_rect(g, cut(H, 1, 2), H, 0, W, 255);
        return true;
    case 0x1FB94:
        glyph_paint_rect(g, 0, H, 0, cut(W, 1, 2), 128);
        glyph_paint_rect(g, 0, H, cut(W, 1, 2), W, 255);
        return true;
    case 0x1FB95: fill_mosaic(g, 0x9999, 4, 4, 0); return true;
    case 0x1FB96: fill_mosaic(g, 0x6666, 4, 4, 0); return true;
    case 0x1FB97:
        glyph_paint_rect(g, cut(H, 1, 4), cut(H, 2, 4), 0, W, 255);
        glyph_paint_rect(g, cut(H, 3, 4), H, 0, W, 255);
        return true;
    case 0x1FB98: fill_hatch(g, true); return true;
    case 0x1FB99: fill_hatch(g, false); return true;
    case 0x1FB9A:
        fill_edge_triangle(g, EDGE_TOP, 255);
        fill_edge_triangle(g, EDGE_BOTTOM, 255);
        return true;
    case 0x1FB9B:
        fill_edge_triangle(g, EDGE_LEFT, 255);
        fill_edge_triangle(g, EDGE_RIGHT, 255);
        return true;
    }
    return false;
}

static bool draw_legacy_supplement_symbol(uint32_t cp, struct glyph *g)
{

    if (cp >= 0x1CC21 && cp <= 0x1CC2F) {
        fill_mosaic(g, cp - 0x1CC20, 2, 2, mosaic_inset(2, 2));
        return true;
    }
    if (cp >= 0x1CE51 && cp <= 0x1CE8F) {
        fill_mosaic(g, cp - 0x1CE50, 2, 3, mosaic_inset(2, 3));
        return true;
    }

    if (cp >= 0x1CD00 && cp <= 0x1CDE5) {
        fill_mosaic(g, nth_mosaic_mask(cp - 0x1CD00, octants_absent,
                                  (int)sizeof(octants_absent)), 2, 4, 0);
        return true;
    }
    return false;
}

enum { ARM_NONE = 0, ARM_LIGHT = 1, ARM_HEAVY = 2, ARM_DOUBLE = 3 };
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

static const uint8_t box_weights[0x80] = {
    0x50, 0xA0, 0x05, 0x0A, 0x50, 0xA0, 0x05, 0x0A,
    0x50, 0xA0, 0x05, 0x0A, 0x44, 0x84, 0x48, 0x88,
    0x14, 0x24, 0x18, 0x28, 0x41, 0x81, 0x42, 0x82,
    0x11, 0x21, 0x12, 0x22, 0x45, 0x85, 0x46, 0x49,
    0x4A, 0x86, 0x89, 0x8A, 0x15, 0x25, 0x16, 0x19,
    0x1A, 0x26, 0x29, 0x2A, 0x54, 0x64, 0x94, 0xA4,
    0x58, 0x68, 0x98, 0xA8, 0x51, 0x61, 0x91, 0xA1,
    0x52, 0x62, 0x92, 0xA2, 0x55, 0x65, 0x95, 0xA5,
    0x56, 0x59, 0x5A, 0x66, 0x96, 0x69, 0x99, 0xA6,
    0xA9, 0x6A, 0x9A, 0xAA, 0x50, 0xA0, 0x05, 0x0A,
    0xF0, 0x0F, 0xC4, 0x4C, 0xCC, 0x34, 0x1C, 0x3C,
    0xC1, 0x43, 0xC3, 0x31, 0x13, 0x33, 0xC5, 0x4F,
    0xCF, 0x35, 0x1F, 0x3F, 0xF4, 0x5C, 0xFC, 0xF1,
    0x53, 0xF3, 0xF5, 0x5F, 0xFF, 0x44, 0x14, 0x11,
    0x41, 0x00, 0x00, 0x00, 0x10, 0x01, 0x40, 0x04,
    0x20, 0x02, 0x80, 0x08, 0x90, 0x09, 0x60, 0x06,
};

static void draw_dashes(struct glyph *g, bool vertical, int marks, int t)
{
    int extent = vertical ? GLYPH_CELL_H : GLYPH_CELL_W;
    int lo = (vertical ? GLYPH_CELL_W - t : GLYPH_CELL_H - t) / 2, hi = lo + t;
    int pieces = 2 * marks - 1;

    for (int m = 0; m < marks; m++) {
        int a = cut(extent, 2 * m, pieces), b = cut(extent, 2 * m + 1, pieces);
        if (vertical) glyph_paint_rect(g, a, b, lo, hi, 255);
        else          glyph_paint_rect(g, lo, hi, a, b, 255);
    }
}

static void draw_box_single(struct glyph *g, const int *arm, int thin)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;
    int heavy = 2 * thin;
    int across = arm[DIR_LEFT] > arm[DIR_RIGHT] ? arm[DIR_LEFT] : arm[DIR_RIGHT];
    int along = arm[DIR_UP] > arm[DIR_DOWN] ? arm[DIR_UP] : arm[DIR_DOWN];
    int hy0 = (H - (across == ARM_HEAVY ? heavy : thin)) / 2;
    int hy1 = hy0 + (across == ARM_HEAVY ? heavy : thin);
    int vx0 = (W - (along == ARM_HEAVY ? heavy : thin)) / 2;
    int vx1 = vx0 + (along == ARM_HEAVY ? heavy : thin);

    for (int d = DIR_UP; d <= DIR_RIGHT; d++) {
        if (!arm[d])
            continue;
        int t = arm[d] == ARM_HEAVY ? heavy : thin;
        bool vertical = d == DIR_UP || d == DIR_DOWN;
        int lo = (vertical ? W - t : H - t) / 2, hi = lo + t;
        switch (d) {
        case DIR_UP:    glyph_paint_rect(g, 0, hy1, lo, hi, 255); break;
        case DIR_DOWN:  glyph_paint_rect(g, hy0, H, lo, hi, 255); break;
        case DIR_LEFT:  glyph_paint_rect(g, lo, hi, 0, vx1, 255); break;
        case DIR_RIGHT: glyph_paint_rect(g, lo, hi, vx0, W, 255); break;
        }
    }
}

static void draw_arc(struct glyph *g, bool down, bool right, int thin)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;
    double cy = arm_centre(H, thin), cx = arm_centre(W, thin);
    double sy = down ? 1.0 : -1.0, sx = right ? 1.0 : -1.0;
    double room_y = down ? H - cy : cy, room_x = right ? W - cx : cx;
    double r = room_y < room_x ? room_y : room_x;

    double oy = cy + sy * r, ox = cx + sx * r;

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double py = y + 0.5, px = x + 0.5, d;
            if (sy * (py - oy) <= 0 && sx * (px - ox) <= 0)
                d = fabs(hypot(py - oy, px - ox) - r);
            else if (sy * (py - oy) > 0)
                d = fabs(px - cx);
            else
                d = fabs(py - cy);
            if (on_stroke(d, thin))
                glyph_paint_rect(g, y, y + 1, x, x + 1, 255);
        }
}

static void draw_diagonal(struct glyph *g, bool falling, int thin)
{
    int H = GLYPH_CELL_H, W = GLYPH_CELL_W;
    double len = hypot(H, W);

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double py = y + 0.5, px = falling ? x + 0.5 : W - (x + 0.5);
            if (on_stroke(fabs(H * px - W * py) / len, thin))
                glyph_paint_rect(g, y, y + 1, x, x + 1, 255);
        }
}

static bool draw_box_double(uint32_t cp, struct glyph *g, int thin)
{
    static const uint8_t offset[] = {
        0, 2, 4, 7, 10, 14, 17, 20, 24, 27, 30, 34, 37, 40, 44,
        47, 50, 55, 58, 61, 66, 69, 72, 77, 80, 83, 88, 91, 94, 102,
    };
    static const uint16_t glyph[] = {
        0x21a, 0x22c, 0x688, 0xb08, 0x39a, 0x3ac, 0xf8a, 0x2be,
        0x68e, 0xb0e, 0x29a, 0x68a, 0x32c, 0xb0c, 0xf8a, 0xe1a,
        0xe2c, 0x68e, 0xb0e, 0xa3e, 0xa1a, 0xb0a, 0x62c, 0x68c,
        0x39a, 0x3ac, 0xfa8, 0x2be, 0x6b8, 0xb38, 0x2ac, 0x6a8,
        0x31a, 0xb18, 0xe1a, 0xe2c, 0xfa8, 0xa3e, 0x6b8, 0xb38,
        0xa2c, 0xb28, 0x61a, 0x698, 0x39a, 0x3ac, 0xf88, 0x688,
        0xb08, 0x33e, 0x688, 0x32c, 0xb0c, 0x31a, 0xb18, 0xe1a,
        0xe2c, 0xf88, 0x688, 0xb08, 0x63e, 0xb08, 0x62c, 0x68c,
        0x61a, 0x698, 0x21a, 0x22c, 0xf8d, 0x68e, 0xb0e, 0x23e,
        0x21a, 0x32c, 0xb0c, 0x62c, 0x68c, 0x21a, 0x22c, 0xf90,
        0x6b8, 0xb38, 0x23e, 0x22c, 0x31a, 0xb18, 0x61a, 0x698,
        0x21a, 0x22c, 0xf88, 0x688, 0xb08, 0x23e, 0x32c, 0xb0c,
        0x62c, 0x68c, 0x31a, 0xb18, 0x61a, 0x698,
    };

    if (cp < 0x2550 || cp > 0x256C)
        return false;

    int y[8] = { 0, GLYPH_CELL_H, (GLYPH_CELL_H - 3 * thin) / 2 };
    y[3] = y[2] + thin;
    y[4] = y[3] + thin;
    y[5] = y[4] + thin;
    y[6] = (GLYPH_CELL_H - thin) / 2;
    y[7] = y[6] + thin;
    int x[8] = { 0, GLYPH_CELL_W, (GLYPH_CELL_W - 3 * thin) / 2 };
    x[3] = x[2] + thin;
    x[4] = x[3] + thin;
    x[5] = x[4] + thin;
    x[6] = (GLYPH_CELL_W - thin) / 2;
    x[7] = x[6] + thin;

    unsigned shape = cp - 0x2550;
    for (unsigned p = offset[shape]; p < offset[shape + 1]; p++) {
        unsigned r = glyph[p];
        glyph_paint_rect(g, y[r & 7], y[r >> 3 & 7],
                         x[r >> 6 & 7], x[r >> 9 & 7], 255);
    }
    return true;
}

static bool draw_box(uint32_t cp, struct glyph *g)
{
    if (cp < 0x2500 || cp > 0x257F)
        return false;

    int thin = thin_stroke();

    switch (cp) {
    case 0x256D: draw_arc(g, true, true, thin); return true;
    case 0x256E: draw_arc(g, true, false, thin); return true;
    case 0x256F: draw_arc(g, false, false, thin); return true;
    case 0x2570: draw_arc(g, false, true, thin); return true;
    case 0x2571: draw_diagonal(g, false, thin); return true;
    case 0x2572: draw_diagonal(g, true, thin); return true;
    case 0x2573:
        draw_diagonal(g, true, thin);
        draw_diagonal(g, false, thin);
        return true;
    }
    if (draw_box_double(cp, g, thin))
        return true;

    int marks = cp >= 0x2504 && cp <= 0x2507 ? 3
              : cp >= 0x2508 && cp <= 0x250B ? 4
              : cp >= 0x254C && cp <= 0x254F ? 2 : 0;
    if (marks) {
        int i = (int)(cp & 3);
        draw_dashes(g, i >= 2, marks, (i & 1) ? 2 * thin : thin);
        return true;
    }

    uint8_t packed = box_weights[cp - 0x2500];
    if (!packed)
        return false;
    const int arm[4] = {
        packed >> 2 * DIR_UP & 3, packed >> 2 * DIR_DOWN & 3,
        packed >> 2 * DIR_LEFT & 3, packed >> 2 * DIR_RIGHT & 3,
    };
    draw_box_single(g, arm, thin);
    return true;
}

void glyph_render_5x7(uint32_t cp, struct glyph *g)
{
    if (cp < 32 || cp >= 127)
        return;
    if (g->width < 7)
        return;

    const uint8_t *bits = font5x7[cp - 32];
    for (int r = 0; r < 7; r++) {
        uint16_t mask = 0;
        for (int c = 0; c < 5; c++) {
            if ((bits[c] >> r) & 1)
                mask |= (uint16_t)(1 << (c + 2));
        }
        for (int dy = 0; dy < 2; dy++) {
            int y = 2 + r * 2 + dy;
            if (y < glyph_rows(g)) {
                glyph_mask(g, y)[0] = mask;
                for (int c = 0; c < 5; c++) {
                    if ((bits[c] >> r) & 1)
                        g->alpha[y][c + 2] = 255;
                }
            }
        }
    }
}

void glyph_render_unknown_box(struct glyph *g)
{
    int x1 = GLYPH_CELL_W > 2 ? GLYPH_CELL_W - 1 : GLYPH_CELL_W;
    int x0 = GLYPH_CELL_W > 2 ? 1 : 0;
    int y0 = GLYPH_CELL_H / 6, y1 = GLYPH_CELL_H - GLYPH_CELL_H / 6;
    if (y1 <= y0 + 1) { y0 = 0; y1 = GLYPH_CELL_H; }
    glyph_paint_rect(g, y0, y1, x0, x0 + 1, 255);
    glyph_paint_rect(g, y0, y1, x1 - 1, x1, 255);
    glyph_paint_rect(g, y0, y0 + 1, x0, x1, 255);
    glyph_paint_rect(g, y1 - 1, y1, x0, x1, 255);
    glyph_paint_rect(g, (y0 + y1) / 2 - 1, (y0 + y1) / 2 + 1, (x0 + x1) / 2 - 1, (x0 + x1) / 2, 255);
}

bool is_cell_aligned_symbol(uint32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x259F) ||
           (cp >= 0x1FB00 && cp <= 0x1FB3B) ||
           (cp >= 0x1FB68 && cp <= 0x1FB9F && cp != 0x1FB93) ||
           (cp >= 0x1CC21 && cp <= 0x1CC2F) ||
           (cp >= 0x1CD00 && cp <= 0x1CDE5) ||
           (cp >= 0x1CE51 && cp <= 0x1CE8F);
}

bool draw_cell_aligned_symbol(uint32_t cp, struct glyph *g)
{
    if (cp >= 0x2500 && cp <= 0x257F)
        return draw_box(cp, g);
    if (cp >= 0x2580 && cp <= 0x259F)
        return draw_block(cp, g);
    if (cp >= 0x1FB00 && cp <= 0x1FB9F)
        return draw_legacy_computing_symbol(cp, g);
    if (cp >= 0x1CC00 && cp <= 0x1CE8F)
        return draw_legacy_supplement_symbol(cp, g);
    return false;
}
