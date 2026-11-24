#include "terminal/graphics.h"
#include "terminal/internal.h"
#include "util/grow.h"
#include <stdatomic.h>
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define IMAGE_BYTES_MAX (64u << 20)
#define IMAGE_DECODE_BYTES_MAX (32u << 20)
#define IMAGE_DIM_MAX 4096
#define IMAGE_COUNT_MAX 256
#define PLACEMENT_COUNT_MAX 256
#define PLACEMENT_PIXELS_MAX 65536

struct graphics_image {
    struct graphics_image *next;
    struct ui_image image;
    size_t bytes;
    unsigned refs, placements;
    uint32_t id;
    bool alternate, sixel;
    uint32_t pixels[];
};

struct graphics_cut {
    int band, first, end;
};

struct graphics_placement {
    struct graphics_placement *next;
    struct graphics_image *image;
    struct rect2i uv, dst;
    struct vector2i cell, extent;
    int col, count;
    struct graphics_cut *cuts;
    int n_cuts, cut_cap;
    int32_t z;
    uint32_t id;
    uint64_t rows[];
};

struct graphics_row {
    uint64_t id;
    int y;
};

enum graphics_layer { BEHIND_CELLS, BEHIND_TEXT, ABOVE_TEXT, GRAPHICS_LAYERS };

struct terminal_graphics {
    struct graphics_image *images;
    struct graphics_placement *placements;
    size_t bytes;
    unsigned image_count, placement_count;
    struct ui_scene scenes[GRAPHICS_LAYERS];
    struct graphics_image *frame_refs[PLACEMENT_COUNT_MAX];
    unsigned n_frame_refs;
    uint64_t *row_ids;
    struct graphics_row *view_rows;
    int row_cap, view_cap;
};

static void image_unref(struct graphics_image *image)
{
    if (--image->refs == 0)
        free(image);
}

static void placement_remove(struct terminal_graphics *g, struct graphics_placement **link)
{
    struct graphics_placement *p = *link;
    *link = p->next;
    p->image->placements--;
    image_unref(p->image);
    free(p->cuts);
    free(p);
    g->placement_count--;
}

static void image_remove(struct terminal_graphics *g, struct graphics_image **link)
{
    struct graphics_image *image = *link;
    for (struct graphics_placement **p = &g->placements; *p;) {
        if ((*p)->image == image)
            placement_remove(g, p);
        else
            p = &(*p)->next;
    }
    *link = image->next;
    g->bytes -= image->bytes;
    g->image_count--;
    image_unref(image);
}

static void free_unplaced(struct terminal_graphics *g, bool anonymous_only, bool alternate,
                          uint32_t id, bool targeted)
{
    for (struct graphics_image **link = &g->images; *link;) {
        struct graphics_image *image = *link;
        if (image->alternate == alternate && (!anonymous_only || !image->id) &&
            (!targeted || image->id == id) && !image->placements)
            image_remove(g, link);
        else
            link = &image->next;
    }
}

static void discard_screen(struct terminal_graphics *g, bool alternate)
{
    for (struct graphics_image **link = &g->images; *link;) {
        if ((*link)->alternate == alternate)
            image_remove(g, link);
        else
            link = &(*link)->next;
    }
}

static void clear_frame(struct terminal_graphics *g)
{
    for (int layer = 0; layer < GRAPHICS_LAYERS; layer++)
        ui_scene_init(&g->scenes[layer]);
    for (unsigned i = 0; i < g->n_frame_refs; i++)
        image_unref(g->frame_refs[i]);
    g->n_frame_refs = 0;
}

void terminal_graphics_destroy(struct terminal_graphics *g)
{
    if (!g)
        return;
    clear_frame(g);
    while (g->images)
        image_remove(g, &g->images);
    for (int layer = 0; layer < GRAPHICS_LAYERS; layer++)
        ui_scene_destroy(&g->scenes[layer]);
    free(g->row_ids);
    free(g->view_rows);
    free(g);
}

