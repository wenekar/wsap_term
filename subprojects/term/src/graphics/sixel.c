#include "graphics.h"
#include "core.h"
#include "screen.h"
#include <stdlib.h>
#include <string.h>

#define SIXEL_MAX_COLUMNS (WSAP_VTCORE_GRAPHICS_BYTES_MAX / 4)

struct sixel_color { uint8_t r, g, b, a; };

struct sixel_scan {
    int width, height;
    struct wsap_vtcore_vector2i aspect;
    struct sixel_color *pixels;
};

static struct sixel_color sixel_argb(uint32_t argb)
{
    return (struct sixel_color){(uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb, 255};
}

static uint8_t sixel_percent(int n)
{
    return (uint8_t)((255 * n + 50) / 100);
}

static struct sixel_color sixel_hls(int hue, int light, int saturation)
{
    hue = (hue + 240) % 360;
    int half = light <= 50 ? light : 100 - light;
    int chroma = 2 * half * saturation;
    int ramp = hue % 120;
    int second = chroma * (ramp <= 60 ? ramp : 120 - ramp) / 60;
    int base = light * 100 - chroma / 2;
    int rgb[6][3] = {
        {chroma, second, 0}, {second, chroma, 0}, {0, chroma, second},
        {0, second, chroma}, {second, 0, chroma}, {chroma, 0, second},
    };
    const int *p = rgb[hue / 60];
    return (struct sixel_color){
        (uint8_t)(((base + p[0]) * 255 + 5000) / 10000),
        (uint8_t)(((base + p[1]) * 255 + 5000) / 10000),
        (uint8_t)(((base + p[2]) * 255 + 5000) / 10000), 255,
    };
}

static bool sixel_number(const char **p, const char *end, int *value)
{
    int n = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        n = n * 10 + (*(*p)++ - '0');
        if (n > WSAP_VTCORE_GRAPHICS_DIM_MAX)
            return false;
    }
    *value = n;
    return true;
}

static int sixel_params(const char **p, const char *end, int *values, int cap)
{
    int n = 0;
    do {
        if (n == cap || !sixel_number(p, end, &values[n]))
            return -1;
        n++;
        if (*p == end || **p != ';') break;
        (*p)++;
    } while (true);
    return n;
}

/* First measure and validate the image, then paint into its final stride. */
static bool sixel_scan(const struct wsap_vtcore *t, struct sixel_scan *s,
                       const char *p, const char *end)
{
    struct sixel_color palette[256];
    struct sixel_color color = {0};
    if (s->pixels) {
        for (int i = 0; i < 256; i++) palette[i] = sixel_argb(t->palette.colors[i]);
        color = sixel_argb(t->palette.colors[WSAP_VTCORE_PALETTE_FG]);
    }
    int x = 0, y = 0;
    unsigned budget = SIXEL_MAX_COLUMNS;
    while (p < end) {
        unsigned ch = (unsigned char)*p++;
        int repeat = 1;
        switch (ch) {
        case ' ': case '\t': case '\r': case '\n': case 0x7f:
            continue;
        case '$': x = 0; continue;
        case '-':
            x = 0;
            y += 6;
            if (y > WSAP_VTCORE_GRAPHICS_DIM_MAX) return false;
            continue;
        case '"': {
            int v[4] = {0};
            if (sixel_params(&p, end, v, 4) < 0 || v[0] == 0 || v[1] == 0)
                return false;
            s->aspect = (struct wsap_vtcore_vector2i){v[1], v[0]};
            if (!s->pixels) {
                if (v[2] > s->width) s->width = v[2];
                if (v[3] > s->height) s->height = v[3];
            }
            continue;
        }
        case '#': {
            int v[5] = {0};
            int count = sixel_params(&p, end, v, 5);
            if ((count != 1 && count != 5) || v[0] >= 256) return false;
            if (count == 5) {
                if (v[3] > 100 || v[4] > 100) return false;
                if (v[1] == 1 && v[2] <= 360) {
                    if (s->pixels) palette[v[0]] = sixel_hls(v[2], v[3], v[4]);
                } else if (v[1] == 2 && v[2] <= 100) {
                    if (s->pixels)
                        palette[v[0]] = (struct sixel_color){sixel_percent(v[2]),
                            sixel_percent(v[3]), sixel_percent(v[4]), 255};
                } else {
                    return false;
                }
            }
            if (s->pixels) color = palette[v[0]];
            continue;
        }
        case '!':
            if (!sixel_number(&p, end, &repeat) || !repeat || p == end) return false;
            ch = (unsigned char)*p++;
            break;
        default: break;
        }
        if (ch < '?' || ch > '~' || repeat > WSAP_VTCORE_GRAPHICS_DIM_MAX - x ||
            (unsigned)repeat > budget)
            return false;
        budget -= (unsigned)repeat;
        int stop = x + repeat;
        unsigned bits = ch - '?';
        int height = y + 1;
        for (unsigned b = bits >> 1; b; b >>= 1) height++;
        if (height > WSAP_VTCORE_GRAPHICS_DIM_MAX) return false;
        if (!s->pixels) {
            if (stop > s->width) s->width = stop;
            if (height > s->height) s->height = height;
        } else {
            if (stop > s->width || height > s->height) return false;
            for (int row = y; bits; row++, bits >>= 1) {
                if (!(bits & 1)) continue;
                struct sixel_color *dest = s->pixels + (size_t)row * s->width;
                for (int col = x; col < stop; col++) dest[col] = color;
            }
        }
        x = stop;
    }
    return true;
}

