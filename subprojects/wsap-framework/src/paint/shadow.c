#include "paint/shadow.h"
#include "paint/shape_math.h"

#include <stdbool.h>
#include <stdlib.h>

#define UI_SHADOW_CACHE_SLOTS 4

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

static __thread struct shadow_cache shadow_shape;

static uint8_t *shadow_build_tile_shape(int radius, int extent, float sigma)
{
    int c = radius + 2 * extent;
    int n = geom_max_i(radius * 4, 32);
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

const struct ui_shadow_mask *ui_shadow_mask_shape(int radius, int blur)
{
    if (blur <= 0)
        return NULL;
    blur = geom_min_i(blur, UI_SHADOW_MAX_BLUR);
    radius = geom_clamp_i(radius, 0, UI_SHADOW_MAX_RADIUS);

    for (int i = 0; i < UI_SHADOW_CACHE_SLOTS; i++) {
        struct shadow_slot *hit = &shadow_shape.slots[i];
        if (hit->ready && hit->mask.radius == radius && hit->mask.blur == blur)
            return &hit->mask;
    }

    struct shadow_slot *s = &shadow_shape.slots[shadow_shape.next];
    shadow_shape.next = (shadow_shape.next + 1) % UI_SHADOW_CACHE_SLOTS;
    free(s->tile);
    s->tile = NULL;
    s->ready = false;

    int extent = ui_shadow_extent(blur);
    float sigma = (float)blur * 0.5f;
    for (int i = 0; i <= 2 * extent; i++)
        s->edge[i] = (uint8_t)shape_shadow_edge(i - extent, sigma);
    if (radius > 0)
        s->tile = shadow_build_tile_shape(radius, extent, sigma);

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
        free(shadow_shape.slots[i].tile);
        shadow_shape.slots[i] = (struct shadow_slot){0};
    }
    shadow_shape.next = 0;
}
