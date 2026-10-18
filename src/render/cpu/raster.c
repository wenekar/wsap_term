#include "render/cpu/raster.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "render/cpu/cell_cache.h"
#include "render/cpu/decor.h"
#include "render/cpu/isa.h"
#include "render/cpu/kernels.h"
#include "render/frame/walk.h"

/*
 * The row raster. See raster.h for the two passes and the spill rules.
 *
 * Coordinates. `sx0` is the grid-local x of the cell span. The cell-local
 * x of glyph column 0 is `ox`: g->x_offset for a one-column cell and the
 * left half of a wide glyph, and g->x_offset - cw for the right half.
 *
 * Ownership of overhang ink. A one-column cell spills on both sides. The
 * left half of a wide glyph spills only to the left, and the right half
 * only to the right. The ink between the halves belongs to the half that
 * covers it. So each half holds its own part, and a clipped walk that
 * starts at either half sees the same spills.
 */

/* The glyph id bits of an L0 key: a code point below 128 and the variant. */
#define L0_ID_BITS ((uint64_t)(CELL_CACHE_L0_CPS - 1) | (uint64_t)FONT_STYLE_VARIANT_MASK << 32)

/* Faint and temporary coverage go through a stack buffer of this many pixels. */
#define COV_CHUNK 256

enum glyph_kind {
    KIND_ALPHA = RASTER_SPILL_ALPHA,
    KIND_SUBPX = RASTER_SPILL_SUBPX,
    KIND_COLOR = RASTER_SPILL_COLOR,
};

/* The ink box of a glyph: columns [lo, hi) and rows [y0, y1). Empty: lo >= hi. */
struct ink {
    int lo, hi;
    int y0, y1;
    enum glyph_kind kind;
};

/* The state of one row job. Only its own thread touches it. */
struct row_ctx {
    struct raster_worker *w;
    uint32_t *row;
    size_t stride;
    int cw, ch;
    int x0, x1;
    int area, width;
    int reach;
    /* The open fill run [run_x0, run_x1) of color run_bg. Empty: -1, -1. */
    int run_x0, run_x1;
    uint32_t run_bg;
    /* The L0 pair: the default pair after reverse video. */
    uint32_t l0_fg, l0_bg;
    uint32_t margin;
    uint32_t cursor;
    uint64_t font_version;
    struct cell_class *klass;
    struct cell_cache_worker *cache;
    const struct cell_l0 *l0;
    const struct decor_set *decor;
    /* Faint coverage of one chunk. It lives here, not on the stack of the
     * cell callback, so that the callback needs no large frame. */
    uint8_t tmp[3 * COV_CHUNK];
};

static inline int min_i(int a, int b)
{
    return a < b ? a : b;
}

static inline int max_i(int a, int b)
{
    return a > b ? a : b;
}

/* The rows of a glyph that the cell shows. */
static inline int glyph_rows(const struct glyph *g, int ch)
{
    int rows = g->height ? g->height : ch;
    return rows < ch ? rows : ch;
}

/* The ink box of a glyph from its row masks. A glyph with no ink gives an empty box. */
static inline RENDER_ALWAYS_INLINE struct ink glyph_ink(const struct glyph *g, int ch)
{
    struct ink k = { .lo = INT_MAX, .hi = INT_MIN };
    if (!g || !g->width || !g->row_masks || (g->has_color && !g->color))
        return (struct ink){0};
    k.kind = g->has_color ? KIND_COLOR : g->has_subpx_ink && g->subpx ? KIND_SUBPX : KIND_ALPHA;
    int rows = glyph_rows(g, ch);
    int words = g->mask_words;
    int y0 = -1, y1 = 0;
    if (words == 1) {
        uint64_t any = 0;
        for (int y = 0; y < rows; y++) {
            uint64_t m = g->row_masks[y];
            if (!m)
                continue;
            if (y0 < 0)
                y0 = y;
            y1 = y + 1;
            any |= m;
        }
        if (any) {
            k.lo = __builtin_ctzll(any);
            k.hi = 64 - __builtin_clzll(any);
        }
    } else {
        for (int y = 0; y < rows; y++) {
            const uint64_t *m = glyph_mask(g, y);
            bool inked = false;
            for (int i = 0; i < words; i++) {
                if (!m[i])
                    continue;
                inked = true;
                int lo = i * 64 + __builtin_ctzll(m[i]);
                int hi = i * 64 + 64 - __builtin_clzll(m[i]);
                if (lo < k.lo)
                    k.lo = lo;
                if (hi > k.hi)
                    k.hi = hi;
            }
            if (!inked)
                continue;
            if (y0 < 0)
                y0 = y;
            y1 = y + 1;
        }
    }
    if (y0 < 0)
        return (struct ink){0};
    if (k.hi > g->width)
        k.hi = g->width;
    if (k.lo >= k.hi)
        return (struct ink){0};
    k.y0 = y0;
    k.y1 = y1;
    return k;
}

