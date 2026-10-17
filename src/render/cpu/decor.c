#include "render/cpu/decor.h"

#include <stdlib.h>
#include <string.h>

#define DECOR_SETS 4
/* Planes in one set: the zero plane, and a full and a faint plane for each mask. */
#define DECOR_MASKS (DECOR_UNDERLINE_KINDS - 1 + 4)
#define DECOR_PLANES (1 + 2 * DECOR_MASKS)

/* Antialiasing grid: samples per pixel on each axis. */
#define SS 16
#define SS_AREA (SS * SS)

struct decor_entry {
    struct decor_set set;
    uint8_t *mem;
    size_t mem_size;
    uint64_t stamp;
    bool used;
};

struct decor_cache {
    struct decor_entry e[DECOR_SETS];
    uint64_t clock;
};

struct decor_key decor_key_of(const struct font_metrics *m)
{
    int t = (m->pixel_size * 3 + 32) / 64;
    return (struct decor_key){
        .cw = m->cell_w, .ch = m->cell_h, .baseline = m->baseline,
        .thickness = t < 1 ? 1 : t,
    };
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The geometry of one key, in whole pixel rows. */
struct decor_geom {
    int cw, ch, t;
    int u;          /* The first row of the single underline. */
};

static void rows_fill(uint8_t *cov, const struct decor_geom *g, int y0, int y1,
                      int x0, int x1)
{
    y0 = clampi(y0, 0, g->ch);
    y1 = clampi(y1, 0, g->ch);
    x0 = clampi(x0, 0, g->cw);
    x1 = clampi(x1, 0, g->cw);
    for (int y = y0; y < y1; y++)
        memset(cov + (size_t)y * g->cw + x0, 255, (size_t)(x1 - x0));
}

/*
 * The phase of the sample at sub-column sx, in Q16 of one period. A cell
 * holds k whole periods, so the pattern restarts at each cell edge without
 * a seam. The sample sits at the middle of its sub-column.
 */
static unsigned phase_q16(const struct decor_geom *g, int k, int sx)
{
    int64_t w2 = 2 * (int64_t)g->cw * SS;
    int64_t xs = 2 * (int64_t)sx + 1;
    return (unsigned)(((xs * k) % w2) * 65536 / w2);
}

/* sin(2 pi p) for p in Q16, as a parabola. The result is in [-65536, 65536]. */
static int sine_q16(unsigned p)
{
    int64_t h = 32768;
    if (p < h)
        return (int)((int64_t)p * (h - (int64_t)p) * 16 / 65536);
    int64_t q = (int64_t)p - h;
    return -(int)(q * (65536 - (int64_t)p) * 16 / 65536);
}

enum shape { SHAPE_CURLY, SHAPE_DOTTED, SHAPE_DASHED };

/* How many periods one cell holds. The period is about `target` pixels. */
static int periods(const struct decor_geom *g, int target_x4)
{
    int k = (g->cw * 4 + target_x4 / 2) / target_x4;
    return k < 1 ? 1 : k;
}

static void shape_fill(uint8_t *cov, const struct decor_geom *g, enum shape shape)
{
    int cw = g->cw, ch = g->ch, t = g->t;
    /* All centers and radii are in 1/SS pixel. */
    int amp = t < 1 ? 1 : t;
    int yc = g->u * SS + t * SS / 2;
    int k;
    switch (shape) {
    case SHAPE_CURLY:
        /* The wave must stay inside the cell: move its center to fit. */
        k = periods(g, 4 * (4 * t + 2));
        int lo = amp * SS + t * SS / 2;
        int hi = ch * SS - lo;
        yc = lo > hi ? ch * SS / 2 : clampi(yc, lo, hi);
        break;
    case SHAPE_DOTTED:
        k = periods(g, 4 * (2 * t + 1));
        break;
    default:
        k = periods(g, 4 * (4 * t + 1));
        break;
    }
    /* The rows that can hold ink. */
    int ya = 0, yb = ch;
    if (shape != SHAPE_CURLY) {
        ya = g->u;
        yb = g->u + t;
    }
    /* count[x] for the pixel columns of one row, built over sub-columns. */
    for (int y = ya; y < yb; y++) {
        for (int x = 0; x < cw; x++) {
            unsigned count = 0;
            for (int sx = x * SS; sx < (x + 1) * SS; sx++) {
                unsigned p = phase_q16(g, k, sx);
                int cy = yc;
                if (shape == SHAPE_CURLY)
                    cy += (int)((int64_t)amp * SS * sine_q16(p) / 65536);
                for (int sy = y * SS; sy < (y + 1) * SS; sy++) {
                    int64_t dy2 = 2 * (int64_t)sy + 1 - 2 * (int64_t)cy;
                    int64_t dxn = (int64_t)p - 32768;
                    bool in;
                    if (shape == SHAPE_CURLY) {
                        in = dy2 >= -t * SS && dy2 < t * SS;
                    } else if (shape == SHAPE_DOTTED) {
                        /* An ellipse: half width = period / 4, half height = t / 2. */
                        int64_t b2 = (int64_t)t * SS;
                        in = (4 * dxn) * (4 * dxn) * b2 * b2 +
                             (dy2 * 65536) * (dy2 * 65536) <= (65536 * b2) * (65536 * b2);
                    } else {
                        /* A dash covers two thirds of the period. */
                        in = (dxn < 0 ? -dxn : dxn) * 3 < 65536 && dy2 >= -t * SS && dy2 < t * SS;
                    }
                    count += in;
                }
            }
            cov[(size_t)y * cw + x] = (uint8_t)((count * 255 + SS_AREA / 2) / SS_AREA);
        }
    }
}

/* The row range and the solid flag of a finished plane. */
static void mask_scan(struct decor_mask *m, const uint8_t *cov, int cw, int ch)
{
    int y0 = ch, y1 = 0;
    bool solid = true;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            uint8_t v = cov[(size_t)y * cw + x];
            if (!v)
                continue;
            if (y < y0)
                y0 = y;
            y1 = y + 1;
            break;
        }
    }
    if (y1 <= y0)
        return;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < cw; x++)
            solid &= cov[(size_t)y * cw + x] == 255;
    m->y0 = y0;
    m->y1 = y1;
    m->solid = solid;
}