/* Returns the SIXEL introducer, not an intermediate followed by q. */
static const char *sixel_body(const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == ';')) i++;
    return i < len && buf[i] == 'q' ? buf + i : NULL;
}

bool wsap_vtcore_sixel_header(const char *buf, size_t len)
{
    /* A valid header with three bounded numbers fits in 16 bytes. */
    return sixel_body(buf, len < 16 ? len : 16) != NULL;
}

/* Make room for the bitmap and the following cursor row before the
 * receiver anchors the bitmap to live rows. */
static void sixel_make_room(struct wsap_vtcore *t, const struct sixel_scan *scan)
{
    if (t->cell_pixels.y <= 0 || t->cursor_y < t->scroll_top ||
        t->cursor_y > t->scroll_bottom || lr_margins_partial(t))
        return;
    int height = scan->height;
    if (scan->aspect.y > scan->aspect.x)
        height = (height * scan->aspect.y + scan->aspect.x / 2) / scan->aspect.x;
    int rows = (int)(((int64_t)height - 1) / t->cell_pixels.y) + 1;
    int region = t->scroll_bottom - t->scroll_top + 1;
    int needed = rows < region ? rows + 1 : region;
    int scroll = needed - (t->scroll_bottom - t->cursor_y + 1);
    if (scroll > 0) {
        wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom, scroll, false, true);
        sc_move(t, t->cursor_x, t->cursor_y - scroll);
    }
}

bool wsap_vtcore_sixel_dispatch(struct wsap_vtcore *t, const char *buf, size_t len)
{
    const char *body = sixel_body(buf, len < 16 ? len : 16);
    if (!body) return false;
    const char *end = buf + len;
    int v[3] = {0};
    if (sixel_params(&buf, body, v, 3) < 0 || buf != body || v[1] > 2)
        return true;
    static const int aspect[] = {2, 2, 5, 3, 3, 2, 2, 1, 1, 1};
    struct sixel_scan scan = {.aspect = {1, v[0] <= 9 ? aspect[v[0]] : 2}};
    if (!sixel_scan(t, &scan, body + 1, end) || !scan.width || !scan.height)
        return true;
    size_t count = (size_t)scan.width * (size_t)scan.height;
    if (count > WSAP_VTCORE_GRAPHICS_BYTES_MAX / sizeof(*scan.pixels))
        return true;
    scan.pixels = malloc(count * sizeof(*scan.pixels));
    if (!scan.pixels)
        return true;
    if (v[1] == 1) {
        memset(scan.pixels, 0, count * sizeof(*scan.pixels));
    } else {
        struct sixel_color bg = sixel_argb(t->palette.colors[WSAP_VTCORE_PALETTE_BG]);
        for (size_t i = 0; i < count; i++) scan.pixels[i] = bg;
    }
    if (sixel_scan(t, &scan, body + 1, end)) {
        struct wsap_vtcore_graphics cmd = {
            .action = WSAP_VTCORE_GRAPHICS_SIXEL,
            .format = WSAP_VTCORE_GRAPHICS_RGBA,
            .data = (const uint8_t *)scan.pixels,
            .len = count * sizeof(*scan.pixels),
            .size = {scan.width, scan.height},
            .pixel_aspect = scan.aspect,
            .keep_cursor = true,
        };
        sixel_make_room(t, &scan);
        wsap_vtcore_graphics_emit(t, &cmd);
    }
    free(scan.pixels);
    return true;
}