/* ---- Pass 1: fills and tiles ------------------------------------------ */

/*
 * Fills the columns [a, b) of the row band with bg. Each level has one
 * out-of-line wrapper, so the cell callback carries no fill loop.
 */
typedef void (*run_fill_fn)(const struct row_ctx *c, int a, int b, uint32_t bg);

static inline RENDER_ALWAYS_INLINE void run_fill_body(int isa, const struct row_ctx *c, int a,
                                                      int b, uint32_t bg)
{
    uint32_t *p = c->row + a;
    if (a == 0 && b == c->width && c->stride == (size_t)c->width) {
        render_kernel_fill(isa, p, c->ch * c->width, bg);
        return;
    }
    for (int y = 0; y < c->ch; y++)
        render_kernel_fill(isa, p + (size_t)y * c->stride, b - a, bg);
}

static inline RENDER_ALWAYS_INLINE void run_flush(struct row_ctx *c, run_fill_fn fill)
{
    if (c->run_x1 < 0)
        return;
    int a = max_i(c->run_x0, c->x0), b = min_i(c->run_x1, c->x1);
    c->run_x0 = c->run_x1 = -1;
    if (a < b)
        fill(c, a, b, c->run_bg);
}

static inline RENDER_ALWAYS_INLINE void run_add(struct row_ctx *c, int x0, int x1, uint32_t bg,
                                                run_fill_fn fill)
{
    if (c->run_x1 == x0 && c->run_bg == bg) {
        c->run_x1 = x1;
        return;
    }
    run_flush(c, fill);
    c->run_x0 = x0;
    c->run_x1 = x1;
    c->run_bg = bg;
}

/*
 * Copies a whole tile of cw x ch pixels, row stride cw, to dst. A cell 8 to
 * 16 pixels wide copies each row with two fixed 32-byte copies, which every
 * level lowers to vector moves in its own wrapper: one ymm on AVX2 and
 * AVX-512, two 16-byte moves elsewhere. At cw == 8 the two copies are the
 * rows y and y + 1. The generic kernel is a call with a loop for each width
 * class, and a typing frame spends most of its blit time there.
 */
static inline RENDER_ALWAYS_INLINE void tile_blit(int isa, uint32_t *dst, size_t ds,
                                                  const uint32_t *tile, int cw, int ch)
{
    if (cw == 8) {
        int y = 0;
        for (; y + 2 <= ch; y += 2) {
            memcpy(dst + (size_t)y * ds, tile + (size_t)y * 8, 32);
            memcpy(dst + (size_t)y * ds + ds, tile + (size_t)y * 8 + 8, 32);
        }
        if (y < ch)
            memcpy(dst + (size_t)y * ds, tile + (size_t)y * 8, 32);
        return;
    }
    if (cw > 8 && cw <= 16) {
        for (int y = 0; y < ch; y++) {
            const uint32_t *s = tile + (size_t)y * (size_t)cw;
            uint32_t *d = dst + (size_t)y * ds;
            memcpy(d, s, 32);
            memcpy(d + cw - 8, s + cw - 8, 32);
        }
        return;
    }
    render_kernel_blit_tile(isa, dst, ds, tile, cw, ch);
}

/* Copies a cell of cw x ch pixels, row stride cw, into the window. */
static inline RENDER_ALWAYS_INLINE void put_tile(int isa, const struct row_ctx *c, int sx0,
                                                 const uint32_t *tile)
{
    int cw = c->cw;
    if (sx0 >= c->x0 && sx0 + cw <= c->x1) {
        tile_blit(isa, c->row + sx0, c->stride, tile, cw, c->ch);
        return;
    }
    int a = max_i(sx0, c->x0), b = min_i(sx0 + cw, c->x1);
    if (a >= b)
        return;
    for (int y = 0; y < c->ch; y++)
        memcpy(c->row + (size_t)y * c->stride + a, tile + (size_t)y * cw + (a - sx0),
               (size_t)(b - a) * sizeof(*tile));
}

/* ---- Compose ---------------------------------------------------------- */

static inline uint8_t faint_cov(uint8_t c)
{
    return (uint8_t)((c + 1) >> 1);
}

