#include "render/gpu/build.h"

#include <stdlib.h>
#include <string.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "paint/framebuffer.h"
#include "paint/scene.h"
#include "paint/shadow.h"
#include "paint/text_run.h"
#include "render/cpu/decor.h"
#include "render/frame/walk.h"
#include "render/render.h"
#include "util/damage.h"
#include "util/hash.h"

/* See build.h for the contract of this file. */

#define ATLAS_MAP_SIZE (1u << 15)
#define ATLAS_MAP_LIMIT (ATLAS_MAP_SIZE / 2)

enum atlas_space {
    SPACE_GRID,
    SPACE_UI,
    SPACE_DECOR,
};

enum glyph_kind {
    KIND_NONE,
    KIND_ALPHA,
    KIND_SUBPX,
    KIND_COLOR,
};

/*
 * One atlas entry. `kind` NONE means: the glyph has no ink, and it takes no
 * room. `rows` are the rows in the atlas. `x_offset` is the glyph offset
 * from the cell.
 */
struct gpu_atlas_entry {
    uint64_t id;
    uint64_t version;
    uint32_t space;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t rows;
    int16_t x_offset;
    uint8_t kind;
    bool used;
};

struct gpu_shelf {
    uint16_t y;
    uint16_t h;
    uint16_t x;
};

/* A grid color. The grid is opaque, whatever the alpha of the color says. */
static inline void opaque_color_of(uint32_t argb, float opacity, float out[4])
{
    gpu_color(argb | 0xFF000000u, opacity, out);
}

/* White with an opacity: the color of an instance that shows its texture. */
static inline void white_of(float opacity, float out[4])
{
    out[0] = 1.0f;
    out[1] = 1.0f;
    out[2] = 1.0f;
    out[3] = opacity;
}

static inline void rect_to_vec4(float x, float y, float w, float h, float out[4])
{
    out[0] = x;
    out[1] = y;
    out[2] = w;
    out[3] = h;
}

static inline void rect2i_to_vec4(struct rect2i r, float out[4])
{
    rect_to_vec4((float)r.x, (float)r.y, (float)r.width, (float)r.height, out);
}

static inline int min_i(int a, int b)
{
    return a < b ? a : b;
}

static inline int max_i(int a, int b)
{
    return a > b ? a : b;
}

/* ---- Instance streams and commands -------------------------------------- */

static void *stream_push(struct gpu_build *b, enum gpu_stream_id id)
{
    struct gpu_stream *s = &b->streams[id];
    size_t stride = gpu_stream_stride(id);
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 256;
        void *p = realloc(s->data, (size_t)cap * stride);
        if (!p) {
            b->failed = true;
            return NULL;
        }
        s->data = p;
        s->cap = cap;
    }
    void *e = s->data + (size_t)s->n++ * stride;
    memset(e, 0, stride);
    return e;
}

static struct gpu_glyph_inst *push_glyph(struct gpu_build *b, enum gpu_stream_id id)
{
    return stream_push(b, id);
}

/* Adds a command for the instances that the stream holds now, or extends the last. */
static struct gpu_cmd *cmd_open(struct gpu_build *b, enum gpu_cmd_kind kind,
                                enum gpu_stream_id stream, struct rect2i scissor, int image)
{
    uint32_t next = b->streams[stream].n;
    if (b->n_cmds) {
        struct gpu_cmd *last = &b->cmds[b->n_cmds - 1];
        if (last->kind == kind && last->stream == stream && last->image == image &&
            rect2i_eq(last->scissor, scissor) && last->start + last->count == next)
            return last;
    }
    if (b->n_cmds == b->cap_cmds) {
        uint32_t cap = b->cap_cmds ? b->cap_cmds * 2 : 64;
        struct gpu_cmd *p = realloc(b->cmds, (size_t)cap * sizeof(*p));
        if (!p) {
            b->failed = true;
            return NULL;
        }
        b->cmds = p;
        b->cap_cmds = cap;
    }
    struct gpu_cmd *c = &b->cmds[b->n_cmds++];
    *c = (struct gpu_cmd){
        .kind = kind, .stream = stream, .scissor = scissor, .start = next,
        .image = image,
    };
    return c;
}

/* A command for every instance that the stream holds. For the grid streams. */
static void cmd_whole_stream(struct gpu_build *b, enum gpu_cmd_kind kind,
                             enum gpu_stream_id stream, struct rect2i scissor)
{
    uint32_t n = b->streams[stream].n;
    if (!n)
        return;
    struct gpu_cmd *c = cmd_open(b, kind, stream, scissor, 0);
    if (!c)
        return;
    c->start = 0;
    c->count = n;
}

static void begin_build(struct gpu_build *b)
{
    for (int i = 0; i < GPU_ST_COUNT; i++)
        b->streams[i].n = 0;
    b->n_cmds = 0;
    b->atlas.stage_used = 0;
    b->atlas.n_uploads = 0;
    b->atlas.full = false;
    b->failed = false;
}

/* ---- The atlas ---------------------------------------------------------- */

/* Forgets every glyph. The texture of the backend keeps its pixels, but no
 * entry points to them anymore. */
static void atlas_clear(struct gpu_atlas *a)
{
    memset(a->map, 0, (size_t)ATLAS_MAP_SIZE * sizeof(*a->map));
    a->count = 0;
    a->n_shelves = 0;
    a->next_y = 0;
    a->stage_used = 0;
    a->n_uploads = 0;
    a->full = false;
}