static enum wsap_vtcore_graphics_result decode_pixels(const struct wsap_vtcore_graphics *cmd,
    const uint8_t *input, size_t len, struct graphics_image **out)
{
    bool is_png = cmd->format == WSAP_VTCORE_GRAPHICS_PNG;
    png_image png = {.version = PNG_IMAGE_VERSION};
    unsigned w = (unsigned)cmd->size.x, h = (unsigned)cmd->size.y;
    enum wsap_vtcore_graphics_result result = WSAP_VTCORE_GRAPHICS_INVALID;
    if (is_png) {
        if (!png_image_begin_read_from_memory(&png, input, len))
            goto done;
        w = png.width;
        h = png.height;
    }
    if (!w || !h || w > IMAGE_DIM_MAX || h > IMAGE_DIM_MAX)
        goto done;
    size_t count = (size_t)w * h;
    size_t bytes = count * sizeof(uint32_t);
    unsigned channels = cmd->format == WSAP_VTCORE_GRAPHICS_RGB ? 3 : 4;
    if (!is_png && len != count * channels)
        goto done;
    result = WSAP_VTCORE_GRAPHICS_NO_SPACE;
    if (bytes > IMAGE_DECODE_BYTES_MAX)
        goto done;
    struct graphics_image *image = malloc(sizeof(*image) + bytes);
    if (!image)
        goto done;
    *image = (struct graphics_image){
        .image = {.pixels = image->pixels, .size = {.x = (int)w, .y = (int)h}},
        .bytes = bytes, .refs = 1, .id = cmd->image_id, .alternate = cmd->alternate,
        .sixel = cmd->action == WSAP_VTCORE_GRAPHICS_SIXEL,
    };
    if (is_png) {
        png.format = PNG_FORMAT_RGBA;
        if (!png_image_finish_read(&png, NULL, image->pixels, 0, NULL)) {
            image_unref(image);
            result = WSAP_VTCORE_GRAPHICS_INVALID;
            goto done;
        }
        input = (const uint8_t *)image->pixels;
    }
    for (size_t i = 0; i < count; i++, input += channels) {
        unsigned a = channels == 4 ? input[3] : 255;
        unsigned r = (input[0] * a + 127) / 255;
        unsigned g = (input[1] * a + 127) / 255;
        unsigned b = (input[2] * a + 127) / 255;
        image->pixels[i] = a << 24 | r << 16 | g << 8 | b;
    }
    *out = image;
    result = WSAP_VTCORE_GRAPHICS_OK;
done:
    if (is_png) png_image_free(&png);
    return result;
}

static enum wsap_vtcore_graphics_result decode(const struct wsap_vtcore_graphics *cmd,
                                               struct graphics_image **out)
{
    if (!cmd->zlib)
        return decode_pixels(cmd, cmd->data, cmd->len, out);
    if (!cmd->uncompressed_size || cmd->uncompressed_size > IMAGE_DECODE_BYTES_MAX)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    uint8_t *raw = malloc(cmd->uncompressed_size);
    if (!raw)
        return WSAP_VTCORE_GRAPHICS_NO_SPACE;
    uLongf size = (uLongf)cmd->uncompressed_size;
    uLong used = (uLong)cmd->len;
    bool ok = uncompress2(raw, &size, cmd->data, &used) == Z_OK &&
              size == cmd->uncompressed_size && used == cmd->len;
    enum wsap_vtcore_graphics_result result = ok ? decode_pixels(cmd, raw, size, out)
                                                : WSAP_VTCORE_GRAPHICS_INVALID;
    free(raw);
    return result;
}

static struct graphics_image **find_image(struct terminal_graphics *g, uint32_t id, bool alternate)
{
    struct graphics_image **link = &g->images;
    while (*link && (!id || (*link)->id != id || (*link)->alternate != alternate))
        link = &(*link)->next;
    return link;
}