/* Blends n pixels of alpha coverage over dst, with the faint rule. */
static inline RENDER_ALWAYS_INLINE void blend_alpha_cov(int isa, uint8_t *tmp, uint32_t *dst,
                                                        const uint8_t *cov, int n, uint32_t fg,
                                                        bool faint)
{
    if (!faint) {
        render_kernel_blend_any(isa, dst, cov, n, fg);
        return;
    }
    for (int i = 0; i < n; i += COV_CHUNK) {
        int m = min_i(n - i, COV_CHUNK);
        for (int j = 0; j < m; j++)
            tmp[j] = faint_cov(cov[i + j]);
        render_kernel_blend_any(isa, dst + i, tmp, m, fg);
    }
}

/* Blends n pixels of subpixel coverage over dst, with the faint rule. */
static inline RENDER_ALWAYS_INLINE void blend_subpx_cov(int isa, uint8_t *tmp, uint32_t *dst,
                                                        const uint8_t *cov, int n, uint32_t fg,
                                                        bool faint)
{
    if (!faint) {
        render_kernel_blend_any_subpx(isa, dst, cov, n, fg);
        return;
    }
    for (int i = 0; i < n; i += COV_CHUNK) {
        int m = min_i(n - i, COV_CHUNK);
        for (int j = 0; j < 3 * m; j++)
            tmp[j] = faint_cov(cov[3 * i + j]);
        render_kernel_blend_any_subpx(isa, dst + i, tmp, m, fg);
    }
}

/* One decoration or cursor mask over a cell. */
static inline RENDER_ALWAYS_INLINE void mask_blend(int isa, uint32_t *dst, size_t ds,
                                                   const struct decor_mask *m, int cw,
                                                   uint32_t color, bool faint)
{
    if (m->y0 >= m->y1)
        return;
    if (m->solid && !faint) {
        for (int y = m->y0; y < m->y1; y++)
            render_kernel_fill(isa, dst + (size_t)y * ds, cw, color);
        return;
    }
    const uint8_t *cov = faint ? m->faint : m->cov;
    for (int y = m->y0; y < m->y1; y++)
        render_kernel_blend_any(isa, dst + (size_t)y * ds, cov + (size_t)y * cw, cw, color);
}

/*
 * Composes one cell into dst (row stride ds): the bg, the glyph inside the
 * cell span, the decorations, and the cursor marks. The result depends only
 * on the cell key, so a tile and a direct compose give the same pixels.
 */
static inline RENDER_ALWAYS_INLINE void compose(int isa, struct row_ctx *c, uint32_t *dst,
                                                size_t ds, const struct render_cell *cell,
                                                const struct glyph *g, const struct ink *ink,
                                                int half)
{
    const int cw = c->cw, ch = c->ch;
    const uint32_t fg = cell->fg, bg = cell->bg;
    const uint16_t fl = cell->flags;
    const bool faint = (fl & RENDER_CELL_FAINT) != 0;
    if (ds == (size_t)cw) {
        render_kernel_fill(isa, dst, cw * ch, bg);
    } else {
        for (int y = 0; y < ch; y++)
            render_kernel_fill(isa, dst + (size_t)y * ds, cw, bg);
    }

    if (ink->lo < ink->hi) {
        int ox = g->x_offset - half * cw;
        int lo = max_i(ink->lo, -ox), hi = min_i(ink->hi, cw - ox);
        if (lo < hi) {
            int n = hi - lo;
            uint32_t *d = dst + (ox + lo);
            switch (ink->kind) {
            case KIND_ALPHA: {
                const uint32_t *lut = pair_lut_alpha(&c->w->lut, fg, bg, faint);
                for (int y = ink->y0; y < ink->y1; y++) {
                    const uint8_t *a = g->alpha[y] + lo;
                    if (lut)
                        render_kernel_compose_alpha_lut(isa, d + (size_t)y * ds, a, n, lut);
                    else
                        blend_alpha_cov(isa, c->tmp, d + (size_t)y * ds, a, n, fg, faint);
                }
                break;
            }
            case KIND_SUBPX: {
                /* The table matches the per-pixel blend only over an opaque bg. */
                const struct subpx_lut *lut =
                    bg >> 24 == 255 ? pair_lut_subpx(&c->w->lut, fg, bg, faint) : NULL;
                for (int y = ink->y0; y < ink->y1; y++) {
                    const uint8_t *s = g->subpx[y] + (size_t)lo * 3;
                    if (lut)
                        render_kernel_compose_subpx_lut(isa, d + (size_t)y * ds, s, n, lut, bg);
                    else
                        blend_subpx_cov(isa, c->tmp, d + (size_t)y * ds, s, n, fg, faint);
                }
                break;
            }
            case KIND_COLOR:
                for (int y = ink->y0; y < ink->y1; y++)
                    render_kernel_compose_color(isa, d + (size_t)y * ds,
                                                g->color + (size_t)y * g->width + lo, n, faint);
                break;
            }
        }
    }

    const struct decor_set *dc = c->decor;
    if (!dc || !(fl & (RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)))
        return;
    unsigned ul = render_cell_underline(fl);
    if (ul && ul < DECOR_UNDERLINE_KINDS)
        mask_blend(isa, dst, ds, &dc->underline[ul], cw, fg, faint);
    if (fl & RENDER_CELL_STRIKE)
        mask_blend(isa, dst, ds, &dc->strike, cw, fg, faint);
    if (fl & RENDER_CELL_OVERLINE)
        mask_blend(isa, dst, ds, &dc->overline, cw, fg, faint);
    if (fl & RENDER_CELL_CURSOR_BAR)
        mask_blend(isa, dst, ds, &dc->cursor_bar, cw, c->cursor, false);
    if (fl & RENDER_CELL_CURSOR_UNDERLINE)
        mask_blend(isa, dst, ds, &dc->cursor_underline, cw, c->cursor, false);
}