static inline uint64_t atlas_hash(const struct gpu_atlas_entry *k)
{
    uint64_t h = k->id ^ (k->version * UINT64_C(0x9E3779B97F4A7C15)) ^
                 ((uint64_t)k->space << 56) ^ UINT64_C(0xA5A5A5A5);
    return hash64_finish(h);
}

static bool atlas_key_eq(const struct gpu_atlas_entry *e, const struct gpu_atlas_entry *k)
{
    return e->id == k->id && e->version == k->version && e->space == k->space;
}

static const struct gpu_atlas_entry *atlas_find(const struct gpu_atlas *a, uint64_t id,
                                                uint64_t version, enum atlas_space space)
{
    struct gpu_atlas_entry key = { .id = id, .version = version, .space = space };
    for (uint32_t i = (uint32_t)atlas_hash(&key) & (ATLAS_MAP_SIZE - 1);;
         i = (i + 1) & (ATLAS_MAP_SIZE - 1)) {
        const struct gpu_atlas_entry *e = &a->map[i];
        if (!e->used)
            return NULL;
        if (atlas_key_eq(e, &key))
            return e;
    }
}

static bool atlas_insert(struct gpu_atlas *a, const struct gpu_atlas_entry *entry)
{
    if (a->count >= ATLAS_MAP_LIMIT) {
        a->full = true;
        return false;
    }
    uint32_t i = (uint32_t)atlas_hash(entry) & (ATLAS_MAP_SIZE - 1);
    while (a->map[i].used)
        i = (i + 1) & (ATLAS_MAP_SIZE - 1);
    a->map[i] = *entry;
    a->map[i].used = true;
    a->count++;
    return true;
}

/* Finds room for w by h texels. Rows of nearly one height share a shelf. */
static bool atlas_place(struct gpu_atlas *a, int w, int h, uint16_t *x, uint16_t *y)
{
    int quantized = (h + 3) & ~3;
    for (int i = 0; i < a->n_shelves; i++) {
        struct gpu_shelf *s = &a->shelves[i];
        if (s->h >= h && s->h <= quantized + quantized / 2 && s->x + w <= a->size) {
            *x = s->x;
            *y = s->y;
            s->x = (uint16_t)(s->x + w);
            return true;
        }
    }
    if (a->n_shelves == GPU_SHELVES_MAX || a->next_y + quantized > a->size)
        return false;
    struct gpu_shelf *s = &a->shelves[a->n_shelves++];
    *s = (struct gpu_shelf){ .y = (uint16_t)a->next_y, .h = (uint16_t)quantized, .x = (uint16_t)w };
    a->next_y += quantized;
    *x = 0;
    *y = s->y;
    return true;
}

/*
 * Stages a w by h rect. Returns where to write its texels, or NULL. NULL
 * with atlas.full set means: no room. NULL without it means: no memory.
 * The pointer stays valid until the next call.
 */
static uint32_t *atlas_stage(struct gpu_build *b, int w, int h, uint16_t *x, uint16_t *y)
{
    struct gpu_atlas *a = &b->atlas;
    if (w > a->size || h > a->size) {
        a->full = true;
        return NULL;
    }
    if (!atlas_place(a, w, h, x, y)) {
        a->full = true;
        return NULL;
    }
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (a->stage_used + bytes > a->stage_cap) {
        size_t cap = a->stage_cap ? a->stage_cap * 2 : (size_t)1 << 20;
        while (cap < a->stage_used + bytes)
            cap *= 2;
        uint8_t *p = realloc(a->stage, cap);
        if (!p) {
            b->failed = true;
            return NULL;
        }
        a->stage = p;
        a->stage_cap = cap;
    }
    if (a->n_uploads == a->cap_uploads) {
        uint32_t cap = a->cap_uploads ? a->cap_uploads * 2 : 128;
        struct gpu_upload *p = realloc(a->uploads, (size_t)cap * sizeof(*p));
        if (!p) {
            b->failed = true;
            return NULL;
        }
        a->uploads = p;
        a->cap_uploads = cap;
    }
    a->uploads[a->n_uploads++] = (struct gpu_upload){
        .x = *x, .y = *y, .w = (uint16_t)w, .h = (uint16_t)h, .offset = a->stage_used,
    };
    uint32_t *texels = (uint32_t *)(void *)(a->stage + a->stage_used);
    a->stage_used += bytes;
    return texels;
}

static enum glyph_kind glyph_kind_of(const struct glyph *gl)
{
    if (!gl || !gl->width || !gl->height)
        return KIND_NONE;
    if (gl->has_color)
        return gl->color ? KIND_COLOR : KIND_NONE;
    if (gl->has_subpx_ink && gl->subpx)
        return KIND_SUBPX;
    return gl->alpha ? KIND_ALPHA : KIND_NONE;
}

/*
 * Puts a glyph in the atlas. With `cache` the map keeps it. Without, the
 * entry is for this one use. Returns false when the frame must stop. The
 * `out` entry is a copy that stays valid.
 */