/* Prefer retaining images with a placement when reclaiming the image quota. */
static void store_image(struct terminal_graphics *g, struct graphics_image *image)
{
    /* The GPU cache can retain a key after the allocator reuses its address. */
    static _Atomic uint64_t generation = UINT64_C(1) << 63;
    image->image.gen = atomic_fetch_add_explicit(&generation, 1, memory_order_relaxed);
    struct graphics_image **same = find_image(g, image->id, image->alternate);
    if (*same)
        image_remove(g, same);
    while (g->bytes + image->bytes > IMAGE_BYTES_MAX || g->image_count >= IMAGE_COUNT_MAX) {
        struct graphics_image **oldest = &g->images, **unused = NULL;
        for (struct graphics_image **link = &g->images; *link; link = &(*link)->next) {
            oldest = link;
            if (!(*link)->placements) unused = link;
        }
        image_remove(g, unused ? unused : oldest);
    }
    image->next = g->images;
    g->images = image;
    g->bytes += image->bytes;
    g->image_count++;
}

static int extent_cells(int pixels, int cell)
{
    return (pixels + cell - 1) / cell;
}

static enum wsap_vtcore_graphics_result make_placement(struct wsap_vtcore *term,
    struct graphics_image *image, const struct wsap_vtcore_graphics *cmd,
    struct graphics_placement **out)
{
    struct vector2i cell = {.x = cmd->cell_pixels.x, .y = cmd->cell_pixels.y};
    if (cell.x <= 0 || cell.y <= 0 || cell.x > PLACEMENT_PIXELS_MAX || cell.y > PLACEMENT_PIXELS_MAX)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    struct rect2i uv = {.x = cmd->source_pos.x, .y = cmd->source_pos.y};
    int w = image->image.size.x - uv.x, h = image->image.size.y - uv.y;
    uv.width = cmd->source_size.x ? geom_min_i(cmd->source_size.x, w) : w;
    uv.height = cmd->source_size.y ? geom_min_i(cmd->source_size.y, h) : h;
    struct rect2i dst = {.x = cmd->offset.x, .y = cmd->offset.y, .size = uv.size};
    if (w <= 0 || h <= 0 || dst.x >= cell.x || dst.y >= cell.y)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    int64_t dw = uv.width, dh = uv.height;
    if (cmd->action == WSAP_VTCORE_GRAPHICS_SIXEL) {
        int ax = cmd->pixel_aspect.x, ay = cmd->pixel_aspect.y;
        if (ax > ay) dw = (dw * ax + ay / 2) / ay;
        if (ay > ax) dh = (dh * ay + ax / 2) / ax;
    }
    int64_t box_w = (int64_t)cmd->placement_size.x * cell.x;
    int64_t box_h = (int64_t)cmd->placement_size.y * cell.y;
    if (box_w > PLACEMENT_PIXELS_MAX || box_h > PLACEMENT_PIXELS_MAX)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    if (box_w && (!box_h || box_w * dh <= box_h * dw)) {
        dh = (dh * box_w + dw / 2) / dw;
        dw = box_w;
    } else if (box_h) {
        dw = (dw * box_h + dh / 2) / dh;
        dh = box_h;
    }
    if (dw > PLACEMENT_PIXELS_MAX || dh > PLACEMENT_PIXELS_MAX)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    dst.width = geom_max_i(1, (int)dw);
    dst.height = geom_max_i(1, (int)dh);
    if (box_w) dst.x += ((int)box_w - dst.width) / 2;
    if (box_h) dst.y += ((int)box_h - dst.height) / 2;
    struct vector2i extent = {
        .x = extent_cells((int)(box_w ? box_w : dst.x + dst.width), cell.x),
        .y = extent_cells((int)(box_h ? box_h : dst.y + dst.height), cell.y),
    };
    int count = geom_min_i(extent_cells(dst.y + dst.height, cell.y), cmd->grid.y - cmd->cursor.y);
    if (count <= 0)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    struct graphics_placement *p = malloc(sizeof(*p) + (size_t)count * sizeof(p->rows[0]));
    if (!p)
        return WSAP_VTCORE_GRAPHICS_NO_SPACE;
    *p = (struct graphics_placement){
        .image = image, .uv = uv, .dst = dst, .cell = cell, .extent = extent,
        .col = cmd->cursor.x, .count = count, .z = cmd->z_index,
        .id = image->id ? cmd->placement_id : 0,
    };
    wsap_vtcore_graphics_rows(term, true, cmd->cursor.y, count, p->rows);
    *out = p;
    return WSAP_VTCORE_GRAPHICS_OK;
}