/* ---- Spills ----------------------------------------------------------- */

/* Grows the list to at least `cap` items and `size` coverage bytes. */
static bool spill_grow(struct raster_spill_list *l, int cap, size_t size)
{
    if (cap > l->cap) {
        struct raster_spill *items = realloc(l->items, (size_t)cap * sizeof(*items));
        if (!items)
            return false;
        l->items = items;
        l->cap = cap;
    }
    if (size > l->size) {
        uint8_t *cov = realloc(l->cov, size);
        if (!cov)
            return false;
        l->cov = cov;
        l->size = size;
    }
    return true;
}

/* Makes room for one more item and for `bytes` coverage bytes in all. */
static bool spill_room(struct raster_spill_list *l, size_t bytes)
{
    if (l->n < l->cap && bytes <= l->size)
        return true;
    int cap = l->n < l->cap ? l->cap : l->cap ? 2 * l->cap : 64;
    size_t size = l->size ? l->size : 4096;
    while (size < bytes)
        size *= 2;
    return spill_grow(l, cap, size);
}

/*
 * Copies the glyph ink in grid-local columns [a, b) into the spill list.
 * `gx0` is the grid-local x of glyph column 0. The part clips to the cell
 * area and to the window. A failed allocation drops the part.
 */
static void spill_add(struct row_ctx *c, const struct glyph *g, const struct ink *ink,
                      uint32_t fg, bool faint, int gx0, int a, int b)
{
    a = max_i(max_i(a, 0), c->x0);
    b = min_i(min_i(b, c->area), c->x1);
    if (a >= b)
        return;
    struct raster_spill_list *l = &c->w->spill;
    int n = b - a;
    size_t bpp = ink->kind == KIND_COLOR ? 4 : ink->kind == KIND_SUBPX ? 3 : 1;
    size_t row_bytes = (size_t)n * bpp;
    size_t off = (l->used + 3) & ~(size_t)3;
    size_t end = off + row_bytes * (size_t)(ink->y1 - ink->y0);
    if (!spill_room(l, end))
        return;
    uint8_t *dst = l->cov + off;
    int gx = a - gx0;
    for (int y = ink->y0; y < ink->y1; y++, dst += row_bytes) {
        switch (ink->kind) {
        case KIND_ALPHA: {
            const uint8_t *s = g->alpha[y] + gx;
            if (faint) {
                for (int i = 0; i < n; i++)
                    dst[i] = faint_cov(s[i]);
            } else {
                memcpy(dst, s, row_bytes);
            }
            break;
        }
        case KIND_SUBPX: {
            const uint8_t *s = g->subpx[y] + (size_t)gx * 3;
            if (faint) {
                for (int i = 0; i < 3 * n; i++)
                    dst[i] = faint_cov(s[i]);
            } else {
                memcpy(dst, s, row_bytes);
            }
            break;
        }
        case KIND_COLOR:
            memcpy(dst, g->color + (size_t)y * g->width + gx, row_bytes);
            break;
        }
    }
    l->items[l->n++] = (struct raster_spill){
        .x0 = a, .x1 = b, .y0 = ink->y0, .y1 = ink->y1, .fg = fg, .offset = off,
        .kind = (enum raster_spill_kind)ink->kind, .faint = faint,
    };
    l->used = end;
}