static bool atlas_add_glyph(struct gpu_build *b, uint64_t id, uint64_t version,
                            enum atlas_space space, const struct glyph *gl, bool cache,
                            struct gpu_atlas_entry *out)
{
    enum glyph_kind kind = glyph_kind_of(gl);
    struct gpu_atlas_entry e = { .id = id, .version = version, .space = space, .kind = kind };
    if (kind != KIND_NONE) {
        int w = gl->width, rows = gl->height;
        uint32_t *dst = atlas_stage(b, w, rows, &e.x, &e.y);
        if (!dst)
            return false;
        e.w = (uint16_t)w;
        e.rows = (uint16_t)rows;
        e.x_offset = gl->x_offset;
        for (int y = 0; y < rows; y++, dst += w) {
            switch (kind) {
            case KIND_ALPHA: {
                const uint8_t *s = gl->alpha[y];
                for (int x = 0; x < w; x++)
                    dst[x] = (uint32_t)s[x] << 24;
                break;
            }
            case KIND_SUBPX: {
                const uint8_t *s = gl->subpx[y];
                for (int x = 0; x < w; x++)
                    dst[x] = 0xFF000000u | (uint32_t)s[3 * x] << 16 |
                             (uint32_t)s[3 * x + 1] << 8 | s[3 * x + 2];
                break;
            }
            case KIND_COLOR:
                memcpy(dst, gl->color + (size_t)y * (size_t)w, (size_t)w * sizeof(*dst));
                break;
            case KIND_NONE:
                break;
            }
        }
    }
    *out = e;
    return !cache || atlas_insert(&b->atlas, &e);
}

/* Puts a coverage mask in the atlas. `stride` is the bytes from row to row. */
static bool atlas_add_mask(struct gpu_build *b, uint64_t id, uint64_t version,
                           enum atlas_space space, const uint8_t *cov, int w, int rows,
                           int stride, struct gpu_atlas_entry *out)
{
    struct gpu_atlas_entry e = {
        .id = id, .version = version, .space = space, .kind = KIND_ALPHA,
        .w = (uint16_t)w, .rows = (uint16_t)rows,
    };
    uint32_t *dst = atlas_stage(b, w, rows, &e.x, &e.y);
    if (!dst)
        return false;
    for (int y = 0; y < rows; y++, dst += w)
        for (int x = 0; x < w; x++)
            dst[x] = (uint32_t)cov[(size_t)y * (size_t)stride + (size_t)x] << 24;
    *out = e;
    return atlas_insert(&b->atlas, &e);
}

/* ---- The grid ----------------------------------------------------------- */

/* The part of a render_frame that the grid draws. */
struct grid_geometry {
    const struct terminal_frame *tf;
    struct font_instance *font;
    struct rect2i rect;
    int cw;
    int ch;
    int cols;
    int rows;
};

/* The same rules as render_draw(): clip, cell size, and visible cells. */
static bool grid_geometry_of(const struct render_frame *frame, struct vector2i canvas,
                             struct grid_geometry *out)
{
    const struct terminal_frame *tf = frame->grid.frame;
    struct font_instance *font = frame->grid.font ? frame->grid.font : font_instance_default();
    struct rect2i rect = frame->grid.rect;
    if (!tf || !terminal_frame_valid(tf) || !font || rect.x < 0 || rect.y < 0 ||
        !rect2i_clip(&rect, canvas))
        return false;
    const struct font_metrics *m = font_instance_metrics(font);
    if (m->cell_w <= 0 || m->cell_h <= 0)
        return false;
    *out = (struct grid_geometry){
        .tf = tf, .font = font, .rect = rect, .cw = m->cell_w, .ch = m->cell_h,
        .cols = min_i(rect.width / m->cell_w, tf->cols),
        .rows = min_i(rect.height / m->cell_h, tf->rows),
    };
    return true;
}

/* The state of the row walk. */
struct grid_walk {
    struct gpu_build *b;
    const struct grid_geometry *geo;
    const struct decor_set *decor;
    uint64_t font_version;
    uint64_t decor_version;
    uint32_t default_bg;
    uint32_t cursor_color;
    int row;
    enum wsap_vtcore_line_size line_size;
    /* The open run of equal backgrounds: columns [run_x0, run_x1). */
    bool run_open;
    int run_x0;
    int run_x1;
    uint32_t run_bg;
};

static void bg_flush(struct grid_walk *w)
{
    if (!w->run_open)
        return;
    w->run_open = false;
    struct gpu_glyph_inst *in = push_glyph(w->b, GPU_ST_BG);
    if (!in)
        return;
    const struct grid_geometry *geo = w->geo;
    int cell_w = (w->line_size != WSAP_VTCORE_LINE_SINGLE) ? 2 * geo->cw : geo->cw;
    rect_to_vec4((float)(geo->rect.x + w->run_x0 * cell_w),
                 (float)(geo->rect.y + w->row * geo->ch),
                 (float)((w->run_x1 - w->run_x0) * cell_w), (float)geo->ch, in->rect);
    opaque_color_of(w->run_bg, 1.0f, in->color);
    in->mode = GPU_MODE_SOLID;
}

/* Adds the backgrounds of columns [c0, c1). The grid area is in default_bg already. */
static void bg_add(struct grid_walk *w, int c0, int c1, uint32_t bg)
{
    bg |= 0xFF000000u;
    if (bg == w->default_bg) {
        bg_flush(w);
        return;
    }
    if (w->run_open && w->run_x1 == c0 && w->run_bg == bg) {
        w->run_x1 = c1;
        return;
    }
    bg_flush(w);
    w->run_open = true;
    w->run_x0 = c0;
    w->run_x1 = c1;
    w->run_bg = bg;
}

static void grid_run(void *ctx, int c0, int c1, uint32_t bg)
{
    bg_add(ctx, c0, c1, bg);
}

