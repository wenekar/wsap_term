#define _GNU_SOURCE
#include "font/backends/freetype/internal.h"

#include <stdlib.h>
#include <string.h>

static inline uint8_t subpx_luma(unsigned a, unsigned b, unsigned c)
{
    return (uint8_t)((a * 77 + b * 150 + c * 29) >> 8);
}

static inline uint8_t lcd_luma3(const uint8_t *row, unsigned int c)
{
    const uint8_t *p = row + (size_t)c * 3;
    return subpx_luma(p[0], p[1], p[2]);
}

static inline const uint8_t *bitmap_row(const FT_Bitmap *bm, int r)
{
    int pitch = bm->pitch;
    size_t apitch = (size_t)(pitch < 0 ? -(int64_t)pitch : pitch);
    int y = pitch >= 0 ? r : (int)bm->rows - 1 - r;
    return bm->buffer + (size_t)y * apitch;
}

static inline uint8_t lcdv_luma(const FT_Bitmap *bm, int r, unsigned int c)
{
    return subpx_luma(bitmap_row(bm, r * 3)[c],
                      bitmap_row(bm, r * 3 + 1)[c],
                      bitmap_row(bm, r * 3 + 2)[c]);
}

static inline uint8_t bitmap_alpha(const FT_Bitmap *bm, const uint8_t *row, unsigned int c)
{
    switch (bm->pixel_mode) {
    case FT_PIXEL_MODE_GRAY:
        return (bm->num_grays == 256 || bm->num_grays <= 1) ? row[c]
            : (uint8_t)((unsigned int)row[c] * 255 / (bm->num_grays - 1));
    case FT_PIXEL_MODE_MONO:
        return (row[c >> 3] & (0x80u >> (c & 7))) ? 255 : 0;
    case FT_PIXEL_MODE_GRAY2:
        return (uint8_t)(((row[c >> 2] >> (6 - 2 * (c & 3))) & 3) * 85);
    case FT_PIXEL_MODE_GRAY4:
        return (uint8_t)(((row[c >> 1] >> ((c & 1) ? 0 : 4)) & 15) * 17);
    case FT_PIXEL_MODE_LCD:
        return lcd_luma3(row, c);
    default:
        return 0;
    }
}

bool supported_pixel_mode(const FT_Bitmap *bitmap)
{
    switch (bitmap->pixel_mode) {
    case FT_PIXEL_MODE_GRAY:
    case FT_PIXEL_MODE_MONO:
    case FT_PIXEL_MODE_GRAY2:
    case FT_PIXEL_MODE_GRAY4:
    case FT_PIXEL_MODE_LCD:
    case FT_PIXEL_MODE_LCD_V:
    case FT_PIXEL_MODE_BGRA:
        return true;
    default:
        return false;
    }
}

struct box ft_place_ink(int left, int above_baseline, int w, int h,
                        int advance, int cols, int max_rows, bool color)
{
    int rows = max_rows > 0 && max_rows < GLYPH_CELL_H ? max_rows : GLYPH_CELL_H;
    if (h <= 0) h = 1;
    struct box box = { left, font_thread_metrics.baseline - above_baseline, w, h };

    if (color || h > rows) {
        box.w = w * rows / h > 0 ? w * rows / h : 1;
        box.h = rows;
        box.x = (cols - box.w) / 2;
        box.y = (GLYPH_CELL_H - box.h) / 2;
        return box;
    }

    if (box.y < 0)
        box.y = 0;
    else if (box.y + box.h > rows)
        box.y = rows - box.h;

    if (advance > 0 && advance < cols)
        box.x += (cols - advance) / 2;
    return box;
}