/*
 * The overhang of a cell: how far its ink reaches past its span on the
 * left and the right, in the parts that this cell owns.
 */
struct overhang {
    int left, right;
};

static inline RENDER_ALWAYS_INLINE struct overhang cell_overhang(const struct glyph *g,
                                                                 const struct ink *ink,
                                                                 uint16_t flags, int half,
                                                                 int cw)
{
    struct overhang o = {0};
    if (ink->lo >= ink->hi)
        return o;
    int ox = g->x_offset - half * cw;
    int lo = ox + ink->lo, hi = ox + ink->hi;
    if (!half && lo < 0)
        o.left = -lo;
    if ((half || !(flags & RENDER_CELL_WIDE)) && hi > cw)
        o.right = hi - cw;
    return o;
}

/* Adds the overhang of a cell to the reach and copies it into the spill list. */
static inline RENDER_ALWAYS_INLINE void cell_spill(struct row_ctx *c, int sx0,
                                                   const struct render_cell *cell,
                                                   const struct glyph *g, const struct ink *ink,
                                                   int half, struct overhang o)
{
    int r = max_i(o.left, o.right);
    if (r > c->reach)
        c->reach = r;
    if (!r)
        return;
    bool faint = (cell->flags & RENDER_CELL_FAINT) != 0;
    int gx0 = sx0 + g->x_offset - half * c->cw;
    if (o.left)
        spill_add(c, g, ink, cell->fg, faint, gx0, sx0 - o.left, sx0);
    if (o.right)
        spill_add(c, g, ink, cell->fg, faint, gx0, sx0 + c->cw, sx0 + c->cw + o.right);
}

/* Pass 2: blends the spill list over the row, in list order. */
static inline RENDER_ALWAYS_INLINE void spill_blend(int isa, const struct row_ctx *c)
{
    const struct raster_spill_list *l = &c->w->spill;
    for (int i = 0; i < l->n; i++) {
        const struct raster_spill *s = &l->items[i];
        int n = s->x1 - s->x0;
        const uint8_t *cov = l->cov + s->offset;
        uint32_t *d = c->row + (size_t)s->y0 * c->stride + s->x0;
        for (int y = s->y0; y < s->y1; y++, d += c->stride) {
            switch (s->kind) {
            case RASTER_SPILL_ALPHA:
                render_kernel_blend_any(isa, d, cov, n, s->fg);
                cov += n;
                break;
            case RASTER_SPILL_SUBPX:
                render_kernel_blend_any_subpx(isa, d, cov, n, s->fg);
                cov += (size_t)n * 3;
                break;
            case RASTER_SPILL_COLOR:
                render_kernel_compose_color(isa, d, (const uint32_t *)(const void *)cov, n,
                                            s->faint);
                cov += (size_t)n * 4;
                break;
            }
        }
    }
}

/* ---- One cell --------------------------------------------------------- */

/*
 * A cell that is not a plain fill and not an L0 hit: a cell outside the
 * window, a cache lookup, or a compose. The open run is flushed. It runs
 * out of line, so that the hot path of the callback keeps a small frame.
 */