/* One quad of the glyph in the atlas: its columns [c0, c1), with column c0 at dest_x. */
static void emit_glyph_part(struct grid_walk *w, const struct gpu_atlas_entry *e, int c0, int c1,
                            int dest_x, uint32_t fg, bool faint)
{
    const struct grid_geometry *geo = w->geo;
    int rows = min_i(e->rows, geo->ch);
    if (c1 <= c0 || rows <= 0)
        return;
    bool subpx = e->kind == KIND_SUBPX;
    struct gpu_glyph_inst *in = push_glyph(w->b, subpx ? GPU_ST_SUBPX : GPU_ST_INK);
    if (!in)
        return;
    bool double_w = (w->line_size != WSAP_VTCORE_LINE_SINGLE);
    bool double_h = (w->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP ||
                     w->line_size == WSAP_VTCORE_LINE_DOUBLE_BOTTOM);
    float width = (float)(c1 - c0);
    float height = (float)rows;
    float v0 = (float)e->y;
    float v1 = (float)(e->y + rows);
    if (double_w)
        width *= 2.0f;
    if (double_h) {
        int half_h = geo->ch / 2;
        if (w->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP) {
            int sub_rows = min_i(rows, half_h);
            v1 = (float)(e->y + sub_rows);
            height = (float)(sub_rows * 2);
        } else {
            int start_row = min_i(half_h, rows);
            int sub_rows = rows > half_h ? rows - half_h : 0;
            v0 = (float)(e->y + start_row);
            v1 = (float)(e->y + rows);
            height = (float)(sub_rows * 2);
        }
    }
    rect_to_vec4((float)dest_x, (float)(geo->rect.y + w->row * geo->ch), width, height, in->rect);
    rect_to_vec4((float)(e->x + c0), v0, (float)(e->x + c1), v1, in->uv);
    float opacity = faint ? 128.0f / 255.0f : 1.0f;
    if (e->kind == KIND_COLOR) {
        white_of(opacity, in->color);
        in->mode = GPU_MODE_COLOR;
    } else {
        opaque_color_of(fg, opacity, in->color);
        in->mode = subpx ? GPU_MODE_SUBPX : GPU_MODE_ALPHA;
    }
}

static void emit_mask(struct grid_walk *w, int col, const struct decor_mask *m, int index,
                      uint32_t color, bool faint)
{
    if (!m || m->y0 >= m->y1)
        return;
    const struct grid_geometry *geo = w->geo;
    bool double_w = (w->line_size != WSAP_VTCORE_LINE_SINGLE);
    bool double_h = (w->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP ||
                     w->line_size == WSAP_VTCORE_LINE_DOUBLE_BOTTOM);
    int cell_w = double_w ? 2 * geo->cw : geo->cw;
    int rows = m->y1 - m->y0;
    int y0 = m->y0;
    float dest_y = (float)(geo->rect.y + w->row * geo->ch);
    float height = (float)rows;
    if (double_h) {
        int half_h = geo->ch / 2;
        if (w->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP) {
            if (y0 >= half_h) return;
            rows = min_i(m->y1, half_h) - y0;
            height = (float)(rows * 2);
            dest_y += (float)(y0 * 2);
        } else {
            if (m->y1 <= half_h) return;
            y0 = max_i(y0, half_h);
            rows = m->y1 - y0;
            height = (float)(rows * 2);
            dest_y += (float)((y0 - half_h) * 2);
        }
    } else {
        dest_y += (float)y0;
    }
    struct gpu_glyph_inst *in = push_glyph(w->b, GPU_ST_DECO);
    if (!in)
        return;
    rect_to_vec4((float)(geo->rect.x + col * cell_w), dest_y, (float)cell_w, height, in->rect);
    opaque_color_of(color, faint ? 128.0f / 255.0f : 1.0f, in->color);
    if (m->solid) {
        in->mode = GPU_MODE_SOLID;
        return;
    }
    struct gpu_atlas_entry e;
    const struct gpu_atlas_entry *hit = atlas_find(&w->b->atlas, (uint64_t)index, w->decor_version,
                                               SPACE_DECOR);
    if (hit) {
        e = *hit;
    } else if (!atlas_add_mask(w->b, (uint64_t)index, w->decor_version, SPACE_DECOR,
                               m->cov + (size_t)m->y0 * (size_t)geo->cw, geo->cw, rows, geo->cw,
                               &e)) {
        return;
    }
    in->mode = GPU_MODE_ALPHA;
    rect_to_vec4((float)e.x, (float)e.y, (float)(e.x + geo->cw), (float)(e.y + rows), in->uv);
}

enum {
    MASK_STRIKE = 16,
    MASK_OVERLINE,
    MASK_CURSOR_BAR,
    MASK_CURSOR_UNDERLINE,
};

static void emit_decor(struct grid_walk *w, int col, const struct render_cell *cell)
{
    const struct decor_set *dc = w->decor;
    uint16_t fl = cell->flags;
    if (!dc || !(fl & (RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)))
        return;
    bool faint = (fl & RENDER_CELL_FAINT) != 0;
    uint32_t ul = (fl & RENDER_CELL_UL_COLOR) ? cell->ul : cell->fg;
    unsigned underline = render_cell_underline(fl);
    if (underline && underline < DECOR_UNDERLINE_KINDS)
        emit_mask(w, col, &dc->underline[underline], (int)underline, ul, faint);
    if (fl & RENDER_CELL_STRIKE)
        emit_mask(w, col, &dc->strike, MASK_STRIKE, cell->fg, faint);
    if (fl & RENDER_CELL_OVERLINE)
        emit_mask(w, col, &dc->overline, MASK_OVERLINE, cell->fg, faint);
    if (fl & RENDER_CELL_CURSOR_BAR)
        emit_mask(w, col, &dc->cursor_bar, MASK_CURSOR_BAR, w->cursor_color, false);
    if (fl & RENDER_CELL_CURSOR_UNDERLINE)
        emit_mask(w, col, &dc->cursor_underline, MASK_CURSOR_UNDERLINE, w->cursor_color, false);
}