/* Fill the planes of one set. `mem` holds DECOR_PLANES planes of cw * ch. */
static void set_build(struct decor_set *s, uint8_t *mem, struct decor_key key)
{
    size_t plane = (size_t)key.cw * (size_t)key.ch;
    memset(mem, 0, plane * DECOR_PLANES);
    struct decor_geom g = {.cw = key.cw, .ch = key.ch,
                           .t = clampi(key.thickness, 1, key.ch)};
    int t = g.t, ch = g.ch, cw = g.cw;
    g.u = clampi(key.baseline + (ch - key.baseline) / 2, 0, ch - t);

    /* Plane 0 is zero. Mask i uses planes 1 + 2i (full) and 2 + 2i (faint). */
    struct decor_mask *masks[DECOR_MASKS];
    int n = 0;
    masks[n++] = &s->underline[WSAP_VTCORE_UNDERLINE_SINGLE];
    masks[n++] = &s->underline[WSAP_VTCORE_UNDERLINE_DOUBLE];
    masks[n++] = &s->underline[WSAP_VTCORE_UNDERLINE_CURLY];
    masks[n++] = &s->underline[WSAP_VTCORE_UNDERLINE_DOTTED];
    masks[n++] = &s->underline[WSAP_VTCORE_UNDERLINE_DASHED];
    masks[n++] = &s->strike;
    masks[n++] = &s->overline;
    masks[n++] = &s->cursor_bar;
    masks[n++] = &s->cursor_underline;

    for (int i = 0; i < DECOR_UNDERLINE_KINDS; i++)
        s->underline[i] = (struct decor_mask){.cov = mem, .faint = mem};
    s->strike = s->overline = s->cursor_bar = s->cursor_underline =
        (struct decor_mask){.cov = mem, .faint = mem};

    for (int i = 0; i < n; i++) {
        masks[i]->cov = mem + (size_t)(1 + 2 * i) * plane;
        masks[i]->faint = mem + (size_t)(2 + 2 * i) * plane;
    }
    struct decor_mask *const *mk = masks;
    uint8_t *c;

    c = (uint8_t *)mk[0]->cov;
    rows_fill(c, &g, g.u, g.u + t, 0, cw);
    c = (uint8_t *)mk[1]->cov;
    int e = g.u + 2 * t < ch ? g.u + 2 * t : ch;
    rows_fill(c, &g, e - 3 * t, e - 2 * t, 0, cw);
    rows_fill(c, &g, e - t, e, 0, cw);
    shape_fill((uint8_t *)mk[2]->cov, &g, SHAPE_CURLY);
    shape_fill((uint8_t *)mk[3]->cov, &g, SHAPE_DOTTED);
    shape_fill((uint8_t *)mk[4]->cov, &g, SHAPE_DASHED);
    int sy = (ch - t + 1) / 2;
    rows_fill((uint8_t *)mk[5]->cov, &g, sy, sy + t, 0, cw);
    int oy = ch - t < 1 ? ch - t : 1;
    rows_fill((uint8_t *)mk[6]->cov, &g, oy, oy + t, 0, cw);
    rows_fill((uint8_t *)mk[7]->cov, &g, 0, ch, 0, 2 * t);
    int cu = ch < 2 * t ? ch : 2 * t;
    rows_fill((uint8_t *)mk[8]->cov, &g, ch - cu, ch, 0, cw);

    for (int i = 0; i < n; i++) {
        const uint8_t *full = masks[i]->cov;
        uint8_t *faint = (uint8_t *)masks[i]->faint;
        for (size_t j = 0; j < plane; j++)
            faint[j] = (uint8_t)((full[j] + 1) >> 1);
        mask_scan(masks[i], full, cw, ch);
    }
    s->key = key;
}

void decor_cache_destroy(struct decor_cache *c)
{
    if (!c)
        return;
    for (int i = 0; i < DECOR_SETS; i++)
        free(c->e[i].mem);
    free(c);
}

struct decor_cache *decor_cache_create(void)
{
    return calloc(1, sizeof(struct decor_cache));
}

const struct decor_set *decor_get(struct decor_cache *c, const struct font_metrics *m)
{
    struct decor_key key = decor_key_of(m);
    if (!c || key.cw <= 0 || key.ch <= 0)
        return NULL;
    struct decor_entry *victim = NULL;
    for (int i = 0; i < DECOR_SETS; i++) {
        struct decor_entry *e = &c->e[i];
        if (e->used && !memcmp(&e->set.key, &key, sizeof(key))) {
            e->stamp = ++c->clock;
            return &e->set;
        }
        if (!victim || !e->used < !victim->used ||
            (e->used == victim->used && e->stamp < victim->stamp))
            victim = e;
    }
    size_t need = (size_t)key.cw * (size_t)key.ch * DECOR_PLANES;
    victim->used = false;
    if (victim->mem_size < need) {
        uint8_t *mem = realloc(victim->mem, need);
        if (!mem)
            return NULL;
        victim->mem = mem;
        victim->mem_size = need;
    }
    memset(&victim->set, 0, sizeof(victim->set));
    set_build(&victim->set, victim->mem, key);
    victim->used = true;
    victim->stamp = ++c->clock;
    return &victim->set;
}
