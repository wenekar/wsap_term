#include "paint/shadow.h"

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

static struct shadow_slot shadow_cache[UI_SHADOW_CACHE_SLOTS];
static int shadow_next;

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

const struct ui_shadow_mask *ui_shadow_mask(int radius, int blur)
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
        struct shadow_slot *hit = &shadow_cache[i];
        if (hit->ready && hit->mask.radius == radius && hit->mask.blur == blur)
            return &hit->mask;
    }

    struct shadow_slot *s = &shadow_cache[shadow_next];
    shadow_next = (shadow_next + 1) % UI_SHADOW_CACHE_SLOTS;
    free(s->tile);
    s->tile = NULL;
    s->ready = false;

    int extent = ui_shadow_extent(blur);
    double sigma = (double)blur * 0.5;
    for (int i = 0; i <= 2 * extent; i++)
        s->edge[i] = shadow_quantize(
            shadow_phi(((double)(i - extent) + 0.5) / sigma));
    if (radius > 0)
        s->tile = shadow_build_tile(radius, extent, sigma);

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

void ui_shadow_cache_destroy(void)
{
    for (int i = 0; i < UI_SHADOW_CACHE_SLOTS; i++) {
        free(shadow_cache[i].tile);
        shadow_cache[i] = (struct shadow_slot){0};
    }
    shadow_next = 0;
}