static void grid_cell(void *ctx, int col, const struct render_cell *cell,
                      const struct render_glyph_ref *ref)
{
    struct grid_walk *w = ctx;
    const struct grid_geometry *geo = w->geo;
    if (w->b->failed || w->b->atlas.full)
        return;
    bg_add(w, col, col + 1, cell->bg);
    uint16_t fl = cell->flags;
    if (!(fl & RENDER_CELL_BLANK)) {
        struct gpu_atlas_entry e;
        const struct gpu_atlas_entry *hit =
            ref->id ? atlas_find(&w->b->atlas, ref->id, w->font_version, SPACE_GRID) : NULL;
        if (hit) {
            e = *hit;
        } else if (!atlas_add_glyph(w->b, ref->id, w->font_version, SPACE_GRID,
                                    render_glyph_get(ref), ref->id != 0, &e)) {
            return;
        }
        if (e.kind != KIND_NONE) {
            bool double_w = (w->line_size != WSAP_VTCORE_LINE_SINGLE);
            int cell_w = double_w ? 2 * geo->cw : geo->cw;
            int cell_x = geo->rect.x + col * cell_w;
            int x_off = double_w ? 2 * e.x_offset : e.x_offset;
            int split = geo->cw - e.x_offset;
            split = split < 0 ? 0 : split > e.w ? e.w : split;
            bool faint = (fl & RENDER_CELL_FAINT) != 0;
            if (fl & RENDER_CELL_WIDE_TAIL)
                emit_glyph_part(w, &e, split, e.w,
                                double_w ? cell_x + 2 * (split - (geo->cw - e.x_offset))
                                         : cell_x + split - (geo->cw - e.x_offset),
                                cell->fg, faint);
            else if (fl & RENDER_CELL_WIDE)
                emit_glyph_part(w, &e, 0, split, cell_x + x_off, cell->fg, faint);
            else
                emit_glyph_part(w, &e, 0, e.w, cell_x + x_off, cell->fg, faint);
        }
    }
    emit_decor(w, col, cell);
}

static bool build_grid(struct gpu_build *b, const struct grid_geometry *geo)
{
    const struct terminal_frame *tf = geo->tf;
    struct grid_walk w = {
        .b = b,
        .geo = geo,
        .decor = decor_get(b->decor, font_instance_metrics(geo->font)),
        .font_version = font_instance_version(geo->font),
        .default_bg = tf->default_bg | 0xFF000000u,
        .cursor_color = tf->cursor_color,
    };
    struct decor_key dk = decor_key_of(font_instance_metrics(geo->font));
    w.decor_version = hash64_finish(((uint64_t)(uint32_t)dk.cw << 32) | (uint32_t)dk.ch) ^
                      hash64_finish(((uint64_t)(uint32_t)dk.baseline << 32) |
                                    (uint32_t)dk.thickness);

    /* The whole grid area first: the margins, and every cell with the default bg. */
    struct gpu_glyph_inst *fill = push_glyph(b, GPU_ST_BG);
    if (!fill)
        return false;
    rect2i_to_vec4(geo->rect, fill->rect);
    opaque_color_of(w.default_bg, 1.0f, fill->color);
    fill->mode = GPU_MODE_SOLID;

    const struct render_walk_sink sink = { .cell = grid_cell, .run = grid_run, .ctx = &w };
    (void)render_walk_thread_reserve(tf->cols);
    glyph_cache_set_parallel(true);
    for (int row = 0; row < geo->rows && !b->failed && !b->atlas.full; row++) {
        w.row = row;
        w.line_size = terminal_frame_row_size(tf, row);
        int cols = (w.line_size != WSAP_VTCORE_LINE_SINGLE)
            ? (geo->cols / 2 > 0 ? geo->cols / 2 : 1) : geo->cols;
        render_row_walk(geo->font, tf, row, 0, cols, RENDER_WALK_PEEK, &sink);
        bg_flush(&w);
    }
    glyph_cache_set_parallel(false);
    return !b->failed && !b->atlas.full;
}

/* ---- The UI scene ------------------------------------------------------- */

static void emit_solid(struct gpu_build *b, struct rect2i r, const float color[4],
                       struct rect2i canvas_rect)
{
    struct gpu_cmd *c = cmd_open(b, GPU_CMD_GLYPH, GPU_ST_UI, canvas_rect, 0);
    struct gpu_glyph_inst *in = push_glyph(b, GPU_ST_UI);
    if (!c || !in)
        return;
    c->count++;
    rect2i_to_vec4(r, in->rect);
    memcpy(in->color, color, sizeof(in->color));
    in->mode = GPU_MODE_SOLID;
}

static void emit_shape(struct gpu_build *b, struct rect2i scissor, struct rect2i shape,
                       const float color[4], float radius, enum gpu_shape_mode mode, uint32_t blur)
{
    struct gpu_cmd *c = cmd_open(b, GPU_CMD_SHAPE, GPU_ST_SHAPE, scissor, 0);
    struct gpu_shape_inst *in = stream_push(b, GPU_ST_SHAPE);
    if (!c || !in)
        return;
    c->count++;
    rect2i_to_vec4(shape, in->rect);
    memcpy(in->color, color, sizeof(in->color));
    in->radius = radius;
    in->mode = (uint32_t)mode;
    in->blur = blur;
}