static inline RENDER_ALWAYS_INLINE void cell_slow(int isa, struct row_ctx *c, int sx0,
                                                  const struct render_cell *cell,
                                                  const struct render_glyph_ref *ref)
{
    const int cw = c->cw;
    const uint16_t fl = cell->flags;
    const int half = (fl & RENDER_CELL_WIDE_TAIL) ? 1 : 0;
    const bool blank = (fl & RENDER_CELL_BLANK) != 0;

    /* A cell outside the window can only spill into it. */
    if (sx0 + cw <= c->x0 || sx0 >= c->x1) {
        if (blank)
            return;
        const struct glyph *g = render_glyph_get(ref);
        struct ink ink = glyph_ink(g, c->ch);
        cell_spill(c, sx0, cell, g, &ink, half, cell_overhang(g, &ink, fl, half, cw));
        return;
    }

    bool marks = (fl & (RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)) != 0;
    bool cacheable = ref->id && c->klass && (c->decor || !marks);
    struct cell_key key = {0};
    if (cacheable) {
        key.glyph = ref->id;
        key.font_version = c->font_version;
        key.fg = cell->fg;
        key.bg = cell->bg;
        key.cursor = (fl & RENDER_CELL_CURSOR_MASK) ? c->cursor : 0;
        key.flags = (uint16_t)(fl & RENDER_CELL_PIXEL_MASK);
        key.half = (uint8_t)half;
        struct cell_tile t = cell_cache_find(c->klass, c->cache, &key);
        if (t.px) {
            put_tile(isa, c, sx0, t.px);
            if (t.spill) {
                const struct glyph *g = render_glyph_get(ref);
                struct ink ink = glyph_ink(g, c->ch);
                cell_spill(c, sx0, cell, g, &ink, half, cell_overhang(g, &ink, fl, half, cw));
            }
            return;
        }
    }

    const struct glyph *g = blank ? NULL : render_glyph_get(ref);
    struct ink ink = glyph_ink(g, c->ch);
    struct overhang o = cell_overhang(g, &ink, fl, half, cw);
    uint32_t *tile = cacheable ? cell_cache_claim(c->klass, c->cache, &key, o.left || o.right)
                               : NULL;
    if (tile) {
        compose(isa, c, tile, (size_t)cw, cell, g, &ink, half);
        put_tile(isa, c, sx0, tile);
    } else if (sx0 >= c->x0 && sx0 + cw <= c->x1) {
        compose(isa, c, c->row + sx0, c->stride, cell, g, &ink, half);
    } else if (c->w->scratch) {
        compose(isa, c, c->w->scratch, (size_t)cw, cell, g, &ink, half);
        put_tile(isa, c, sx0, c->w->scratch);
    }
    cell_spill(c, sx0, cell, g, &ink, half, o);
}

typedef void (*cell_slow_fn)(struct row_ctx *c, int sx0, const struct render_cell *cell,
                             const struct render_glyph_ref *ref);

/* The run callback of the walk: blank cells that draw only their bg. */
static inline RENDER_ALWAYS_INLINE void run_body(void *ctx, int c0, int c1, uint32_t bg,
                                                 run_fill_fn fill)
{
    struct row_ctx *c = ctx;
    run_add(c, c0 * c->cw, c1 * c->cw, bg, fill);
}

/*
 * The text run callback of the walk: plain ASCII cells. In the default
 * pair each one is an L0 lookup and a blit. It stops at the first cell
 * that has no L0 tile, has a spill, or leaves the window. That cell goes
 * to the cell callback, which takes the full path.
 */
static inline RENDER_ALWAYS_INLINE int text_body(int isa, void *ctx, int c0, int c1,
                                                 uint32_t fg, uint32_t bg, uint16_t style,
                                                 const uint32_t *text, int stride,
                                                 run_fill_fn fill)
{
    struct row_ctx *c = ctx;
    if (!c->klass || fg != c->l0_fg || bg != c->l0_bg)
        return c0;
    run_flush(c, fill);
    const int cw = c->cw, ch = c->ch;
    const unsigned v = style & FONT_STYLE_VARIANT_MASK;
    const uint32_t *const *tiles = c->l0->px[v];
    const uint64_t *spill = c->l0->spill[v];
    int col = c0;
    for (; col < c1; col++, text += stride) {
        int sx0 = col * cw;
        uint32_t cp = *text;
        const uint32_t *t = tiles[cp];
        if (!t || (spill[cp >> 6] >> (cp & 63)) & 1 || sx0 < c->x0 || sx0 + cw > c->x1)
            break;
        tile_blit(isa, c->row + sx0, c->stride, t, cw, ch);
    }
    return col;
}

/*
 * The span run callback of the walk: shaped cells with no flag. In the
 * default pair each one is an L0 id lookup and a blit. It stops as
 * text_body does.
 */
static inline RENDER_ALWAYS_INLINE int span_body(int isa, void *ctx, int c0, int c1,
                                                 uint32_t fg, uint32_t bg, const uint64_t *ids,
                                                 run_fill_fn fill)
{
    struct row_ctx *c = ctx;
    if (!c->klass || fg != c->l0_fg || bg != c->l0_bg)
        return c0;
    run_flush(c, fill);
    const int cw = c->cw, ch = c->ch;
    int col = c0;
    for (; col < c1; col++) {
        int sx0 = col * cw;
        struct cell_tile t = cell_l0_find_id(c->l0, ids[col - c0]);
        if (!t.px || t.spill || sx0 < c->x0 || sx0 + cw > c->x1)
            break;
        tile_blit(isa, c->row + sx0, c->stride, t.px, cw, ch);
    }
    return col;
}

/*
 * The walk callback. Plain fills and L0 hits stay here. An L0 hit is a
 * cell inside the window, so it needs no clip check beyond put_tile.
 */