static void store_placement(struct terminal_graphics *g, struct graphics_placement *p)
{
    if (p->id) {
        for (struct graphics_placement **link = &g->placements; *link; link = &(*link)->next) {
            if ((*link)->image == p->image && (*link)->id == p->id) {
                placement_remove(g, link);
                break;
            }
        }
    }
    if (g->placement_count == PLACEMENT_COUNT_MAX)
        placement_remove(g, &g->placements);
    struct graphics_placement **link = &g->placements;
    while (*link && ((*link)->z < p->z || ((*link)->z == p->z && (*link)->image->id <= p->image->id)))
        link = &(*link)->next;
    p->next = *link;
    *link = p;
    p->image->refs++;
    p->image->placements++;
    g->placement_count++;
}

static int first_column(const struct graphics_placement *p)
{
    return p->col + p->dst.x / p->cell.x;
}

static int end_column(const struct graphics_placement *p)
{
    return p->col + extent_cells(p->dst.x + p->dst.width, p->cell.x);
}

static bool covers_column(const struct graphics_placement *p, int first, int end)
{
    return first < end_column(p) && end > first_column(p);
}

static int cut_offset(const struct graphics_placement *p, int band)
{
    int lo = 0, hi = p->n_cuts;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->cuts[mid].band < band) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Records erased spans in cell coordinates so they scale with the image.
 * If the span budget is full, drop the band rather than keep erased pixels. */
static void cut_band(struct graphics_placement *p, int band, int first, int end)
{
    enum { CUT_LIMIT = 1024 };
    first = geom_max_i(first, first_column(p));
    end = geom_min_i(end, end_column(p));
    int at = cut_offset(p, band);
    while (at < p->n_cuts && p->cuts[at].band == band && p->cuts[at].end < first)
        at++;
    int stop = at;
    while (stop < p->n_cuts && p->cuts[stop].band == band && p->cuts[stop].first <= end) {
        first = geom_min_i(first, p->cuts[stop].first);
        end = geom_max_i(end, p->cuts[stop].end);
        stop++;
    }
    bool whole = first == first_column(p) && end == end_column(p);
    int count = p->n_cuts - (stop - at) + !whole;
    if (count > CUT_LIMIT || !util_grow(&p->cuts, &p->cut_cap, count, sizeof(*p->cuts))) {
        p->rows[band] = 0;
        return;
    }
    if (p->n_cuts > stop)
        memmove(p->cuts + at + !whole, p->cuts + stop,
                (size_t)(p->n_cuts - stop) * sizeof(*p->cuts));
    p->n_cuts = count;
    if (whole) p->rows[band] = 0;
    else p->cuts[at] = (struct graphics_cut){band, first, end};
}

static bool covers_row(const struct graphics_placement *p, uint64_t row)
{
    if (!row) return false;
    for (int i = p->dst.y / p->cell.y; i < p->count; i++)
        if (p->rows[i] == row) return true;
    return false;
}

static bool rows_capture(struct terminal_graphics *g, struct wsap_vtcore *term,
                         bool live, int first, int count)
{
    if (!util_grow(&g->row_ids, &g->row_cap, count, sizeof(*g->row_ids)))
        return false;
    wsap_vtcore_graphics_rows(term, live, first, count, g->row_ids);
    return true;
}

static void erase_region(struct terminal_graphics *g, struct wsap_vtcore *term,
                         const struct wsap_vtcore_graphics *cmd)
{
    if (!g->placements) return;
    int x = cmd->source_pos.x, w = cmd->source_size.x;
    int y = cmd->source_pos.y, h = cmd->source_size.y;
    if (!rows_capture(g, term, true, y, h))
        return;
    for (struct graphics_placement **link = &g->placements; *link;) {
        struct graphics_placement *p = *link;
        if (p->image->alternate != cmd->alternate || !covers_column(p, x, x + w)) {
            link = &p->next;
            continue;
        }
        bool any = false;
        for (int band = 0; band < p->count; band++) {
            for (int j = 0; p->rows[band] && j < h; j++) {
                if (p->rows[band] == g->row_ids[j]) {
                    cut_band(p, band, x, x + w);
                    break;
                }
            }
            any |= p->rows[band] != 0;
        }
        if (any) link = &p->next;
        else placement_remove(g, link);
    }
    free_unplaced(g, true, cmd->alternate, 0, false);
}