/*
 * The slot of a UI image, or -1 when every slot holds an image of this
 * frame. A slot that this frame draws is never taken, because a command of
 * the frame points to it. A new image prefers the slot of the same pixel
 * buffer, so an image that changes in place keeps its slot and its texture.
 * An image with gen 0 has no identity between frames, and it loads again
 * in each frame.
 */
static int image_slot(struct gpu_build *b, const struct ui_image *image)
{
    struct gpu_image *victim = NULL;
    for (int i = 0; i < GPU_IMAGES; i++) {
        struct gpu_image *e = &b->images[i];
        bool same_buffer = e->pixels == image->pixels && vector2i_eq(e->size, image->size);
        if (same_buffer && e->gen == image->gen && (image->gen || e->frame == b->frame)) {
            e->used = ++b->image_clock;
            e->frame = b->frame;
            return i;
        }
        if (e->frame == b->frame)
            continue;
        bool victim_same = victim && victim->pixels == image->pixels &&
                           vector2i_eq(victim->size, image->size);
        if (!victim || (same_buffer && !victim_same) ||
            (same_buffer == victim_same && e->used < victim->used))
            victim = e;
    }
    if (!victim)
        return -1;
    *victim = (struct gpu_image){
        .pixels = image->pixels, .gen = image->gen, .size = image->size,
        .used = ++b->image_clock, .frame = b->frame, .dirty = true,
    };
    return (int)(victim - b->images);
}

static void scene_image(struct gpu_build *b, const struct ui_scene_op *op, struct rect2i clip,
                        struct rect2i canvas_rect)
{
    const struct ui_image *image = op->image;
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return;
    struct rect2i img = {
        .pos = op->rect.pos,
        .size = { .x = min_i(op->rect.width, image->size.x),
                  .y = min_i(op->rect.height, image->size.y) },
    };
    struct rect2i r = rect2i_intersect(img, clip);
    if (!rect2i_clip(&r, canvas_rect.size))
        return;
    int slot = image_slot(b, image);
    if (slot < 0) {
        b->failed = true;
        return;
    }
    struct gpu_cmd *c = cmd_open(b, GPU_CMD_GLYPH, GPU_ST_UI, canvas_rect, slot + 1);
    struct gpu_glyph_inst *in = push_glyph(b, GPU_ST_UI);
    if (!c || !in)
        return;
    c->count++;
    float sx = (float)(r.x - op->rect.x), sy = (float)(r.y - op->rect.y);
    rect2i_to_vec4(r, in->rect);
    rect_to_vec4(sx, sy, sx + (float)r.width, sy + (float)r.height, in->uv);
    white_of(1.0f, in->color);
    in->mode = GPU_MODE_IMAGE;
}

static void scene_text(struct gpu_build *b, struct font_instance *fi, const struct ui_scene *s,
                       const struct ui_scene_op *op, struct rect2i clip, struct rect2i canvas_rect)
{
    const char *text = ui_scene_op_text(s, op);
    if (!text || op->style.advance <= 0 || op->style.cell_h <= 0)
        return;
    clip = rect2i_intersect(clip, ui_scene_text_ink(s, op));
    if (!rect2i_clip(&clip, canvas_rect.size))
        return;
    const int max_rows = min_i(op->style.cell_h, font_instance_metrics(fi)->cell_h);
    const uint16_t flags = (uint16_t)(op->style.flags & (UI_TEXT_BOLD | UI_TEXT_ITALIC));
    const uint64_t version = font_instance_version(fi);
    float color[4];
    gpu_color(op->color, 1.0f, color);
    int64_t tx = op->rect.x;
    for (uint32_t i = 0; i < op->text_len;) {
        uint32_t cp;
        int cells;
        i += (uint32_t)ui_text_run_next(text + i, op->text_len - i, &cp, &cells);
        if (tx + 382 > clip.x) {
            uint64_t id = (uint64_t)cp | (uint64_t)flags << 32;
            struct gpu_atlas_entry e;
            const struct gpu_atlas_entry *hit = atlas_find(&b->atlas, id, version, SPACE_UI);
            if (hit)
                e = *hit;
            else if (!atlas_add_glyph(b, id, version, SPACE_UI,
                                      glyph_cache_fetch(fi, cp, flags), true, &e))
                return;
            int rows = min_i(e.rows, max_rows);
            if (e.kind != KIND_NONE && rows > 0) {
                bool subpx = e.kind == KIND_SUBPX;
                enum gpu_cmd_kind kind = subpx ? GPU_CMD_SUBPX : GPU_CMD_GLYPH;
                struct gpu_cmd *c = cmd_open(b, kind, GPU_ST_UI, clip, 0);
                struct gpu_glyph_inst *in = push_glyph(b, GPU_ST_UI);
                if (!c || !in)
                    return;
                c->count++;
                rect_to_vec4((float)(tx + e.x_offset), (float)op->rect.y, (float)e.w, (float)rows,
                             in->rect);
                rect_to_vec4((float)e.x, (float)e.y, (float)(e.x + e.w), (float)(e.y + rows),
                             in->uv);
                if (e.kind == KIND_COLOR) {
                    white_of(color[3], in->color);
                    in->mode = GPU_MODE_COLOR;
                } else {
                    memcpy(in->color, color, sizeof(in->color));
                    in->mode = subpx ? GPU_MODE_SUBPX : GPU_MODE_ALPHA;
                }
            }
        }
        tx += (int64_t)cells * op->style.advance;
        if (tx >= (int64_t)clip.x + clip.width + 128)
            break;
    }
}