static inline RENDER_ALWAYS_INLINE void cell_body(int isa, void *ctx, int col,
                                                  const struct render_cell *cell,
                                                  const struct render_glyph_ref *ref,
                                                  cell_slow_fn slow, run_fill_fn fill)
{
    struct row_ctx *c = ctx;
    const int sx0 = col * c->cw;
    const uint16_t fl = cell->flags;
    if ((fl & (RENDER_CELL_BLANK | RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)) ==
        RENDER_CELL_BLANK) {
        run_add(c, sx0, sx0 + c->cw, cell->bg, fill);
        return;
    }
    run_flush(c, fill);
    /* L0: a cell in the default pair. ASCII is one load. */
    if (c->klass && !(fl & (RENDER_CELL_PIXEL_MASK | RENDER_CELL_WIDE_TAIL)) && ref->id &&
        cell->fg == c->l0_fg && cell->bg == c->l0_bg && sx0 >= c->x0 && sx0 + c->cw <= c->x1) {
        struct cell_tile t = !(ref->id & ~L0_ID_BITS) ? cell_l0_find(c->l0, cell->text, cell->style)
                                                      : cell_l0_find_id(c->l0, ref->id);
        if (t.px && !t.spill) {
            tile_blit(isa, c->row + sx0, c->stride, t.px, c->cw, c->ch);
            return;
        }
    }
    slow(c, sx0, cell, ref);
}

/* ---- One row ---------------------------------------------------------- */

static bool scratch_room(struct raster_worker *w, size_t px)
{
    if (px <= w->scratch_px)
        return true;
    uint32_t *p = realloc(w->scratch, px * sizeof(*p));
    if (!p)
        return false;
    w->scratch = p;
    w->scratch_px = px;
    return true;
}

static inline RENDER_ALWAYS_INLINE int row_body(int isa, const struct raster_frame *f,
                                                struct raster_worker *w,
                                                const struct raster_job *job,
                                                const struct render_walk_sink *sink,
                                                run_fill_fn fill)
{
    const struct terminal_frame *tf = f->frame;
    const int cw = f->cw;
    if (!tf || cw <= 0 || f->ch <= 0 || f->width <= 0)
        return 0;
    struct row_ctx c = {
        .w = w,
        .row = f->origin + (size_t)job->row * (size_t)f->ch * (size_t)f->stride,
        .stride = (size_t)f->stride,
        .cw = cw,
        .ch = f->ch,
        .x0 = max_i(job->x0, 0),
        .x1 = min_i(job->x1, f->width),
        .area = f->cols * cw,
        .width = f->width,
        .run_x0 = -1,
        .run_x1 = -1,
        .l0_fg = tf->reverse_video ? tf->default_bg : tf->default_fg,
        .l0_bg = tf->reverse_video ? tf->default_fg : tf->default_bg,
        .margin = tf->default_bg,
        .cursor = tf->cursor_color,
        .font_version = f->font_version,
        .klass = f->cache,
        .cache = f->cache ? w->cache : NULL,
        .l0 = cell_class_l0(f->cache),
        .decor = f->decor,
    };
    if (c.area > c.width)
        c.area = c.width;
    if (c.x0 >= c.x1)
        return 0;
    w->spill.n = 0;
    w->spill.used = 0;

    bool full = c.x0 == 0 && c.x1 == c.width;
    int c0 = 0, c1 = f->cols;
    if (!full) {
        int reach = job->reach > 0 ? job->reach : 0;
        int64_t lo = (int64_t)c.x0 - reach, hi = (int64_t)c.x1 + reach;
        c0 = lo <= 0 ? 0 : (int)(lo / cw);
        c1 = (int)((hi + cw - 1) / cw < f->cols ? (hi + cw - 1) / cw : f->cols);
        /* A cut cell composes in the scratch. Without it the cell stays unpainted. */
        (void)scratch_room(w, (size_t)cw * (size_t)f->ch);
    }
    if (c0 < c1) {
        struct render_walk_sink k = *sink;
        k.ctx = &c;
        render_row_walk(f->font, tf, job->row, c0, c1, RENDER_WALK_PEEK, &k);
    }

    if (c.width > c.area)
        run_add(&c, c.area, c.width, c.margin, fill);
    run_flush(&c, fill);
    spill_blend(isa, &c);
    return c.reach;
}

/* ---- Levels ----------------------------------------------------------- */