static enum wsap_vtcore_graphics_result delete_images(struct terminal_graphics *g,
    struct wsap_vtcore *term, const struct wsap_vtcore_graphics *cmd)
{
    char mode = cmd->delete_mode;
    bool hard = mode >= 'A' && mode <= 'Z';
    if (hard) mode += 'a' - 'A';
    int col = cmd->source_pos.x - 1, row = cmd->source_pos.y - 1;
    if (mode == 'c') {
        col = cmd->cursor.x;
        row = cmd->cursor.y;
    }
    if ((mode == 'i' && !cmd->image_id) ||
        ((mode == 'x' || mode == 'p' || mode == 'q') && col < 0) ||
        ((mode == 'y' || mode == 'p' || mode == 'q') && row < 0))
        return WSAP_VTCORE_GRAPHICS_INVALID;
    if (mode == 'a' && !rows_capture(g, term, true, 0, cmd->grid.y))
        return WSAP_VTCORE_GRAPHICS_NO_SPACE;
    uint64_t row_id = 0;
    if (mode == 'y' || mode == 'p' || mode == 'q' || mode == 'c')
        wsap_vtcore_graphics_rows(term, true, row, 1, &row_id);
    for (struct graphics_placement **link = &g->placements; *link;) {
        struct graphics_placement *p = *link;
        bool match = false;
        if (p->image->alternate == cmd->alternate && !p->image->sixel) {
            switch (mode) {
            case 'a':
                for (int y = 0; y < cmd->grid.y && !match; y++)
                    match = covers_row(p, g->row_ids[y]);
                break;
            case 'i': match = p->image->id == cmd->image_id && (!cmd->placement_id ||
                                                              p->id == cmd->placement_id); break;
            case 'r': match = p->image->id && p->image->id >= (uint32_t)cmd->source_pos.x &&
                                             p->image->id <= (uint32_t)cmd->source_pos.y; break;
            case 'z': match = p->z == cmd->z_index; break;
            case 'x': match = covers_column(p, col, col + 1); break;
            case 'y': match = covers_row(p, row_id); break;
            case 'q': if (p->z != cmd->z_index) break;
                /* fall through */
            case 'c': case 'p': match = covers_column(p, col, col + 1) && covers_row(p, row_id); break;
            }
        }
        if (match) {
            struct graphics_image *image = p->image;
            placement_remove(g, link);
            if (hard && !image->placements) {
                struct graphics_image **target = &g->images;
                while (*target != image) target = &(*target)->next;
                image_remove(g, target);
            }
        } else {
            link = &p->next;
        }
    }
    if (hard && mode == 'i' && !cmd->placement_id)
        free_unplaced(g, false, cmd->alternate, cmd->image_id, true);
    free_unplaced(g, true, cmd->alternate, 0, false);
    return WSAP_VTCORE_GRAPHICS_OK;
}