static bool op_outside_clip(const struct ui_scene *s, const struct ui_scene_op *op,
                            struct rect2i clip)
{
    struct rect2i ink = op->kind == UI_SCENE_OP_TEXT ? ui_scene_text_ink(s, op) : op->rect;
    return rect2i_is_empty(rect2i_intersect(ink, clip));
}

static void scene_rect(struct gpu_build *b, struct rect2i rect, struct rect2i clip,
                       uint32_t color, float opacity_mult, struct rect2i canvas_rect)
{
    struct rect2i r = rect2i_intersect(rect, clip);
    if (!rect2i_clip(&r, canvas_rect.size))
        return;
    float c[4];
    gpu_color(color, opacity_mult, c);
    if (c[3] > 0.0f)
        emit_solid(b, r, c, canvas_rect);
}

static bool build_scene(struct gpu_build *b, struct font_instance *ufont,
                        const struct ui_scene *scene, struct rect2i canvas_rect)
{
    struct rect2i inline_stack[16];
    struct rect2i *stack = inline_stack;
    int stack_cap = (int)(sizeof(inline_stack) / sizeof(inline_stack[0]));
    if (scene->clips_cap > stack_cap) {
        if (scene->clips_cap > b->cap_clips) {
            struct rect2i *p = realloc(b->clips, (size_t)scene->clips_cap * sizeof(*p));
            if (!p) {
                b->failed = true;
                return false;
            }
            b->clips = p;
            b->cap_clips = scene->clips_cap;
        }
        stack = b->clips;
        stack_cap = b->cap_clips;
    }
    int depth = 0;
    struct rect2i clip = canvas_rect;

    for (int i = 0; i < scene->n_ops && !b->failed && !b->atlas.full; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        switch (op->kind) {
        case UI_SCENE_OP_PUSH_CLIP:
            if (depth < stack_cap)
                stack[depth++] = clip;
            clip = rect2i_intersect(clip, op->rect);
            break;
        case UI_SCENE_OP_POP_CLIP:
            if (depth > 0)
                clip = stack[--depth];
            break;
        case UI_SCENE_OP_RECT:
            if (!op_outside_clip(scene, op, clip))
                scene_rect(b, op->rect, clip, op->color, 1.0f, canvas_rect);
            break;
        case UI_SCENE_OP_BLEND_RECT:
            if (!op_outside_clip(scene, op, clip))
                scene_rect(b, op->rect, clip, op->color | 0xFF000000u, op->alpha / 255.0f,
                           canvas_rect);
            break;
        case UI_SCENE_OP_ROUNDED_RECT: {
            if (op_outside_clip(scene, op, clip))
                break;
            if (op->radius <= 0) {
                scene_rect(b, op->rect, clip, op->color, 1.0f, canvas_rect);
                break;
            }
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size) || !(op->color >> 24))
                break;
            int radius = min_i(op->radius, min_i(op->rect.width / 2, op->rect.height / 2));
            float c[4];
            gpu_color(op->color, 1.0f, c);
            emit_shape(b, visible, op->rect, c, (float)radius, GPU_SHAPE_ROUNDED, 0);
            break;
        }
        case UI_SCENE_OP_CIRCLE: {
            if (op_outside_clip(scene, op, clip) || op->radius <= 0 || !op->alpha)
                break;
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size))
                break;
            float c[4];
            gpu_color(op->color | 0xFF000000u, op->alpha / 255.0f, c);
            emit_shape(b, visible, op->rect, c, (float)op->radius, GPU_SHAPE_CIRCLE, 0);
            break;
        }
        case UI_SCENE_OP_SHADOW: {
            if (op_outside_clip(scene, op, clip) || op->blur <= 0 || !(op->color >> 24))
                break;
            int blur = min_i(op->blur, UI_SHADOW_MAX_BLUR);
            int radius = geom_clamp_i(op->radius, 0, UI_SHADOW_MAX_RADIUS);
            if (rect2i_is_empty(rect2i_inset(op->rect, ui_shadow_extent(blur))))
                break;
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size))
                break;
            float c[4];
            gpu_color(op->color, 1.0f, c);
            emit_shape(b, visible, op->rect, c, (float)radius, GPU_SHAPE_SHADOW, (uint32_t)blur);
            break;
        }
        case UI_SCENE_OP_TEXT:
            if (!op_outside_clip(scene, op, clip))
                scene_text(b, ufont, scene, op, clip, canvas_rect);
            break;
        case UI_SCENE_OP_IMAGE:
            if (!op_outside_clip(scene, op, clip))
                scene_image(b, op, clip, canvas_rect);
            break;
        }
    }
    return !b->failed && !b->atlas.full;
}

/* One try to build the frame. False with atlas.full set means: no room. */
static bool build_frame(struct gpu_build *b, const struct render_frame *frame,
                        struct vector2i canvas)
{
    begin_build(b);
    const struct rect2i canvas_rect = { .size = canvas };

