#include "app/thumbnail.h"

#include <stdlib.h>

#include "app/app.h"
#include "font/font.h"
#include "paint/scene.h"
#include "render/render.h"
#include "render/term_frame.h"
#include <wsap_vtcore/wsap_vtcore.h>

struct app_thumbnail {
    struct ui_image image;
    uint32_t *pixels;
    size_t pixels_cap;
    uint32_t *full;
    size_t full_cap;
    struct wsap_vtcore_snapshot *snapshot;
    const struct wsap_vtcore *source;
    uint64_t signature;
};

static bool reserve(uint32_t **buf, size_t *cap, size_t count)
{
    if (count <= *cap)
        return true;
    if (count > SIZE_MAX / sizeof(**buf))
        return false;
    uint32_t *grown = realloc(*buf, count * sizeof(*grown));
    if (!grown)
        return false;
    *buf = grown;
    *cap = count;
    return true;
}

static uint64_t content_signature(const struct wsap_vtcore_snapshot *snapshot,
                                  const struct font_instance *font, int width)
{
    uint64_t h = wsap_vtcore_snapshot_view(snapshot)->version * 0x9E3779B97F4A7C15ull ^
                 (uint64_t)width;
    return (h ^ font_instance_version(font)) * 1099511628211ull;
}

struct sample_range {
    int x0;
    int x1;
};

static void resample(uint32_t *dst, struct vector2i dst_size,
                     const uint32_t *src, struct vector2i src_size)
{
    if (dst_size.x <= 0 || dst_size.y <= 0)
        return;

    struct sample_range stack_ranges[256];
    struct sample_range *ranges = stack_ranges;
    if ((size_t)dst_size.x > sizeof(stack_ranges) / sizeof(stack_ranges[0])) {
        ranges = malloc((size_t)dst_size.x * sizeof(*ranges));
        if (!ranges)
            return;
    }

    for (int x = 0; x < dst_size.x; x++) {
        int x0 = (int)((int64_t)x * src_size.x / dst_size.x);
        int x1 = (int)((int64_t)(x + 1) * src_size.x / dst_size.x);
        if (x1 <= x0)
            x1 = x0 + 1;
        ranges[x] = (struct sample_range){ .x0 = x0, .x1 = x1 };
    }

    for (int y = 0; y < dst_size.y; y++) {
        int y0 = (int)((int64_t)y * src_size.y / dst_size.y);
        int y1 = (int)((int64_t)(y + 1) * src_size.y / dst_size.y);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dst_size.x; x++) {
            int x0 = ranges[x].x0;
            int x1 = ranges[x].x1;
            unsigned red = 0, green = 0, blue = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint32_t *row = src + (size_t)yy * src_size.x;
                for (int xx = x0; xx < x1; xx++) {
                    red += (row[xx] >> 16) & 0xFF;
                    green += (row[xx] >> 8) & 0xFF;
                    blue += row[xx] & 0xFF;
                    n++;
                }
            }
            dst[(size_t)y * dst_size.x + x] = 0xFF000000u |
                ((red / n) << 16) | ((green / n) << 8) | (blue / n);
        }
    }

    if (ranges != stack_ranges)
        free(ranges);
}

bool app_thumbnail_capture(void *ud, void **state, struct font_instance *font,
                           struct wsap_vtcore *term, int width, struct ui_image *image)
{
    struct app *app = ud;
    if (!app || !state || !font || !term || width <= 0 || !image)
        return false;
    struct app_thumbnail *thumb = *state;
    if (!thumb) {
        thumb = calloc(1, sizeof(*thumb));
        if (!thumb)
            return false;
        thumb->snapshot = wsap_vtcore_snapshot_new();
        if (!thumb->snapshot) {
            free(thumb);
            return false;
        }
        *state = thumb;
    }
    if (!wsap_vtcore_snapshot_update(thumb->snapshot, term))
        return false;
    struct terminal_frame grid;
    terminal_frame_init(&grid, thumb->snapshot, false);
    uint64_t signature = content_signature(thumb->snapshot, font, width);
    if (thumb->source == term && thumb->signature == signature && thumb->image.pixels) {
        *image = thumb->image;
        return true;
    }

    const struct font_metrics *metrics = font_instance_metrics(font);
    struct vector2i full_size = {
        .x = grid.cols * metrics->cell_w,
        .y = grid.rows * metrics->cell_h,
    };
    if (full_size.x <= 0 || full_size.y <= 0)
        return false;
    struct vector2i size = {
        .x = full_size.x < width ? full_size.x : width,
        .y = (int)((int64_t)full_size.y *
                   (full_size.x < width ? full_size.x : width) / full_size.x),
    };
    if (size.y <= 0 || (size_t)full_size.x > SIZE_MAX / (size_t)full_size.y ||
        (size_t)size.x > SIZE_MAX / (size_t)size.y ||
        !reserve(&thumb->full, &thumb->full_cap,
                 (size_t)full_size.x * full_size.y) ||
        !reserve(&thumb->pixels, &thumb->pixels_cap, (size_t)size.x * size.y))
        return false;

    struct damage_list damage = {0};
    struct framebuffer target = {
        .pixels = thumb->full,
        .size = full_size,
        .stale = true,
        .damage = &damage,
    };
    struct render_frame frame = {
        .font = font,
        .grid = grid,
        .has_grid = true,
        .repaint = true,
        .gpu_off = true,
    };
    (void)render_draw(app->renderer, &target, &frame);
    resample(thumb->pixels, size, thumb->full, full_size);
    thumb->image = (struct ui_image){
        .pixels = thumb->pixels,
        .size = size,
        .gen = thumb->image.gen + 1,
    };
    thumb->source = term;
    thumb->signature = signature;
    *image = thumb->image;
    return true;
}

void app_thumbnail_release(void *state)
{
    struct app_thumbnail *thumb = state;
    if (!thumb)
        return;
    free(thumb->pixels);
    free(thumb->full);
    wsap_vtcore_snapshot_free(thumb->snapshot);
    free(thumb);
}
