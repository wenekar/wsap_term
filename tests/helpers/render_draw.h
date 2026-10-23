#ifndef WSAP_TEST_RENDER_DRAW_H
#define WSAP_TEST_RENDER_DRAW_H

/*
 * Helpers for the tests of the new renderer. They go through
 * render_draw() only: real terminals in, pixels and damage out.
 *
 * - struct tterm: a terminal and the snapshot that its frames point into.
 * - struct tcanvas: a persistent canvas and its framebuffer history.
 * - tdraw_fresh(): the oracle. It paints a frame with no history, so the
 *   renderer keeps no state and paints every pixel.
 * - tcheck_same(), tcheck_damage(): pixel and damage checks that print the
 *   first difference.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "paint/damage.h"
#include "paint/scene.h"
#include "render/render.h"
#include "term_api.h"

#define TCHECK(cond, ...)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s: ", __FILE__, __LINE__, \
                    #cond);                                                  \
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
            abort();                                                         \
        }                                                                    \
    } while (0)

static inline struct rect2i trect(int x, int y, int w, int h)
{
    return (struct rect2i){ .pos = { .x = x, .y = y }, .size = { .x = w, .y = h } };
}

/* ---- Terminals ---------------------------------------------------------- */

struct tterm {
    struct wsap_vtcore *t;
    struct wsap_vtcore_snapshot *snap;
    struct terminal_frame f;
};

static inline void tterm_open(struct tterm *tt, int cols, int rows)
{
    *tt = (struct tterm){ .t = tt_new(cols, rows), .snap = wsap_vtcore_snapshot_new() };
    TCHECK(tt->t && tt->snap, "terminal %dx%d", cols, rows);
}

static inline void tterm_close(struct tterm *tt)
{
    wsap_vtcore_snapshot_free(tt->snap);
    wsap_vtcore_free(tt->t);
    *tt = (struct tterm){0};
}

static inline void tterm_feed(struct tterm *tt, const char *s)
{
    tt_puts(tt->t, s);
}

/* Takes a new snapshot. The frame points into it until the next call. */
static inline const struct terminal_frame *tterm_frame(struct tterm *tt, bool cursor)
{
    TCHECK(wsap_vtcore_snapshot_update(tt->snap, tt->t), "snapshot update");
    terminal_frame_init(&tt->f, tt->snap, cursor);
    return &tt->f;
}

/* ---- Canvases ----------------------------------------------------------- */

struct tcanvas {
    uint32_t *px;
    struct vector2i size;
    struct framebuffer_history history;
    bool fresh;
};

static inline void tcanvas_init(struct tcanvas *c, int w, int h)
{
    *c = (struct tcanvas){
        .px = calloc((size_t)w * (size_t)h, sizeof(uint32_t)),
        .size = { .x = w, .y = h },
        .fresh = true,
    };
    TCHECK(c->px, "canvas %dx%d", w, h);
}

/* A new size gets new pixels. The first frame after it is fresh. */
static inline void tcanvas_resize(struct tcanvas *c, int w, int h)
{
    free(c->px);
    c->px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    TCHECK(c->px, "canvas %dx%d", w, h);
    c->size = (struct vector2i){ .x = w, .y = h };
    c->fresh = true;
}

static inline void tcanvas_free(struct tcanvas *c)
{
    framebuffer_history_release(&c->history);
    free(c->px);
    *c = (struct tcanvas){0};
}

static inline size_t tcanvas_px(const struct tcanvas *c)
{
    return (size_t)c->size.x * (size_t)c->size.y;
}

static inline struct render_result tdraw(struct renderer *r, struct tcanvas *c,
                                         const struct render_frame *frame)
{
    struct framebuffer fb = {
        .pixels = c->px, .size = c->size, .fresh = c->fresh, .history = &c->history,
    };
    c->fresh = false;
    return render_draw(r, &fb, frame);
}

/* The oracle: a paint with no history. */
static inline struct render_result tdraw_fresh(struct renderer *r, uint32_t *px,
                                               struct vector2i size,
                                               const struct render_frame *frame)
{
    struct framebuffer fb = { .pixels = px, .size = size };
    return render_draw(r, &fb, frame);
}

/* A grid frame that covers the whole canvas. */
static inline struct render_frame tframe(const struct terminal_frame *f,
                                         struct font_instance *font, struct vector2i size)
{
    static const struct ui_damage none = {0};
    return (struct render_frame){
        .base = 0xFF202020u,
        .grid = { .frame = f, .font = font, .rect = { .size = size } },
        .ui_font = font,
        .ui_damage = &none,
    };
}

/* ---- Checks ------------------------------------------------------------- */

static inline long tcount_diff(const uint32_t *a, const uint32_t *b, size_t n)
{
    long d = 0;
    for (size_t i = 0; i < n; i++)
        d += a[i] != b[i];
    return d;
}

/* Fails at the first pixel where got and want differ. */
static inline void tcheck_same(const char *what, const uint32_t *got, const uint32_t *want,
                               struct vector2i size)
{
    size_t n = (size_t)size.x * (size_t)size.y;
    for (size_t i = 0; i < n; i++) {
        if (got[i] == want[i])
            continue;
        fprintf(stderr, "%s: pixel (%d, %d) is %08x, want %08x; %ld pixels differ\n", what,
                (int)(i % (size_t)size.x), (int)(i / (size_t)size.x), got[i], want[i],
                tcount_diff(got, want, n));
        abort();
    }
}

static inline bool tdamage_has(const struct damage_list *d, struct vector2i p)
{
    if (d->all)
        return true;
    if (d->n < 0)
        return rect2i_contains(d->bounds, p);
    for (int i = 0; i < d->n; i++)
        if (rect2i_contains(d->rects[i], p))
            return true;
    return false;
}

/* Fails when a pixel changed from `before` to `after` outside the damage. */
static inline void tcheck_damage(const char *what, const uint32_t *before, const uint32_t *after,
                                 struct vector2i size, const struct damage_list *d)
{
    size_t n = (size_t)size.x * (size_t)size.y;
    for (size_t i = 0; i < n; i++) {
        if (before[i] == after[i])
            continue;
        struct vector2i p = { .x = (int)(i % (size_t)size.x), .y = (int)(i / (size_t)size.x) };
        if (tdamage_has(d, p))
            continue;
        fprintf(stderr, "%s: pixel (%d, %d) changed %08x -> %08x outside the damage "
                        "(any %d all %d n %d)\n",
                what, p.x, p.y, before[i], after[i], d->any, d->all, d->n);
        abort();
    }
}

/* The number of damaged pixels, for size checks. */
static inline long tdamage_area(const struct damage_list *d, struct vector2i size)
{
    long n = 0;
    for (int y = 0; y < size.y; y++)
        for (int x = 0; x < size.x; x++)
            n += tdamage_has(d, (struct vector2i){ .x = x, .y = y });
    return n;
}

static inline uint64_t thash(const uint32_t *px, size_t n)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; i++)
        h = (h ^ px[i]) * UINT64_C(1099511628211);
    return h;
}

#endif