    struct grid_geometry geo;
    if (grid_geometry_of(frame, canvas, &geo)) {
        if (!build_grid(b, &geo))
            return false;
        cmd_whole_stream(b, GPU_CMD_GLYPH, GPU_ST_BG, canvas_rect);
        cmd_whole_stream(b, GPU_CMD_GLYPH, GPU_ST_INK, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
        cmd_whole_stream(b, GPU_CMD_SUBPX, GPU_ST_SUBPX, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
        cmd_whole_stream(b, GPU_CMD_GLYPH, GPU_ST_DECO, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
    }
    const struct ui_scene *scene = frame->ui;
    if (scene && ui_scene_is_valid(scene)) {
        struct font_instance *ufont =
            frame->ui_font ? frame->ui_font : font_instance_default();
        if (ufont && !build_scene(b, ufont, scene, canvas_rect))
            return false;
    }
    return !b->failed && !b->atlas.full;
}

/* ---- The public calls --------------------------------------------------- */

bool gpu_build_init(struct gpu_build *b)
{
    b->atlas.size = GPU_ATLAS_START_SIZE;
    b->atlas.map = calloc(ATLAS_MAP_SIZE, sizeof(*b->atlas.map));
    b->atlas.shelves = calloc(GPU_SHELVES_MAX, sizeof(*b->atlas.shelves));
    b->decor = decor_cache_create();
    return b->atlas.map && b->atlas.shelves && b->decor;
}

void gpu_build_release(struct gpu_build *b)
{
    for (int i = 0; i < GPU_ST_COUNT; i++)
        free(b->streams[i].data);
    free(b->cmds);
    free(b->clips);
    free(b->atlas.map);
    free(b->atlas.shelves);
    free(b->atlas.stage);
    free(b->atlas.uploads);
    decor_cache_destroy(b->decor);
    *b = (struct gpu_build){0};
}

bool gpu_build_frame(struct gpu_build *b, const struct render_frame *frame,
                     struct vector2i canvas)
{
    b->frame++;
    bool built = build_frame(b, frame, canvas);
    if (!built && b->atlas.full) {
        /* A full atlas gets one retry: it forgets every glyph and doubles
         * its size, up to the maximum. */
        if (b->atlas.size < GPU_ATLAS_MAX_SIZE)
            b->atlas.size *= 2;
        atlas_clear(&b->atlas);
        built = build_frame(b, frame, canvas);
    }
    glyph_cache_flush_frame();
    if (!built)
        gpu_build_abort(b);
    return built;
}

void gpu_build_commit(struct gpu_build *b)
{
    b->atlas.stage_used = 0;
    b->atlas.n_uploads = 0;
    for (int i = 0; i < GPU_IMAGES; i++)
        if (b->images[i].frame == b->frame)
            b->images[i].dirty = false;
}

void gpu_build_abort(struct gpu_build *b)
{
    atlas_clear(&b->atlas);
}

size_t gpu_build_instance_layout(const struct gpu_build *b, size_t offsets[GPU_ST_COUNT])
{
    size_t total = 0;
    for (int i = 0; i < GPU_ST_COUNT; i++) {
        offsets[i] = total;
        size_t bytes = (size_t)b->streams[i].n * gpu_stream_stride(i);
        total += (bytes + GPU_STREAM_ALIGN - 1) & ~(size_t)(GPU_STREAM_ALIGN - 1);
    }
    return total ? total : GPU_STREAM_ALIGN;
}

void gpu_build_copy_instances(const struct gpu_build *b, const size_t offsets[GPU_ST_COUNT],
                              void *dst)
{
    for (int i = 0; i < GPU_ST_COUNT; i++)
        if (b->streams[i].n)
            memcpy((uint8_t *)dst + offsets[i], b->streams[i].data,
                   (size_t)b->streams[i].n * gpu_stream_stride(i));
}

/* ---- Presenting CPU pixels ---------------------------------------------- */

int gpu_pixels_rects(const struct framebuffer *fb, const struct damage_list *damage, bool whole,
                     struct rect2i *rects, size_t *bytes)
{
    int n = 0;
    if (whole || !damage || damage->all) {
        rects[n++] = (struct rect2i){ .size = fb->size };
    } else if (damage->n < 0) {
        rects[n] = damage->bounds;
        if (rect2i_clip(&rects[n], fb->size))
            n++;
    } else {
        for (int i = 0; i < damage->n && i < DAMAGE_RECTS_MAX; i++) {
            rects[n] = damage->rects[i];
            if (rect2i_clip(&rects[n], fb->size))
                n++;
        }
    }
    *bytes = 0;
    for (int i = 0; i < n; i++)
        *bytes += (size_t)rects[i].width * (size_t)rects[i].height * 4;
    return n;
}

void gpu_pixels_pack(const struct framebuffer *fb, const struct rect2i *rects, int n, void *dst)
{
    uint8_t *out = dst;
    for (int i = 0; i < n; i++) {
        const struct rect2i *r = &rects[i];
        size_t row_bytes = (size_t)r->width * 4;
        const uint32_t *src = fb->pixels + (size_t)r->y * (size_t)fb->size.x + (size_t)r->x;
        if (r->width == fb->size.x) {
            /* Rows of the full width have no gap between them. */
            memcpy(out, src, row_bytes * (size_t)r->height);
            out += row_bytes * (size_t)r->height;
            continue;
        }
        for (int y = 0; y < r->height; y++, out += row_bytes, src += fb->size.x)
            memcpy(out, src, row_bytes);
    }
}

void gpu_pixels_quad(struct vector2i size, struct gpu_glyph_inst *quad)
{
    *quad = (struct gpu_glyph_inst){ .mode = GPU_MODE_IMAGE };
    rect_to_vec4(0, 0, (float)size.x, (float)size.y, quad->rect);
    rect_to_vec4(0, 0, (float)size.x, (float)size.y, quad->uv);
    white_of(1.0f, quad->color);
}
