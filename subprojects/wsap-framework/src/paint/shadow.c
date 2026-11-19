#include "paint/shadow.h"
#include "paint/shape_math.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#define UI_SHADOW_CACHE_SLOTS 4

#define UI_SHADOW_TAU 6.283185307179586

struct shadow_slot {
    struct ui_shadow_mask mask;
    uint8_t *tile;
    uint8_t edge[2 * UI_SHADOW_MAX_EXTENT + 1];
    bool ready;
};

struct shadow_cache {
    struct shadow_slot slots[UI_SHADOW_CACHE_SLOTS];
    int next;
};

static struct shadow_cache shadow_legacy;
static __thread struct shadow_cache shadow_shape;

static double shadow_phi(double z)
{
    return 0.5 * erfc(-z * 0.70710678118654752440);
}

static uint8_t shadow_quantize(double v)
{
    if (!(v > 0.0))
        return 0;
    if (v >= 1.0)
        return 255;
    return (uint8_t)(v * 255.0 + 0.5);
}

static uint8_t *shadow_build_tile(int radius, int extent, double sigma)
{
    int c = radius + 2 * extent;
    int n = radius * 4 < 32 ? 32 : radius * 4;
    uint8_t *tile = malloc((size_t)c * (size_t)c);
    float *rows = malloc((size_t)c * (size_t)n * sizeof(*rows));
    float *cols = malloc((size_t)c * (size_t)n * sizeof(*cols));
    if (!tile || !rows || !cols) {
        free(tile);
        free(rows);
        free(cols);
        return NULL;
    }

    double step = (double)radius / (double)n;
    double norm = 1.0 / (sigma * sqrt(UI_SHADOW_TAU));
    for (int i = 0; i < c; i++) {
        double p = (double)(i - extent) + 0.5;
        for (int k = 0; k < n; k++) {
            double v = ((double)k + 0.5) * step;
            double dv = (double)radius - v;
            double l = (double)radius -
                       sqrt((double)radius * radius - dv * dv);
            double t = (p - v) / sigma;
            rows[(size_t)i * n + k] = (float)(norm * exp(-0.5 * t * t) * step);
            cols[(size_t)i * n + k] = (float)shadow_phi((p - l) / sigma);
        }
    }

    for (int dy = 0; dy < c; dy++) {
        double y = (double)(dy - extent) + 0.5;
        double tail = shadow_phi((y - (double)radius) / sigma);
        const float *rv = rows + (size_t)dy * n;
        for (int dx = 0; dx < c; dx++) {
            const float *cv = cols + (size_t)dx * n;
            float sum = 0.0f;
            for (int k = 0; k < n; k++)
                sum += rv[k] * cv[k];
            double x = (double)(dx - extent) + 0.5;
            tile[(size_t)dy * c + dx] =
                shadow_quantize((double)sum + shadow_phi(x / sigma) * tail);
        }
    }
    free(rows);
    free(cols);
    return tile;
}

static uint8_t *shadow_build_tile_shape(int radius, int extent, float sigma)
{
    int c = radius + 2 * extent;
    int n = radius * 4 < 32 ? 32 : radius * 4;
    float r = (float)radius;
    uint8_t *tile = malloc((size_t)c * (size_t)c);
    float *rows = malloc((size_t)c * (size_t)n * sizeof(*rows));
    float *cols = malloc((size_t)c * (size_t)n * sizeof(*cols));
    if (!tile || !rows || !cols) {
        free(tile);
        free(rows);
        free(cols);
        return NULL;
    }
    for (int i = 0; i < c; i++) {
        float p = (float)(i - extent) + 0.5f;
        for (int k = 0; k < n; k++) {
            float v = shape_shadow_slice(r, k, n);
            rows[(size_t)i * n + k] = shape_shadow_row_term(p, v, r, n, sigma);
            cols[(size_t)i * n + k] = shape_shadow_col_term(p, v, r, sigma);
        }
    }
    for (int dy = 0; dy < c; dy++) {
        float y = (float)(dy - extent) + 0.5f;
        const float *rv = rows + (size_t)dy * n;
        for (int dx = 0; dx < c; dx++) {
            const float *cv = cols + (size_t)dx * n;
            float sum = 0.0f;
            for (int k = 0; k < n; k++)
                sum += rv[k] * cv[k];
            float x = (float)(dx - extent) + 0.5f;
            tile[(size_t)dy * c + dx] =
                (uint8_t)shape_shadow_tile(sum, x, y, r, sigma);
        }
    }
    free(rows);
    free(cols);
    return tile;
}

static const struct ui_shadow_mask *shadow_lookup(struct shadow_cache *cache,
                                                  int radius, int blur,
                                                  bool shape)
{
    if (blur <= 0)
        return NULL;
    if (blur > UI_SHADOW_MAX_BLUR)
        blur = UI_SHADOW_MAX_BLUR;
    if (radius < 0)
        radius = 0;
    if (radius > UI_SHADOW_MAX_RADIUS)
        radius = UI_SHADOW_MAX_RADIUS;

    for (int i = 0; i < UI_SHADOW_CACHE_SLOTS; i++) {
        struct shadow_slot *hit = &cache->slots[i];
        if (hit->ready && hit->mask.radius == radius && hit->mask.blur == blur)
            return &hit->mask;
    }

    struct shadow_slot *s = &cache->slots[cache->next];
    cache->next = (cache->next + 1) % UI_SHADOW_CACHE_SLOTS;
    free(s->tile);
    s->tile = NULL;
    s->ready = false;

    int extent = ui_shadow_extent(blur);
    double sigma = (double)blur * 0.5;
    if (shape) {
        for (int i = 0; i <= 2 * extent; i++)
            s->edge[i] = (uint8_t)shape_shadow_edge(i - extent, (float)sigma);
        if (radius > 0)
            s->tile = shadow_build_tile_shape(radius, extent, (float)sigma);
    } else {
        for (int i = 0; i <= 2 * extent; i++)
            s->edge[i] = shadow_quantize(
                shadow_phi(((double)(i - extent) + 0.5) / sigma));
        if (radius > 0)
            s->tile = shadow_build_tile(radius, extent, sigma);
    }

    s->mask = (struct ui_shadow_mask){
        .radius = radius,
        .blur = blur,
        .extent = extent,
        .corner = radius + 2 * extent,
        .tile = s->tile,
        .edge = s->edge,
    };
    s->ready = true;
    return &s->mask;
}

const struct ui_shadow_mask *ui_shadow_mask(int radius, int blur)
{
    return shadow_lookup(&shadow_legacy, radius, blur, false);
}

const struct ui_shadow_mask *ui_shadow_mask_shape(int radius, int blur)
{
    return shadow_lookup(&shadow_shape, radius, blur, true);
}

void ui_shadow_cache_destroy(void)
{
    struct shadow_cache *caches[2] = { &shadow_legacy, &shadow_shape };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < UI_SHADOW_CACHE_SLOTS; i++) {
            free(caches[c]->slots[i].tile);
            caches[c]->slots[i] = (struct shadow_slot){0};
        }
        caches[c]->next = 0;
    }
}