bool ft_prepare_canvas(struct glyph *g, struct box *ink)
{
    int allocated = g->width;
    int max_span = 4 * allocated > 60 ? 4 * allocated : 60;
    int left = ink->x < 0 ? ink->x : 0;
    int right = ink->x + ink->w > allocated ? ink->x + ink->w : allocated;

    if (right - left > max_span) {
        int span = right - left;
        ink->w = (ink->w * max_span / span) < 1 ? 1 : ink->w * max_span / span;
        ink->h = (ink->h * max_span / span) < 1 ? 1 : ink->h * max_span / span;
        ink->x = (allocated - ink->w) / 2;
        left = ink->x < 0 ? ink->x : 0;
        right = ink->x + ink->w > allocated ? ink->x + ink->w : allocated;
    }

    int total = right - left > max_span ? max_span : right - left;
    g->x_offset = (int16_t)left;
    ink->x -= left;
    return glyph_prepare(g, (unsigned)total);
}

static void blit_direct(struct glyph *g, const FT_Bitmap *bm, const struct box *dst)
{
    int src_w = bitmap_width(bm);
    int src_h = bitmap_height(bm);

    int r0 = dst->y < 0 ? -dst->y : 0;
    int rows = glyph_rows(g);
    int r1 = dst->y + src_h > rows ? rows - dst->y : src_h;
    int c0 = dst->x < 0 ? -dst->x : 0;
    int c1 = dst->x + src_w > (int)g->width ? (int)g->width - dst->x : src_w;
    if (r0 >= r1 || c0 >= c1)
        return;

    bool color = bm->pixel_mode == FT_PIXEL_MODE_BGRA;
    bool is_lcd_v = bm->pixel_mode == FT_PIXEL_MODE_LCD_V;
    bool is_lcd = bm->pixel_mode == FT_PIXEL_MODE_LCD;

    if (is_lcd && !g->has_subpx_ink) {
        glyph_subpx_plane(g);
    }

    for (int r = r0; r < r1; r++) {
        int y = dst->y + r;
        const uint8_t *src_row = bitmap_row(bm, is_lcd_v ? r * 3 + 1 : r);

        if (color) {
            for (int c = c0; c < c1; c++) {
                uint32_t px;
                memcpy(&px, src_row + (size_t)c * 4, sizeof(px));
                if (px >> 24) {
                    *glyph_color_at(g, y, dst->x + c) = px;
                    glyph_mark(g, y, dst->x + c);
                }
            }
            continue;
        }

        if (is_lcd && g->has_subpx_ink && g->subpx) {
            uint8_t *alpha = g->alpha[y];
            uint8_t *subpx_row = g->subpx[y];
            for (int c = c0; c < c1; c++) {
                const uint8_t *p = src_row + (size_t)c * 3;
                uint8_t sr = p[0], sg = p[1], sb = p[2];
                int x = dst->x + c;
                if (sr > subpx_row[x * 3 + 0]) subpx_row[x * 3 + 0] = sr;
                if (sg > subpx_row[x * 3 + 1]) subpx_row[x * 3 + 1] = sg;
                if (sb > subpx_row[x * 3 + 2]) subpx_row[x * 3 + 2] = sb;
                uint8_t a = subpx_luma(sr, sg, sb);
                if (a > alpha[x])
                    alpha[x] = a;
                if (sr || sg || sb || alpha[x])
                    glyph_mark(g, y, x);
            }
            continue;
        }

        uint8_t *alpha = g->alpha[y];
        for (int c = c0; c < c1; c++) {
            uint8_t a = is_lcd_v ? lcdv_luma(bm, r, c)
                                 : bitmap_alpha(bm, src_row, c);
            int x = dst->x + c;
            if (a > alpha[x])
                alpha[x] = a;
            if (alpha[x])
                glyph_mark(g, y, x);
        }
    }
}