#define RASTER_LEVEL(sfx, level, target)                                                   \
    target static RENDER_NOINLINE void fill_##sfx(const struct row_ctx *c, int a, int b,    \
                                                  uint32_t bg)                             \
    {                                                                                      \
        run_fill_body(level, c, a, b, bg);                                                 \
    }                                                                                      \
    target static RENDER_NOINLINE void slow_##sfx(struct row_ctx *c, int sx0,              \
                                                  const struct render_cell *cell,          \
                                                  const struct render_glyph_ref *ref)      \
    {                                                                                      \
        cell_slow(level, c, sx0, cell, ref);                                               \
    }                                                                                      \
    target static RENDER_NOINLINE void cell_##sfx(void *ctx, int col,                      \
                                                  const struct render_cell *cell,          \
                                                  const struct render_glyph_ref *ref)      \
    {                                                                                      \
        cell_body(level, ctx, col, cell, ref, slow_##sfx, fill_##sfx);                     \
    }                                                                                      \
    target static RENDER_NOINLINE void run_##sfx(void *ctx, int c0, int c1, uint32_t bg)   \
    {                                                                                      \
        run_body(ctx, c0, c1, bg, fill_##sfx);                                             \
    }                                                                                      \
    target static RENDER_NOINLINE int text_##sfx(void *ctx, int c0, int c1, uint32_t fg,    \
                                                 uint32_t bg, uint16_t style,              \
                                                 const uint32_t *text, int stride)         \
    {                                                                                      \
        return text_body(level, ctx, c0, c1, fg, bg, style, text, stride, fill_##sfx);     \
    }                                                                                      \
    target static RENDER_NOINLINE int span_##sfx(void *ctx, int c0, int c1, uint32_t fg,    \
                                                 uint32_t bg, const uint64_t *ids)         \
    {                                                                                      \
        return span_body(level, ctx, c0, c1, fg, bg, ids, fill_##sfx);                     \
    }                                                                                      \
    target static RENDER_NOINLINE int row_##sfx(const struct raster_frame *f,              \
                                                struct raster_worker *w,                   \
                                                const struct raster_job *job)              \
    {                                                                                      \
        static const struct render_walk_sink sink = {                                      \
            .cell = cell_##sfx, .run = run_##sfx, .text = text_##sfx, .span = span_##sfx,  \
        };                                                                                 \
        return row_body(level, f, w, job, &sink, fill_##sfx);                              \
    }

#define RASTER_NO_TARGET

RASTER_LEVEL(scalar, RENDER_ISA_SCALAR, RASTER_NO_TARGET)
#if defined(RENDER_KERNEL_X86)
RASTER_LEVEL(avx2, RENDER_ISA_AVX2, RENDER_TARGET_AVX2)
RASTER_LEVEL(avx512, RENDER_ISA_AVX512, RENDER_TARGET_AVX512)
#endif
#if defined(RENDER_KERNEL_NEON)
RASTER_LEVEL(neon, RENDER_ISA_NEON, RASTER_NO_TARGET)
#endif

raster_row_fn raster_row_for(int isa)
{
    switch (isa) {
#if defined(RENDER_KERNEL_X86)
    case RENDER_ISA_AVX512:
        return row_avx512;
    case RENDER_ISA_AVX2:
        return row_avx2;
#endif
#if defined(RENDER_KERNEL_NEON)
    case RENDER_ISA_NEON:
        return row_neon;
#endif
    default:
        return row_scalar;
    }
}

bool raster_worker_init(struct raster_worker *w, struct cell_cache *cache)
{
    if (!w)
        return false;
    if (cache) {
        w->cache = cell_cache_worker_create();
        if (!w->cache)
            return false;
    }
    return true;
}

void raster_workers_balance(struct raster_worker *w, int n)
{
    int cap = 0;
    size_t size = 0, scratch = 0;
    for (int i = 0; i < n; i++) {
        cap = max_i(cap, w[i].spill.cap);
        size = w[i].spill.size > size ? w[i].spill.size : size;
        scratch = w[i].scratch_px > scratch ? w[i].scratch_px : scratch;
    }
    for (int i = 0; i < n; i++) {
        (void)spill_grow(&w[i].spill, cap, size);
        (void)scratch_room(&w[i], scratch);
    }
}

void raster_worker_release(struct raster_worker *w)
{
    if (!w)
        return;
    cell_cache_worker_destroy(w->cache);
    free(w->spill.items);
    free(w->spill.cov);
    free(w->scratch);
    w->cache = NULL;
    w->spill = (struct raster_spill_list){0};
    w->scratch = NULL;
    w->scratch_px = 0;
}