enum wsap_vtcore_graphics_result terminal_graphics_receive(
    void *ud, const struct wsap_vtcore_graphics *cmd, struct wsap_vtcore_vector2i *extent)
{
    struct terminal *t = ud;
    struct terminal_graphics *g = t->graphics;
    switch (cmd->action) {
    case WSAP_VTCORE_GRAPHICS_RESET:
        if (g) while (g->images) image_remove(g, &g->images);
        return WSAP_VTCORE_GRAPHICS_OK;
    case WSAP_VTCORE_GRAPHICS_DISCARD_SCREEN:
        if (g) discard_screen(g, cmd->alternate);
        return WSAP_VTCORE_GRAPHICS_OK;
    case WSAP_VTCORE_GRAPHICS_ERASE:
        if (g) erase_region(g, t->session.term, cmd);
        return WSAP_VTCORE_GRAPHICS_OK;
    case WSAP_VTCORE_GRAPHICS_DELETE:
        return g ? delete_images(g, t->session.term, cmd) : WSAP_VTCORE_GRAPHICS_OK;
    default: break;
    }
    bool place = cmd->action == WSAP_VTCORE_GRAPHICS_SIXEL ||
        cmd->action == WSAP_VTCORE_GRAPHICS_TRANSMIT_AND_PLACE || cmd->action == WSAP_VTCORE_GRAPHICS_PLACE;
    bool transmit = cmd->action != WSAP_VTCORE_GRAPHICS_PLACE;
    struct graphics_image *image = NULL;
    if (transmit) {
        enum wsap_vtcore_graphics_result result = decode(cmd, &image);
        if (result != WSAP_VTCORE_GRAPHICS_OK)
            return result;
        if (cmd->action == WSAP_VTCORE_GRAPHICS_QUERY || (!place && !image->id)) {
            image_unref(image);
            return WSAP_VTCORE_GRAPHICS_OK;
        }
    } else {
        image = g ? *find_image(g, cmd->image_id, cmd->alternate) : NULL;
        if (!image)
            return WSAP_VTCORE_GRAPHICS_NOT_FOUND;
    }
    if (!g) {
        g = calloc(1, sizeof(*g));
        if (!g) {
            image_unref(image);
            return WSAP_VTCORE_GRAPHICS_NO_SPACE;
        }
        t->graphics = g;
    }
    struct graphics_placement *p = NULL;
    if (place) {
        enum wsap_vtcore_graphics_result result = make_placement(t->session.term, image, cmd, &p);
        if (result != WSAP_VTCORE_GRAPHICS_OK) {
            if (transmit) image_unref(image);
            return result;
        }
    }
    if (transmit) store_image(g, image);
    if (p) {
        *extent = (struct wsap_vtcore_vector2i){p->extent.x, p->extent.y};
        if (!cmd->alternate)
            wsap_vtcore_graphics_track_rows(t->session.term, cmd->cursor.y, p->count);
        store_placement(g, p);
    }
    return WSAP_VTCORE_GRAPHICS_OK;
}

static int compare_rows(const void *ap, const void *bp)
{
    const struct graphics_row *a = ap, *b = bp;
    return (a->id > b->id) - (a->id < b->id);
}

static int find_row(const struct terminal_graphics *g, int count, uint64_t id)
{
    if (!id) return -1;
    const struct graphics_row key = {.id = id};
    const struct graphics_row *hit = bsearch(&key, g->view_rows, (size_t)count,
                                             sizeof(key), compare_rows);
    return hit ? hit->y : -1;
}

static int scale_pixels(int px, int target, int base)
{
    return geom_sat_i(((int64_t)px * target + base / 2) / base);
}

static bool draw_image_clip(struct ui_scene *scene, const struct graphics_placement *p,
                            struct rect2i dst, struct rect2i clip)
{
    return ui_scene_push_clip(scene, clip) &&
           ui_scene_image_rect(scene, dst, &p->image->image, &(struct ui_image_options){.uv = p->uv}) &&
           ui_scene_pop_clip(scene);
}