static void blit_scaled(struct glyph *g, const FT_Bitmap *bm, const struct box *dst)
{
    int sw = bitmap_width(bm), sh = bitmap_height(bm);
    bool color = bm->pixel_mode == FT_PIXEL_MODE_BGRA;
    bool is_lcd_v = bm->pixel_mode == FT_PIXEL_MODE_LCD_V;
    bool is_lcd = bm->pixel_mode == FT_PIXEL_MODE_LCD;

    if (is_lcd && !g->has_subpx_ink) {
        glyph_subpx_plane(g);
    }

    int rows = glyph_rows(g);
    for (int r = 0; r < dst->h; r++) {
        int y = dst->y + r;
        if (y < 0 || y >= rows)
            continue;
        int sy = r * sh / dst->h, sy_end = (r + 1) * sh / dst->h;
        if (sy_end <= sy) sy_end = sy + 1;
        if (sy_end > sh) sy_end = sh;

        for (int c = 0; c < dst->w; c++) {
            int x = dst->x + c;
            if (x < 0 || x >= (int)g->width)
                continue;
            int sx = c * sw / dst->w, sx_end = (c + 1) * sw / dst->w;
            if (sx_end <= sx) sx_end = sx + 1;
            if (sx_end > sw) sx_end = sw;

            if (color) {
                unsigned sum[4] = {0}, count = 0;
                for (int yy = sy; yy < sy_end; yy++) {
                    const uint8_t *row = bitmap_row(bm, yy);
                    for (int xx = sx; xx < sx_end; xx++) {
                        for (int k = 0; k < 4; k++)
                            sum[k] += row[xx * 4 + k];
                        count++;
                    }
                }
                if (!count)
                    continue;
                uint8_t a = (uint8_t)(sum[3] / count);
                if (!a)
                    continue;
                uint8_t b = (uint8_t)(sum[0] / count), gg = (uint8_t)(sum[1] / count), rr = (uint8_t)(sum[2] / count);
                *glyph_color_at(g, y, x) = ((uint32_t)a << 24) |
                    ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | b;
                glyph_mark(g, y, x);
            } else if (is_lcd && g->has_subpx_ink && g->subpx) {
                unsigned sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
                for (int yy = sy; yy < sy_end; yy++) {
                    const uint8_t *row = bitmap_row(bm, yy);
                    for (int xx = sx; xx < sx_end; xx++) {
                        const uint8_t *p = row + (size_t)xx * 3;
                        sum_r += p[0];
                        sum_g += p[1];
                        sum_b += p[2];
                        count++;
                    }
                }
                if (!count)
                    continue;
                uint8_t sr = (uint8_t)(sum_r / count);
                uint8_t sg = (uint8_t)(sum_g / count);
                uint8_t sb = (uint8_t)(sum_b / count);
                if (sr > g->subpx[y][x * 3 + 0]) g->subpx[y][x * 3 + 0] = sr;
                if (sg > g->subpx[y][x * 3 + 1]) g->subpx[y][x * 3 + 1] = sg;
                if (sb > g->subpx[y][x * 3 + 2]) g->subpx[y][x * 3 + 2] = sb;
                uint8_t a = subpx_luma(sr, sg, sb);
                if (a > g->alpha[y][x])
                    g->alpha[y][x] = a;
                if (sr || sg || sb || g->alpha[y][x])
                    glyph_mark(g, y, x);
            } else {
                unsigned sum = 0, count = 0;
                for (int yy = sy; yy < sy_end; yy++) {
                    const uint8_t *row = bitmap_row(bm, yy);
                    for (int xx = sx; xx < sx_end; xx++) {
                        sum += is_lcd_v ? lcdv_luma(bm, yy, xx)
                                        : bitmap_alpha(bm, row, xx);
                        count++;
                    }
                }
                if (!count)
                    continue;
                uint8_t a = (uint8_t)(sum / count);
                if (a > g->alpha[y][x])
                    g->alpha[y][x] = a;
                if (g->alpha[y][x])
                    glyph_mark(g, y, x);
            }
        }
    }
}

void ft_blit_bitmap(struct glyph *g, const FT_Bitmap *bm, const struct box *dst)
{
    int w = bitmap_width(bm);
    int h = bitmap_height(bm);
    if (dst->w <= 0 || dst->h <= 0 || w == 0 || h == 0)
        return;
    if (dst->w == w && dst->h == h)
        blit_direct(g, bm, dst);
    else
        blit_scaled(g, bm, dst);
}
