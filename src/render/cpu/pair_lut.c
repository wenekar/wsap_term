#include "render/cpu/pair_lut.h"

#include <stddef.h>

/* The slot of a pair. A direct map: the hash picks one slot. */
static unsigned pair_slot(uint32_t fg, uint32_t bg, bool faint)
{
    uint64_t tag = ((uint64_t)fg << 32) | bg;
    tag ^= faint ? UINT64_C(0xA5A5A5A5DEADBEEF) : 0;
    return (unsigned)((tag * UINT64_C(0x9E3779B97F4A7C15)) >> 60) & (PAIR_LUT_SLOTS - 1);
}

static bool tag_is(const struct pair_lut_tag *t, uint32_t fg, uint32_t bg, bool faint)
{
    return t->used && t->fg == fg && t->bg == bg && t->faint == faint;
}

/* The coverage after the faint rule. */
static inline uint8_t faint_cov(unsigned c, bool faint)
{
    return (uint8_t)(faint ? (c + 1) >> 1 : c);
}

const uint32_t *pair_lut_alpha(struct pair_lut *c, uint32_t fg, uint32_t bg, bool faint)
{
    struct pair_lut_alpha_slot *s = &c->alpha[pair_slot(fg, bg, faint)];
    if (tag_is(&s->tag, fg, bg, faint))
        return s->lut;
    if (!tag_is(&s->pending, fg, bg, faint)) {
        s->pending = (struct pair_lut_tag){.fg = fg, .bg = bg, .faint = faint, .used = true};
        return NULL;
    }
    int fg_l = (int)blend_lum16(fg), bg_l = (int)blend_lum16(bg);
    for (unsigned a = 0; a < 256; a++)
        s->lut[a] = blend_px_lum(fg, bg, fg_l, bg_l, faint_cov(a, faint));
    s->tag = s->pending;
    s->pending = (struct pair_lut_tag){0};
    return s->lut;
}

/* Remap one channel table so that entry c holds the entry of (c + 1) / 2. */
static void faint_channel(uint32_t *t)
{
    /* Go down: the source index is never above c, and it is still intact. */
    for (unsigned c = 255; c > 0; c--)
        t[c] = t[(c + 1) >> 1];
}

const struct subpx_lut *pair_lut_subpx(struct pair_lut *c, uint32_t fg, uint32_t bg, bool faint)
{
    struct pair_lut_subpx_slot *s = &c->subpx[pair_slot(fg, bg, faint)];
    if (tag_is(&s->tag, fg, bg, faint))
        return &s->lut;
    if (!tag_is(&s->pending, fg, bg, faint)) {
        s->pending = (struct pair_lut_tag){.fg = fg, .bg = bg, .faint = faint, .used = true};
        return NULL;
    }
    blend_build_subpx_lut(&s->lut, fg, bg);
    if (faint) {
        faint_channel(s->lut.r);
        faint_channel(s->lut.g);
        faint_channel(s->lut.b);
    }
    s->tag = s->pending;
    s->pending = (struct pair_lut_tag){0};
    return &s->lut;
}