static bool draw_placement(const struct terminal_graphics *g, struct ui_scene *scene,
                           const struct graphics_placement *p, struct rect2i bounds,
                           struct vector2i cell, int count)
{
    struct rect2i dst = {
        .x = geom_sat_i((int64_t)bounds.x + (int64_t)p->col * cell.x +
                        scale_pixels(p->dst.x, cell.x, p->cell.x)),
        .width = geom_max_i(1, scale_pixels(p->dst.width, cell.x, p->cell.x)),
        .height = geom_max_i(1, scale_pixels(p->dst.height, cell.y, p->cell.y)),
    };
    int offset = scale_pixels(p->dst.y, cell.y, p->cell.y);
    bool drew = false;
    for (int i = 0; i < p->count;) {
        int y = find_row(g, count, p->rows[i]);
        if (y < 0) { i++; continue; }
        int start = i++;
        int cut = cut_offset(p, start);
        bool clipped = cut < p->n_cuts && p->cuts[cut].band == start;
        if (!clipped)
            while (i < p->count && (cut == p->n_cuts || i < p->cuts[cut].band) &&
                   find_row(g, count, p->rows[i]) == y + i - start)
                i++;
        struct rect2i clip = {
            .x = bounds.x, .y = geom_sat_i((int64_t)bounds.y + (int64_t)y * cell.y),
            .width = bounds.width, .height = geom_sat_i((int64_t)(i - start) * cell.y),
        };
        dst.y = geom_sat_i((int64_t)clip.y + offset - (int64_t)start * cell.y);
        clip = rect2i_intersect(clip, dst);
        if (rect2i_is_empty(clip))
            continue;
        int left = clip.x, right = rect2i_end(clip).x;
        while (cut < p->n_cuts && p->cuts[cut].band == start && left < right) {
            const struct graphics_cut *span = &p->cuts[cut++];
            int x = geom_sat_i((int64_t)bounds.x + (int64_t)span->first * cell.x);
            int stop = geom_min_i(x, right);
            if (stop > left) {
                struct rect2i piece = clip;
                piece.x = left;
                piece.width = stop - left;
                if (!draw_image_clip(scene, p, dst, piece)) return drew;
                drew = true;
            }
            x = geom_sat_i((int64_t)bounds.x + (int64_t)span->end * cell.x);
            left = geom_max_i(left, x);
        }
        if (right > left) {
            clip.x = left;
            clip.width = right - left;
            if (!draw_image_clip(scene, p, dst, clip)) return drew;
            drew = true;
        }
    }
    return drew;
}

struct terminal_graphics_layers terminal_graphics_capture(struct terminal *t, struct rect2i bounds,
                                                            int rows, bool live_view)
{
    struct terminal_graphics_layers result = {0};
    struct terminal_graphics *g = t->graphics;
    if (!g)
        return result;
    clear_frame(g);
    if (!g->placements || rows <= 0 || rect2i_is_empty(bounds))
        return result;
    const struct font_metrics *m = terminal_font_metrics(t);
    struct vector2i cell = {.x = m->cell_w, .y = m->cell_h};
    if (cell.x <= 0 || cell.y <= 0 ||
        !util_grow(&g->view_rows, &g->view_cap, rows, sizeof(*g->view_rows)) ||
        !rows_capture(g, t->session.term, live_view, 0, rows))
        return result;
    for (int y = 0; y < rows; y++)
        g->view_rows[y] = (struct graphics_row){g->row_ids[y], y};
    qsort(g->view_rows, (size_t)rows, sizeof(*g->view_rows), compare_rows);
    bool alternate = wsap_vtcore_modes(t->session.term).alternate;
    bool prune = live_view && !wsap_vtcore_graphics_has_history(t->session.term);
    for (int layer = 0; layer < GRAPHICS_LAYERS; layer++)
        if (!ui_scene_push_clip(&g->scenes[layer], bounds))
            return result;
    for (struct graphics_placement **link = &g->placements; *link;) {
        struct graphics_placement *p = *link;
        if (p->image->alternate == alternate) {
            if (prune) {
                bool any = false;
                for (int i = 0; i < p->count; i++) {
                    if (find_row(g, rows, p->rows[i]) < 0) p->rows[i] = 0;
                    any |= p->rows[i] != 0;
                }
                if (!any) {
                    placement_remove(g, link);
                    continue;
                }
            }
            enum graphics_layer layer = p->z < INT32_MIN / 2 ? BEHIND_CELLS :
                                        p->z < 0 ? BEHIND_TEXT : ABOVE_TEXT;
            if (draw_placement(g, &g->scenes[layer], p, bounds, cell, rows)) {
                p->image->refs++;
                g->frame_refs[g->n_frame_refs++] = p->image;
            }
        }
        link = &p->next;
    }
    free_unplaced(g, true, alternate, 0, false);
    const struct ui_scene **layers[] = {&result.behind_cells, &result.behind_text, &result.above_text};
    for (int layer = 0; layer < GRAPHICS_LAYERS; layer++) {
        struct ui_scene *scene = &g->scenes[layer];
        if (ui_scene_pop_clip(scene) && scene->n_ops > 2 && ui_scene_is_valid(scene))
            *layers[layer] = scene;
    }
    return result;
}
