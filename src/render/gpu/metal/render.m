#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>
#import <simd/simd.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
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
#include "render/gpu/gpu.h"
#include "render/render.h"
#include "render/surface.h"
#include "util/damage.h"
#include "util/geom.h"
#include "util/hash.h"

#include "metal_shader_source.h"

/*
 * The Metal backend. See render/gpu/gpu.h for the rules of a backend.
 *
 * One frame is one command buffer. render_gpu_draw() builds the instance
 * lists of the whole frame on the CPU, then one render pass draws them:
 *
 *   1. the grid area in default_bg, and the cell backgrounds that differ;
 *   2. the glyphs of the grid, clipped to the cell area;
 *   3. the decorations and cursor marks of the grid;
 *   4. the UI scene ops, in scene order, each with its own scissor.
 *
 * Glyph pixels live in one atlas texture. A glyph enters the atlas once,
 * under its glyph id and font version (render/frame/walk.h). A new glyph is
 * staged in memory, and the command buffer blits it into the atlas before
 * the render pass. The frame owns a slot of three: its instance buffer, its
 * upload buffer, and a semaphore that the command buffer signals.
 *
 * Failure. The build of a frame can fail on memory, and on a full atlas. A
 * full atlas gets one retry: the atlas resets, and its texture doubles in
 * size up to the maximum. Any other failure resets the atlas too, because
 * the map can hold glyphs that were staged but never uploaded. The frame is
 * then RENDER_GPU_UNUSED, and the CPU paints it.
 *
 * Blending. All pipelines write premultiplied color and blend in the
 * sRGB space of the drawable. Subpixel glyphs draw in three passes, one for
 * each color write mask.
 */

#define FRAME_SLOTS 3
#define ATLAS_START_SIZE 1024
#define ATLAS_MAX_SIZE 4096
#define ATLAS_MAP_SIZE (1u << 15)
#define ATLAS_MAP_LIMIT (ATLAS_MAP_SIZE / 2)
#define SHELVES_MAX 1024
#define IMAGE_CACHE 4
#define STREAM_ALIGN 256
#define BUFFER_MIN 65536

/* The glyph instance modes and the shape instance modes of shaders.metal. */
enum glyph_mode {
    MODE_SOLID = 0,
    MODE_ALPHA = 1,
    MODE_COLOR = 2,
    MODE_IMAGE = 3,
    MODE_SUBPX = 4,
};

enum shape_mode {
    SHAPE_ROUNDED = 1,
    SHAPE_CIRCLE = 2,
    SHAPE_SHADOW = 3,
};

struct glyph_inst {
    simd_float4 rect;
    simd_float4 uv;
    simd_float4 color;
    uint32_t mode;
};

struct shape_inst {
    simd_float4 rect;
    simd_float4 color;
    float radius;
    uint32_t mode;
    uint32_t blur;
};

struct uniforms {
    simd_float2 viewport_size;
};

_Static_assert(sizeof(struct glyph_inst) == 64, "GlyphInstance of shaders.metal is 64 bytes");
_Static_assert(sizeof(struct shape_inst) == 48, "ShapeInstance of shaders.metal is 48 bytes");

/* ---- Streams and commands ---------------------------------------------- */

/*
 * An instance stream is a list of instances of one type. The grid fills the
 * first four. The UI fills the last two, in the order of the scene.
 */
enum stream_id {
    ST_BG,
    ST_INK,
    ST_SUBPX,
    ST_DECO,
    ST_UI,
    ST_SHAPE,
    ST_COUNT,
};

static const size_t STREAM_STRIDE[ST_COUNT] = {
    sizeof(struct glyph_inst), sizeof(struct glyph_inst), sizeof(struct glyph_inst),
    sizeof(struct glyph_inst), sizeof(struct glyph_inst), sizeof(struct shape_inst),
};

struct stream {
    uint8_t *data;
    uint32_t n, cap;
};

enum cmd_kind {
    CMD_GLYPH,
    CMD_SUBPX,
    CMD_SHAPE,
};

/* One draw: `count` instances of `stream` from `start`, inside `scissor`. */
struct cmd {
    enum cmd_kind kind;
    enum stream_id stream;
    struct rect2i scissor;
    uint32_t start, count;
    id<MTLTexture> image;
};

/* ---- The atlas ---------------------------------------------------------- */

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
struct atlas_entry {
    uint64_t id, version;
    uint32_t space;
    uint16_t x, y, w, rows;
    int16_t x_offset;
    uint8_t kind;
    bool used;
};

struct shelf {
    uint16_t y, h, x;
};

/* A staged rect: where it goes in the atlas, and where its texels wait. */
struct upload {
    uint16_t x, y, w, h;
    size_t offset;
};

struct atlas {
    id<MTLTexture> texture;
    int size;
    struct atlas_entry *map;
    uint32_t count;
    struct shelf shelves[SHELVES_MAX];
    int n_shelves, next_y;
    uint8_t *stage;
    size_t stage_used, stage_cap;
    struct upload *uploads;
    uint32_t n_uploads, cap_uploads;
    /* The build of the frame ran out of room. */
    bool full;
};

/* ---- The renderer ------------------------------------------------------- */

struct frame_slot {
    id<MTLBuffer> instances;
    id<MTLBuffer> upload;
    dispatch_semaphore_t ready;
};

struct image_entry {
    id<MTLTexture> texture;
    const uint32_t *pixels;
    uint64_t gen;
    struct vector2i size;
    uint64_t used;
};

struct render_gpu {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> glyph_pipeline;
    id<MTLRenderPipelineState> subpx_pipeline[3];
    id<MTLRenderPipelineState> shape_pipeline;
    id<MTLRenderPipelineState> copy_pipeline;

    struct frame_slot slots[FRAME_SLOTS];
    unsigned next_slot;

    struct atlas atlas;
    struct decor_cache *decor;
    struct image_entry images[IMAGE_CACHE];
    uint64_t image_clock;

    struct stream streams[ST_COUNT];
    struct cmd *cmds;
    uint32_t n_cmds, cap_cmds;
    struct rect2i *clips;
    int cap_clips;
    /* The build ran out of memory. */
    bool failed;
};

/* The state of one render surface: the CPU pixels, on the GPU. */
struct gpu_surface {
    id<MTLTexture> pixels;
    struct vector2i size;
};

static inline simd_float4 color_of(uint32_t argb, float opacity)
{
    return simd_make_float4(((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
                            (argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f * opacity);
}

/* A grid color. The grid is opaque, whatever the alpha of the color says. */
static inline simd_float4 opaque_color_of(uint32_t argb, float opacity)
{
    return color_of(argb | 0xFF000000u, opacity);
}

static inline simd_float4 rect_of(float x, float y, float w, float h)
{
    return simd_make_float4(x, y, w, h);
}

static inline simd_float4 rect2i_to_float4(struct rect2i r)
{
    return rect_of((float)r.x, (float)r.y, (float)r.width, (float)r.height);
}

static inline int min_i(int a, int b)
{
    return a < b ? a : b;
}

static inline int max_i(int a, int b)
{
    return a > b ? a : b;
}

/* ---- Instance streams --------------------------------------------------- */

static void *stream_push(struct render_gpu *g, enum stream_id id)
{
    struct stream *s = &g->streams[id];
    size_t stride = STREAM_STRIDE[id];
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 256;
        void *p = realloc(s->data, (size_t)cap * stride);
        if (!p) {
            g->failed = true;
            return NULL;
        }
        s->data = p;
        s->cap = cap;
    }
    void *e = s->data + (size_t)s->n++ * stride;
    memset(e, 0, stride);
    return e;
}

static struct glyph_inst *push_glyph(struct render_gpu *g, enum stream_id id)
{
    return stream_push(g, id);
}

/* Adds a command for the instances that the stream holds now, or extends the last. */
static struct cmd *cmd_open(struct render_gpu *g, enum cmd_kind kind, enum stream_id stream,
                            struct rect2i scissor, id<MTLTexture> image)
{
    uint32_t next = g->streams[stream].n;
    if (g->n_cmds) {
        struct cmd *last = &g->cmds[g->n_cmds - 1];
        if (last->kind == kind && last->stream == stream && last->image == image &&
            rect2i_eq(last->scissor, scissor) && last->start + last->count == next)
            return last;
    }
    if (g->n_cmds == g->cap_cmds) {
        uint32_t cap = g->cap_cmds ? g->cap_cmds * 2 : 64;
        struct cmd *p = realloc(g->cmds, (size_t)cap * sizeof(*p));
        if (!p) {
            g->failed = true;
            return NULL;
        }
        g->cmds = p;
        g->cap_cmds = cap;
    }
    struct cmd *c = &g->cmds[g->n_cmds++];
    *c = (struct cmd){
        .kind = kind, .stream = stream, .scissor = scissor, .start = next,
        .image = [image retain],
    };
    return c;
}

/* A command for every instance that the stream holds. For the grid streams. */
static void cmd_whole_stream(struct render_gpu *g, enum cmd_kind kind, enum stream_id stream,
                             struct rect2i scissor)
{
    uint32_t n = g->streams[stream].n;
    if (!n)
        return;
    struct cmd *c = cmd_open(g, kind, stream, scissor, nil);
    if (!c)
        return;
    c->start = 0;
    c->count = n;
}

static void begin_build(struct render_gpu *g)
{
    for (int i = 0; i < ST_COUNT; i++)
        g->streams[i].n = 0;
    for (uint32_t i = 0; i < g->n_cmds; i++)
        [g->cmds[i].image release];
    g->n_cmds = 0;
    g->atlas.stage_used = 0;
    g->atlas.n_uploads = 0;
    g->atlas.full = false;
    g->failed = false;
}

/* ---- The atlas ---------------------------------------------------------- */

static bool atlas_make_texture(struct render_gpu *g, int size)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)size
                                                          height:(NSUInteger)size
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g->device newTextureWithDescriptor:d];
    if (!t)
        return false;
    [g->atlas.texture release];
    g->atlas.texture = t;
    g->atlas.size = size;
    return true;
}

static void atlas_clear(struct atlas *a)
{
    memset(a->map, 0, (size_t)ATLAS_MAP_SIZE * sizeof(*a->map));
    a->count = 0;
    a->n_shelves = 0;
    a->next_y = 0;
    a->stage_used = 0;
    a->n_uploads = 0;
    a->full = false;
}

/* Forgets every glyph. In-flight frames keep the texture that they use. */
static void atlas_reset(struct render_gpu *g)
{
    atlas_clear(&g->atlas);
}

/* Resets the atlas and doubles its texture, up to the maximum size. Returns
 * false when the texture fails. */
static bool atlas_recycle(struct render_gpu *g)
{
    struct atlas *a = &g->atlas;
    if (a->size < ATLAS_MAX_SIZE && !atlas_make_texture(g, a->size * 2))
        return false;
    atlas_clear(a);
    return true;
}

static uint64_t atlas_hash(const struct atlas_entry *k)
{
    uint64_t h = hash64_finish(k->id * UINT64_C(0x9E3779B97F4A7C15) ^ k->version);
    return hash64_finish(h ^ ((uint64_t)k->space << 56) ^ UINT64_C(0xA5A5A5A5));
}

static bool atlas_key_eq(const struct atlas_entry *e, const struct atlas_entry *k)
{
    return e->id == k->id && e->version == k->version && e->space == k->space;
}

static const struct atlas_entry *atlas_find(const struct atlas *a, uint64_t id, uint64_t version,
                                            enum atlas_space space)
{
    struct atlas_entry key = { .id = id, .version = version, .space = space };
    for (uint32_t i = (uint32_t)atlas_hash(&key) & (ATLAS_MAP_SIZE - 1);;
         i = (i + 1) & (ATLAS_MAP_SIZE - 1)) {
        const struct atlas_entry *e = &a->map[i];
        if (!e->used)
            return NULL;
        if (atlas_key_eq(e, &key))
            return e;
    }
}

static bool atlas_insert(struct atlas *a, const struct atlas_entry *entry)
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
static bool atlas_place(struct atlas *a, int w, int h, uint16_t *x, uint16_t *y)
{
    int quantized = (h + 3) & ~3;
    for (int i = 0; i < a->n_shelves; i++) {
        struct shelf *s = &a->shelves[i];
        if (s->h >= h && s->h <= quantized + quantized / 2 && s->x + w <= a->size) {
            *x = s->x;
            *y = s->y;
            s->x = (uint16_t)(s->x + w);
            return true;
        }
    }
    if (a->n_shelves == SHELVES_MAX || a->next_y + quantized > a->size)
        return false;
    struct shelf *s = &a->shelves[a->n_shelves++];
    *s = (struct shelf){ .y = (uint16_t)a->next_y, .h = (uint16_t)quantized, .x = (uint16_t)w };
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
static uint32_t *atlas_stage(struct render_gpu *g, int w, int h, uint16_t *x, uint16_t *y)
{
    struct atlas *a = &g->atlas;
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
            g->failed = true;
            return NULL;
        }
        a->stage = p;
        a->stage_cap = cap;
    }
    if (a->n_uploads == a->cap_uploads) {
        uint32_t cap = a->cap_uploads ? a->cap_uploads * 2 : 128;
        struct upload *p = realloc(a->uploads, (size_t)cap * sizeof(*p));
        if (!p) {
            g->failed = true;
            return NULL;
        }
        a->uploads = p;
        a->cap_uploads = cap;
    }
    a->uploads[a->n_uploads++] = (struct upload){
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
static bool atlas_add_glyph(struct render_gpu *g, uint64_t id, uint64_t version,
                            enum atlas_space space, const struct glyph *gl, bool cache,
                            struct atlas_entry *out)
{
    enum glyph_kind kind = glyph_kind_of(gl);
    struct atlas_entry e = { .id = id, .version = version, .space = space, .kind = kind };
    if (kind != KIND_NONE) {
        int w = gl->width, rows = gl->height;
        uint32_t *dst = atlas_stage(g, w, rows, &e.x, &e.y);
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
    return !cache || atlas_insert(&g->atlas, &e);
}

/* Puts a coverage mask in the atlas. `stride` is the bytes from row to row. */
static bool atlas_add_mask(struct render_gpu *g, uint64_t id, uint64_t version,
                           enum atlas_space space, const uint8_t *cov, int w, int rows,
                           int stride, struct atlas_entry *out)
{
    struct atlas_entry e = {
        .id = id, .version = version, .space = space, .kind = KIND_ALPHA,
        .w = (uint16_t)w, .rows = (uint16_t)rows,
    };
    uint32_t *dst = atlas_stage(g, w, rows, &e.x, &e.y);
    if (!dst)
        return false;
    for (int y = 0; y < rows; y++, dst += w)
        for (int x = 0; x < w; x++)
            dst[x] = (uint32_t)cov[(size_t)y * (size_t)stride + (size_t)x] << 24;
    *out = e;
    return atlas_insert(&g->atlas, &e);
}

/* ---- The grid ----------------------------------------------------------- */

/* The part of a render_frame that the grid draws. */
struct grid_geometry {
    const struct terminal_frame *tf;
    struct font_instance *font;
    struct rect2i rect;
    int cw, ch, cols, rows;
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
struct grid_build {
    struct render_gpu *g;
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
    int run_x0, run_x1;
    uint32_t run_bg;
};

static void bg_flush(struct grid_build *b)
{
    if (!b->run_open)
        return;
    b->run_open = false;
    struct glyph_inst *in = push_glyph(b->g, ST_BG);
    if (!in)
        return;
    const struct grid_geometry *geo = b->geo;
    int cell_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE) ? 2 * geo->cw : geo->cw;
    in->rect = rect_of((float)(geo->rect.x + b->run_x0 * cell_w),
                       (float)(geo->rect.y + b->row * geo->ch),
                       (float)((b->run_x1 - b->run_x0) * cell_w), (float)geo->ch);
    in->color = opaque_color_of(b->run_bg, 1.0f);
    in->mode = MODE_SOLID;
}

/* Adds the backgrounds of columns [c0, c1). The grid area is in default_bg already. */
static void bg_add(struct grid_build *b, int c0, int c1, uint32_t bg)
{
    bg |= 0xFF000000u;
    if (bg == b->default_bg) {
        bg_flush(b);
        return;
    }
    if (b->run_open && b->run_x1 == c0 && b->run_bg == bg) {
        b->run_x1 = c1;
        return;
    }
    bg_flush(b);
    b->run_open = true;
    b->run_x0 = c0;
    b->run_x1 = c1;
    b->run_bg = bg;
}

static void grid_run(void *ctx, int c0, int c1, uint32_t bg)
{
    bg_add(ctx, c0, c1, bg);
}

/* One quad of the glyph in the atlas: its columns [c0, c1), with column c0 at dest_x. */
static void emit_glyph_part(struct grid_build *b, int col, const struct atlas_entry *e, int c0,
                            int c1, int dest_x, uint32_t fg, bool faint)
{
    const struct grid_geometry *geo = b->geo;
    int rows = min_i(e->rows, geo->ch);
    if (c1 <= c0 || rows <= 0)
        return;
    (void)col;
    bool subpx = e->kind == KIND_SUBPX;
    struct glyph_inst *in = push_glyph(b->g, subpx ? ST_SUBPX : ST_INK);
    if (!in)
        return;
    bool double_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE);
    bool double_h = (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP ||
                     b->line_size == WSAP_VTCORE_LINE_DOUBLE_BOTTOM);
    float width = (float)(c1 - c0);
    float height = (float)rows;
    float v0 = (float)e->y;
    float v1 = (float)(e->y + rows);
    if (double_w)
        width *= 2.0f;
    if (double_h) {
        int half_h = geo->ch / 2;
        if (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP) {
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
    in->rect = rect_of((float)dest_x, (float)(geo->rect.y + b->row * geo->ch),
                       width, height);
    in->uv = simd_make_float4((float)(e->x + c0), v0, (float)(e->x + c1), v1);
    float opacity = faint ? 128.0f / 255.0f : 1.0f;
    if (e->kind == KIND_COLOR) {
        in->color = simd_make_float4(1, 1, 1, opacity);
        in->mode = MODE_COLOR;
    } else {
        in->color = opaque_color_of(fg, opacity);
        in->mode = subpx ? MODE_SUBPX : MODE_ALPHA;
    }
}

/*
 * One decoration mask over a cell. A solid mask is one rect. The others come
 * from the atlas. `index` names the mask in the set.
 */
static void emit_mask(struct grid_build *b, int col, const struct decor_mask *m, int index,
                      uint32_t color, bool faint)
{
    if (!m || m->y0 >= m->y1)
        return;
    const struct grid_geometry *geo = b->geo;
    bool double_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE);
    bool double_h = (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP ||
                     b->line_size == WSAP_VTCORE_LINE_DOUBLE_BOTTOM);
    int cell_w = double_w ? 2 * geo->cw : geo->cw;
    int rows = m->y1 - m->y0;
    int y0 = m->y0;
    float dest_y = (float)(geo->rect.y + b->row * geo->ch);
    float height = (float)rows;
    if (double_h) {
        int half_h = geo->ch / 2;
        if (b->line_size == WSAP_VTCORE_LINE_DOUBLE_TOP) {
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
    struct glyph_inst *in = push_glyph(b->g, ST_DECO);
    if (!in)
        return;
    in->rect = rect_of((float)(geo->rect.x + col * cell_w),
                       dest_y, (float)cell_w, height);
    in->color = opaque_color_of(color, faint ? 128.0f / 255.0f : 1.0f);
    if (m->solid) {
        in->mode = MODE_SOLID;
        return;
    }
    struct atlas_entry e;
    const struct atlas_entry *hit = atlas_find(&b->g->atlas, (uint64_t)index, b->decor_version,
                                               SPACE_DECOR);
    if (hit) {
        e = *hit;
    } else if (!atlas_add_mask(b->g, (uint64_t)index, b->decor_version, SPACE_DECOR,
                               m->cov + (size_t)m->y0 * (size_t)geo->cw, geo->cw, rows, geo->cw,
                               &e)) {
        return;
    }
    in->mode = MODE_ALPHA;
    in->uv = simd_make_float4((float)e.x, (float)e.y, (float)(e.x + geo->cw),
                              (float)(e.y + rows));
}

/* The mask indexes of a decor set. Underline kinds use their own value. */
enum {
    MASK_STRIKE = 16,
    MASK_OVERLINE,
    MASK_CURSOR_BAR,
    MASK_CURSOR_UNDERLINE,
};

static void emit_decor(struct grid_build *b, int col, const struct render_cell *cell)
{
    const struct decor_set *dc = b->decor;
    uint16_t fl = cell->flags;
    if (!dc || !(fl & (RENDER_CELL_DECOR_MASK | RENDER_CELL_CURSOR_MASK)))
        return;
    bool faint = (fl & RENDER_CELL_FAINT) != 0;
    uint32_t ul = (fl & RENDER_CELL_UL_COLOR) ? cell->ul : cell->fg;
    unsigned underline = render_cell_underline(fl);
    if (underline && underline < DECOR_UNDERLINE_KINDS)
        emit_mask(b, col, &dc->underline[underline], (int)underline, ul, faint);
    if (fl & RENDER_CELL_STRIKE)
        emit_mask(b, col, &dc->strike, MASK_STRIKE, cell->fg, faint);
    if (fl & RENDER_CELL_OVERLINE)
        emit_mask(b, col, &dc->overline, MASK_OVERLINE, cell->fg, faint);
    if (fl & RENDER_CELL_CURSOR_BAR)
        emit_mask(b, col, &dc->cursor_bar, MASK_CURSOR_BAR, b->cursor_color, false);
    if (fl & RENDER_CELL_CURSOR_UNDERLINE)
        emit_mask(b, col, &dc->cursor_underline, MASK_CURSOR_UNDERLINE, b->cursor_color, false);
}

/*
 * One cell: its background, its glyph, and its marks. A wide glyph draws in
 * two parts. The left half takes the columns of the glyph up to the cell
 * edge, and the right half takes the rest. Each half has the colors of its
 * own cell. Ink that leaves the cell area is cut by the scissor.
 */
static void grid_cell(void *ctx, int col, const struct render_cell *cell,
                      const struct render_glyph_ref *ref)
{
    struct grid_build *b = ctx;
    const struct grid_geometry *geo = b->geo;
    if (b->g->failed || b->g->atlas.full)
        return;
    bg_add(b, col, col + 1, cell->bg);
    uint16_t fl = cell->flags;
    if (!(fl & RENDER_CELL_BLANK)) {
        struct atlas_entry e;
        const struct atlas_entry *hit =
            ref->id ? atlas_find(&b->g->atlas, ref->id, b->font_version, SPACE_GRID) : NULL;
        if (hit) {
            e = *hit;
        } else if (!atlas_add_glyph(b->g, ref->id, b->font_version, SPACE_GRID,
                                    render_glyph_get(ref), ref->id != 0, &e)) {
            return;
        }
        if (e.kind != KIND_NONE) {
            bool double_w = (b->line_size != WSAP_VTCORE_LINE_SINGLE);
            int cell_w = double_w ? 2 * geo->cw : geo->cw;
            int cell_x = geo->rect.x + col * cell_w;
            int x_off = double_w ? 2 * e.x_offset : e.x_offset;
            int split = geo->cw - e.x_offset;
            split = split < 0 ? 0 : split > e.w ? e.w : split;
            bool faint = (fl & RENDER_CELL_FAINT) != 0;
            if (fl & RENDER_CELL_WIDE_TAIL)
                emit_glyph_part(b, col, &e, split, e.w,
                                double_w ? cell_x + 2 * (split - (geo->cw - e.x_offset))
                                         : cell_x + split - (geo->cw - e.x_offset),
                                cell->fg, faint);
            else if (fl & RENDER_CELL_WIDE)
                emit_glyph_part(b, col, &e, 0, split, cell_x + x_off, cell->fg, faint);
            else
                emit_glyph_part(b, col, &e, 0, e.w, cell_x + x_off, cell->fg, faint);
        }
    }
    emit_decor(b, col, cell);
}

static bool build_grid(struct render_gpu *g, const struct grid_geometry *geo)
{
    const struct terminal_frame *tf = geo->tf;
    struct grid_build b = {
        .g = g,
        .geo = geo,
        .decor = decor_get(g->decor, font_instance_metrics(geo->font)),
        .font_version = font_instance_version(geo->font),
        .default_bg = tf->default_bg | 0xFF000000u,
        .cursor_color = tf->cursor_color,
    };
    struct decor_key dk = decor_key_of(font_instance_metrics(geo->font));
    b.decor_version = hash64_finish(((uint64_t)(uint32_t)dk.cw << 32) | (uint32_t)dk.ch) ^
                      hash64_finish(((uint64_t)(uint32_t)dk.baseline << 32) |
                                    (uint32_t)dk.thickness);

    /* The whole grid area first: the margins, and every cell with the default bg. */
    struct glyph_inst *fill = push_glyph(g, ST_BG);
    if (!fill)
        return false;
    fill->rect = rect2i_to_float4(geo->rect);
    fill->color = opaque_color_of(b.default_bg, 1.0f);
    fill->mode = MODE_SOLID;

    const struct render_walk_sink sink = { .cell = grid_cell, .run = grid_run, .ctx = &b };
    (void)render_walk_thread_reserve(tf->cols);
    glyph_cache_set_parallel(true);
    for (int row = 0; row < geo->rows && !g->failed && !g->atlas.full; row++) {
        b.row = row;
        b.line_size = terminal_frame_row_size(tf, row);
        int cols = (b.line_size != WSAP_VTCORE_LINE_SINGLE)
            ? (geo->cols / 2 > 0 ? geo->cols / 2 : 1) : geo->cols;
        render_row_walk(geo->font, tf, row, 0, cols, RENDER_WALK_PEEK, &sink);
        bg_flush(&b);
    }
    glyph_cache_set_parallel(false);
    return !g->failed && !g->atlas.full;
}

/* ---- The UI scene ------------------------------------------------------- */

static void emit_solid(struct render_gpu *g, struct rect2i r, simd_float4 color,
                       struct rect2i canvas_rect)
{
    struct cmd *c = cmd_open(g, CMD_GLYPH, ST_UI, canvas_rect, nil);
    struct glyph_inst *in = push_glyph(g, ST_UI);
    if (!c || !in)
        return;
    c->count++;
    in->rect = rect2i_to_float4(r);
    in->color = color;
    in->mode = MODE_SOLID;
}

static void emit_shape(struct render_gpu *g, struct rect2i scissor, struct rect2i shape,
                       simd_float4 color, float radius, enum shape_mode mode, uint32_t blur)
{
    struct cmd *c = cmd_open(g, CMD_SHAPE, ST_SHAPE, scissor, nil);
    struct shape_inst *in = stream_push(g, ST_SHAPE);
    if (!c || !in)
        return;
    c->count++;
    in->rect = rect2i_to_float4(shape);
    in->color = color;
    in->radius = radius;
    in->mode = mode;
    in->blur = blur;
}

static id<MTLTexture> image_texture(struct render_gpu *g, const struct ui_image *image)
{
    struct image_entry *victim = &g->images[0];
    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct image_entry *e = &g->images[i];
        if (e->texture && image->gen && e->pixels == image->pixels && e->gen == image->gen &&
            vector2i_eq(e->size, image->size)) {
            e->used = ++g->image_clock;
            return e->texture;
        }
        if (e->used < victim->used)
            victim = e;
    }
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)image->size.x
                                                          height:(NSUInteger)image->size.y
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g->device newTextureWithDescriptor:d];
    if (!t)
        return nil;
    [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->size.x, (NSUInteger)image->size.y)
         mipmapLevel:0
           withBytes:image->pixels
         bytesPerRow:(NSUInteger)image->size.x * 4];
    [victim->texture release];
    *victim = (struct image_entry){
        .texture = t, .pixels = image->pixels, .gen = image->gen, .size = image->size,
        .used = ++g->image_clock,
    };
    return t;
}

static void scene_image(struct render_gpu *g, const struct ui_scene_op *op, struct rect2i clip,
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
    id<MTLTexture> t = image_texture(g, image);
    if (!t) {
        g->failed = true;
        return;
    }
    struct cmd *c = cmd_open(g, CMD_GLYPH, ST_UI, canvas_rect, t);
    struct glyph_inst *in = push_glyph(g, ST_UI);
    if (!c || !in)
        return;
    c->count++;
    float sx = (float)(r.x - op->rect.x), sy = (float)(r.y - op->rect.y);
    in->rect = rect2i_to_float4(r);
    in->uv = simd_make_float4(sx, sy, sx + (float)r.width, sy + (float)r.height);
    in->color = simd_make_float4(1, 1, 1, 1);
    in->mode = MODE_IMAGE;
}

static void scene_text(struct render_gpu *g, struct font_instance *fi, const struct ui_scene *s,
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
    const simd_float4 color = color_of(op->color, 1.0f);
    int64_t tx = op->rect.x;
    for (uint32_t i = 0; i < op->text_len;) {
        uint32_t cp;
        int cells;
        i += (uint32_t)ui_text_run_next(text + i, op->text_len - i, &cp, &cells);
        if (tx + 382 > clip.x) {
            uint64_t id = (uint64_t)cp | (uint64_t)flags << 32;
            struct atlas_entry e;
            const struct atlas_entry *hit = atlas_find(&g->atlas, id, version, SPACE_UI);
            if (hit)
                e = *hit;
            else if (!atlas_add_glyph(g, id, version, SPACE_UI,
                                      glyph_cache_fetch(fi, cp, flags), true, &e))
                return;
            int rows = min_i(e.rows, max_rows);
            if (e.kind != KIND_NONE && rows > 0) {
                bool subpx = e.kind == KIND_SUBPX;
                struct cmd *c = cmd_open(g, subpx ? CMD_SUBPX : CMD_GLYPH, ST_UI, clip, nil);
                struct glyph_inst *in = push_glyph(g, ST_UI);
                if (!c || !in)
                    return;
                c->count++;
                in->rect = rect_of((float)(tx + e.x_offset), (float)op->rect.y, (float)e.w,
                                   (float)rows);
                in->uv = simd_make_float4((float)e.x, (float)e.y, (float)(e.x + e.w),
                                          (float)(e.y + rows));
                if (e.kind == KIND_COLOR) {
                    in->color = simd_make_float4(1, 1, 1, color.w);
                    in->mode = MODE_COLOR;
                } else {
                    in->color = color;
                    in->mode = subpx ? MODE_SUBPX : MODE_ALPHA;
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

/* A rect op: a filled rect with a color and an opacity, cut to the clip. */
static void scene_rect(struct render_gpu *g, struct rect2i rect, struct rect2i clip,
                       uint32_t color, float opacity_mult, struct rect2i canvas_rect)
{
    struct rect2i r = rect2i_intersect(rect, clip);
    if (!rect2i_clip(&r, canvas_rect.size))
        return;
    simd_float4 c = color_of(color, opacity_mult);
    if (c.w > 0.0f)
        emit_solid(g, r, c, canvas_rect);
}

static bool build_scene(struct render_gpu *g, struct font_instance *ufont,
                        const struct ui_scene *scene, struct rect2i canvas_rect)
{
    struct rect2i inline_stack[16];
    struct rect2i *stack = inline_stack;
    int stack_cap = (int)(sizeof(inline_stack) / sizeof(inline_stack[0]));
    if (scene->clips_cap > stack_cap) {
        if (scene->clips_cap > g->cap_clips) {
            struct rect2i *p = realloc(g->clips, (size_t)scene->clips_cap * sizeof(*p));
            if (!p) {
                g->failed = true;
                return false;
            }
            g->clips = p;
            g->cap_clips = scene->clips_cap;
        }
        stack = g->clips;
        stack_cap = g->cap_clips;
    }
    int depth = 0;
    struct rect2i clip = canvas_rect;

    for (int i = 0; i < scene->n_ops && !g->failed && !g->atlas.full; i++) {
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
                scene_rect(g, op->rect, clip, op->color, 1.0f, canvas_rect);
            break;
        case UI_SCENE_OP_BLEND_RECT:
            if (!op_outside_clip(scene, op, clip))
                scene_rect(g, op->rect, clip, op->color | 0xFF000000u, op->alpha / 255.0f,
                           canvas_rect);
            break;
        case UI_SCENE_OP_ROUNDED_RECT: {
            if (op_outside_clip(scene, op, clip))
                break;
            if (op->radius <= 0) {
                scene_rect(g, op->rect, clip, op->color, 1.0f, canvas_rect);
                break;
            }
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size) || !(op->color >> 24))
                break;
            int radius = min_i(op->radius, min_i(op->rect.width / 2, op->rect.height / 2));
            emit_shape(g, visible, op->rect, color_of(op->color, 1.0f), (float)radius,
                       SHAPE_ROUNDED, 0);
            break;
        }
        case UI_SCENE_OP_CIRCLE: {
            if (op_outside_clip(scene, op, clip) || op->radius <= 0 || !op->alpha)
                break;
            struct rect2i visible = rect2i_intersect(op->rect, clip);
            if (!rect2i_clip(&visible, canvas_rect.size))
                break;
            emit_shape(g, visible, op->rect,
                       color_of(op->color | 0xFF000000u, op->alpha / 255.0f), (float)op->radius,
                       SHAPE_CIRCLE, 0);
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
            emit_shape(g, visible, op->rect, color_of(op->color, 1.0f), (float)radius,
                       SHAPE_SHADOW, (uint32_t)blur);
            break;
        }
        case UI_SCENE_OP_TEXT:
            if (!op_outside_clip(scene, op, clip))
                scene_text(g, ufont, scene, op, clip, canvas_rect);
            break;
        case UI_SCENE_OP_IMAGE:
            if (!op_outside_clip(scene, op, clip))
                scene_image(g, op, clip, canvas_rect);
            break;
        }
    }
    return !g->failed && !g->atlas.full;
}

/* ---- Frame assembly ----------------------------------------------------- */

static bool build_frame(struct render_gpu *g, const struct render_frame *frame,
                        struct vector2i canvas)
{
    begin_build(g);
    const struct rect2i canvas_rect = { .size = canvas };

    struct grid_geometry geo;
    if (grid_geometry_of(frame, canvas, &geo)) {
        if (!build_grid(g, &geo))
            return false;
        cmd_whole_stream(g, CMD_GLYPH, ST_BG, canvas_rect);
        cmd_whole_stream(g, CMD_GLYPH, ST_INK, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
        cmd_whole_stream(g, CMD_SUBPX, ST_SUBPX, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
        cmd_whole_stream(g, CMD_GLYPH, ST_DECO, (struct rect2i){
            .pos = geo.rect.pos, .size = { .x = geo.cols * geo.cw, .y = geo.rows * geo.ch } });
    }
    const struct ui_scene *scene = frame->ui;
    if (scene && ui_scene_is_valid(scene)) {
        struct font_instance *ufont =
            frame->ui_font ? frame->ui_font : font_instance_default();
        if (ufont && !build_scene(g, ufont, scene, canvas_rect))
            return false;
    }
    return !g->failed && !g->atlas.full;
}

static bool ensure_buffer(struct render_gpu *g, id<MTLBuffer> *buffer, size_t bytes)
{
    if (*buffer && (*buffer).length >= bytes)
        return true;
    size_t cap = *buffer ? (*buffer).length : BUFFER_MIN;
    while (cap < bytes)
        cap *= 2;
    id<MTLBuffer> next = [g->device newBufferWithLength:cap options:MTLResourceStorageModeShared];
    if (!next)
        return false;
    [*buffer release];
    *buffer = next;
    return true;
}

static size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

/* Takes a slot, or returns NULL when the GPU still reads every slot. The main
 * thread never waits for one: three slots mean that a free slot exists unless
 * the GPU is three frames behind, and a wait cannot catch up then. The window
 * drops the frame and asks again at its next display tick, so a busy GPU costs
 * no input or PTY time. */
static struct frame_slot *slot_acquire(struct render_gpu *g)
{
    for (int i = 0; i < FRAME_SLOTS; i++) {
        unsigned index = (g->next_slot + (unsigned)i) % FRAME_SLOTS;
        struct frame_slot *slot = &g->slots[index];
        if (dispatch_semaphore_wait(slot->ready, DISPATCH_TIME_NOW) != 0)
            continue;
        g->next_slot = (index + 1) % FRAME_SLOTS;
        return slot;
    }
    return NULL;
}

/*
 * Commits the command buffer and shows the drawable. In a live resize the
 * layer presents with the transaction of Core Animation. Then the drawable
 * must present after the command is scheduled, or the frame does not
 * appear with the new size.
 *
 * The completed handler frees the frame slot. It reports GPU completion, not
 * presentation: a display reports the presentation later, through the handler
 * that the window attaches in window_metal_draw().
 */
static void commit_and_present(id<MTLCommandBuffer> cb, id<CAMetalDrawable> drawable,
                               dispatch_semaphore_t ready, bool transaction)
{
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusError)
            fprintf(stderr, "[metal] frame failed: %s\n",
                    done.error.localizedDescription.UTF8String);
        dispatch_semaphore_signal(ready);
    }];
    if (transaction) {
        [cb commit];
        [cb waitUntilScheduled];
        [drawable present];
    } else {
        [cb presentDrawable:drawable];
        [cb commit];
    }
}

/* Copies the staged glyph pixels into the upload buffer of the slot. The blit
 * that reads them comes later, in encode_uploads(). */
static bool stage_uploads(struct render_gpu *g, struct frame_slot *slot)
{
    struct atlas *a = &g->atlas;
    if (!a->n_uploads)
        return true;
    if (!ensure_buffer(g, &slot->upload, a->stage_used))
        return false;
    memcpy(slot->upload.contents, a->stage, a->stage_used);
    return true;
}

/* Encodes the staged glyph uploads into the atlas. */
static bool encode_uploads(struct render_gpu *g, struct frame_slot *slot, id<MTLCommandBuffer> cb)
{
    struct atlas *a = &g->atlas;
    if (!a->n_uploads)
        return true;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!blit)
        return false;
    for (uint32_t i = 0; i < a->n_uploads; i++) {
        const struct upload *u = &a->uploads[i];
        [blit copyFromBuffer:slot->upload
                sourceOffset:u->offset
           sourceBytesPerRow:(NSUInteger)u->w * 4
         sourceBytesPerImage:(NSUInteger)u->w * (NSUInteger)u->h * 4
                  sourceSize:MTLSizeMake(u->w, u->h, 1)
                   toTexture:a->texture
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(u->x, u->y, 0)];
    }
    [blit endEncoding];
    return true;
}

static MTLClearColor clear_color_of(uint32_t argb)
{
    simd_float4 c = color_of(argb, 1.0f);
    return MTLClearColorMake(c.x, c.y, c.z, c.w);
}

static void set_scissor(id<MTLRenderCommandEncoder> enc, struct rect2i r)
{
    [enc setScissorRect:(MTLScissorRect){
        (NSUInteger)r.x, (NSUInteger)r.y, (NSUInteger)r.width, (NSUInteger)r.height }];
}

/* The drawable holds the pixels of `size`. A resize between the build of the
 * frame and the drawable gives a mismatch, and the frame is dropped. */
static bool drawable_size_ok(const struct framebuffer *fb, id<CAMetalDrawable> drawable)
{
    id<MTLTexture> texture = drawable.texture;
    return texture && texture.width == (NSUInteger)fb->size.x &&
           texture.height == (NSUInteger)fb->size.y;
}

/* The drawable of the frame. It takes the drawable from the layer when the
 * frame data is ready, so the drawable pool is held for the encode alone. */
static id<CAMetalDrawable> frame_drawable(struct framebuffer *fb)
{
    if (!fb->drawable && fb->acquire)
        fb->drawable = fb->acquire(fb->acquire_ud);
    return (id<CAMetalDrawable>)fb->drawable;
}

static bool encode_frame(struct render_gpu *g, struct frame_slot *slot, struct framebuffer *fb,
                         uint32_t base)
{
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)fb->drawable;
    size_t offsets[ST_COUNT];
    size_t total = 0;
    for (int i = 0; i < ST_COUNT; i++) {
        offsets[i] = total;
        total += align_up((size_t)g->streams[i].n * STREAM_STRIDE[i], STREAM_ALIGN);
    }

    id<MTLCommandBuffer> cb = [g->queue commandBuffer];
    if (!cb || !encode_uploads(g, slot, cb))
        return false;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clear_color_of(base);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    if (!enc)
        return false;
    const struct uniforms uniforms = {
        .viewport_size = simd_make_float2((float)fb->size.x, (float)fb->size.y),
    };
    [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
    [enc setFragmentTexture:g->atlas.texture atIndex:0];

    const struct rect2i canvas_rect = { .size = fb->size };
    id<MTLRenderPipelineState> bound = nil;
    for (uint32_t i = 0; i < g->n_cmds; i++) {
        const struct cmd *c = &g->cmds[i];
        struct rect2i scissor = c->scissor;
        if (!c->count || !rect2i_clip(&scissor, canvas_rect.size))
            continue;
        set_scissor(enc, scissor);
        [enc setFragmentTexture:c->image ? c->image : g->atlas.texture atIndex:1];
        [enc setVertexBuffer:slot->instances
                      offset:offsets[c->stream] + (size_t)c->start * STREAM_STRIDE[c->stream]
                     atIndex:0];
        int passes = c->kind == CMD_SUBPX ? 3 : 1;
        for (int ch = 0; ch < passes; ch++) {
            id<MTLRenderPipelineState> pipeline = c->kind == CMD_SHAPE ? g->shape_pipeline
                : c->kind == CMD_SUBPX ? g->subpx_pipeline[ch] : g->glyph_pipeline;
            if (pipeline != bound) {
                [enc setRenderPipelineState:pipeline];
                bound = pipeline;
            }
            if (c->kind != CMD_SHAPE) {
                uint32_t channel = (uint32_t)ch;
                [enc setFragmentBytes:&channel length:sizeof(channel) atIndex:0];
            }
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:6
                  instanceCount:c->count];
        }
    }
    [enc endEncoding];
    commit_and_present(cb, drawable, slot->ready, fb->transaction);
    g->atlas.stage_used = 0;
    g->atlas.n_uploads = 0;
    return true;
}

/* Builds the instance lists and the glyph uploads of one frame into a slot.
 * No drawable is held while this runs. */
static bool prepare_frame(struct render_gpu *g, struct frame_slot *slot, struct framebuffer *fb,
                          const struct render_frame *frame)
{
    bool built = false;
    for (int attempt = 0; attempt < 2 && !built; attempt++) {
        if (attempt && !atlas_recycle(g))
            break;
        built = build_frame(g, frame, fb->size);
        if (!built && !g->atlas.full)
            break;
    }
    glyph_cache_flush_frame();
    if (!built) {
        /* The map can hold glyphs that were staged and never uploaded. */
        atlas_reset(g);
        return false;
    }
    size_t offsets[ST_COUNT];
    size_t total = 0;
    for (int i = 0; i < ST_COUNT; i++) {
        offsets[i] = total;
        total += align_up((size_t)g->streams[i].n * STREAM_STRIDE[i], STREAM_ALIGN);
    }
    if (!ensure_buffer(g, &slot->instances, total ? total : STREAM_ALIGN) ||
        !stage_uploads(g, slot)) {
        atlas_reset(g);
        return false;
    }
    for (int i = 0; i < ST_COUNT; i++)
        if (g->streams[i].n)
            memcpy((uint8_t *)slot->instances.contents + offsets[i], g->streams[i].data,
                   (size_t)g->streams[i].n * STREAM_STRIDE[i]);
    return true;
}

enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame)
{
    (void)s;
    if (!g || !fb || !frame || (!fb->drawable && !fb->acquire) || fb->size.x <= 0 ||
        fb->size.y <= 0)
        return RENDER_GPU_UNUSED;
    struct frame_slot *slot = slot_acquire(g);
    if (!slot)
        return RENDER_GPU_DROPPED;
    enum render_gpu_status status = RENDER_GPU_UNUSED;
    @autoreleasepool {
        if (prepare_frame(g, slot, fb, frame)) {
            id<CAMetalDrawable> drawable = frame_drawable(fb);
            if (!drawable || !drawable_size_ok(fb, drawable)) {
                /* The drawable is gone, or a resize changed its size after the
                 * frame data was built. Nothing is drawn or presented. */
                atlas_reset(g);
                status = RENDER_GPU_DROPPED;
            } else if (encode_frame(g, slot, fb, frame->base)) {
                status = RENDER_GPU_PRESENTED;
            } else {
                /* The map can hold glyphs that were staged and never uploaded. */
                atlas_reset(g);
            }
        }
    }
    if (status != RENDER_GPU_PRESENTED)
        dispatch_semaphore_signal(slot->ready);
    return status;
}

/* ---- Presenting CPU pixels ---------------------------------------------- */

static struct gpu_surface *gpu_surface_of(struct render_surface *s)
{
    if (!s->gpu)
        s->gpu = calloc(1, sizeof(struct gpu_surface));
    return s->gpu;
}

/* Makes the texture of the CPU pixels. A new size gives a new texture. */
static bool surface_texture(struct render_gpu *g, struct gpu_surface *gs, struct vector2i size)
{
    if (gs->pixels && vector2i_eq(gs->size, size))
        return true;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)size.x
                                                          height:(NSUInteger)size.y
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g->device newTextureWithDescriptor:d];
    if (!t)
        return false;
    [gs->pixels release];
    gs->pixels = t;
    gs->size = size;
    return true;
}

static bool present_pixels(struct render_gpu *g, struct frame_slot *slot, struct render_surface *s,
                           struct framebuffer *fb, const struct damage_list *damage)
{
    struct gpu_surface *gs = gpu_surface_of(s);
    if (!gs)
        return false;
    bool fresh = !gs->pixels || !vector2i_eq(gs->size, fb->size);
    if (!surface_texture(g, gs, fb->size))
        return false;

    /* The rects to upload. A new texture takes every pixel. */
    struct rect2i whole = { .size = fb->size };
    struct rect2i rects[DAMAGE_RECTS_MAX];
    int n = 0;
    if (fresh || !damage || damage->all) {
        rects[n++] = whole;
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
    size_t bytes = 0;
    for (int i = 0; i < n; i++)
        bytes += (size_t)rects[i].width * (size_t)rects[i].height * 4;

    /* The quad of the copy shares the instance buffer of the slot. */
    if (!ensure_buffer(g, &slot->instances, STREAM_ALIGN) ||
        (bytes && !ensure_buffer(g, &slot->upload, bytes)))
        return false;
    struct glyph_inst quad = {
        .rect = rect2i_to_float4(whole),
        .uv = rect_of(0, 0, (float)fb->size.x, (float)fb->size.y),
        .color = simd_make_float4(1, 1, 1, 1),
        .mode = MODE_IMAGE,
    };
    memcpy(slot->instances.contents, &quad, sizeof(quad));

    id<MTLCommandBuffer> cb = [g->queue commandBuffer];
    if (!cb)
        return false;
    if (bytes) {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        if (!blit)
            return false;
        size_t offset = 0;
        for (int i = 0; i < n; i++) {
            const struct rect2i *r = &rects[i];
            size_t row_bytes = (size_t)r->width * 4;
            for (int y = 0; y < r->height; y++)
                memcpy((uint8_t *)slot->upload.contents + offset + (size_t)y * row_bytes,
                       fb->pixels + (size_t)(r->y + y) * (size_t)fb->size.x + (size_t)r->x,
                       row_bytes);
            [blit copyFromBuffer:slot->upload
                    sourceOffset:offset
               sourceBytesPerRow:row_bytes
             sourceBytesPerImage:row_bytes * (size_t)r->height
                      sourceSize:MTLSizeMake((NSUInteger)r->width, (NSUInteger)r->height, 1)
                       toTexture:gs->pixels
                destinationSlice:0
                destinationLevel:0
               destinationOrigin:MTLOriginMake((NSUInteger)r->x, (NSUInteger)r->y, 0)];
            offset += row_bytes * (size_t)r->height;
        }
        [blit endEncoding];
    }

    id<CAMetalDrawable> drawable = frame_drawable(fb);
    if (!drawable || !drawable_size_ok(fb, drawable))
        return false;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    if (!enc)
        return false;
    const struct uniforms uniforms = {
        .viewport_size = simd_make_float2((float)fb->size.x, (float)fb->size.y),
    };
    [enc setRenderPipelineState:g->copy_pipeline];
    [enc setVertexBuffer:slot->instances offset:0 atIndex:0];
    [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
    [enc setFragmentTexture:gs->pixels atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1];
    [enc endEncoding];
    commit_and_present(cb, drawable, slot->ready, fb->transaction);
    return true;
}

bool render_gpu_present(struct render_gpu *g, struct render_surface *s, struct framebuffer *fb,
                        const struct damage_list *damage)
{
    if (!g || !s || !fb || (!fb->drawable && !fb->acquire) || !fb->pixels ||
        fb->size.x <= 0 || fb->size.y <= 0)
        return false;
    struct frame_slot *slot = slot_acquire(g);
    if (!slot)
        return false;
    bool presented = false;
    @autoreleasepool {
        presented = present_pixels(g, slot, s, fb, damage);
    }
    if (!presented)
        dispatch_semaphore_signal(slot->ready);
    return presented;
}

void render_gpu_surface_free(void *gpu_surface)
{
    struct gpu_surface *gs = gpu_surface;
    if (!gs)
        return;
    [gs->pixels release];
    free(gs);
}

/* ---- Creation and teardown ---------------------------------------------- */

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device, id<MTLLibrary> lib,
                                                NSString *vertex_name, NSString *fragment_name,
                                                bool blend, MTLColorWriteMask mask)
{
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    MTLRenderPipelineColorAttachmentDescriptor *color = pd.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatBGRA8Unorm;
    color.writeMask = mask;
    color.blendingEnabled = blend;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLFunction> vertex = [lib newFunctionWithName:vertex_name];
    id<MTLFunction> fragment = [lib newFunctionWithName:fragment_name];
    pd.vertexFunction = vertex;
    pd.fragmentFunction = fragment;
    NSError *err = nil;
    id<MTLRenderPipelineState> pipeline = vertex && fragment
        ? [device newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
    if (!pipeline)
        fprintf(stderr, "[metal] pipeline %s failed: %s\n", vertex_name.UTF8String,
                err ? err.localizedDescription.UTF8String : "missing function");
    [vertex release];
    [fragment release];
    [pd release];
    return pipeline;
}

static bool pipelines_create(struct render_gpu *g)
{
    NSError *err = nil;
    id<MTLLibrary> lib = [g->device newLibraryWithSource:kMetalShaderSource options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "[metal] shader compilation failed: %s\n",
                err.localizedDescription.UTF8String);
        return false;
    }
    const MTLColorWriteMask all = MTLColorWriteMaskAll;
    g->glyph_pipeline = make_pipeline(g->device, lib, @"glyph_vertex", @"glyph_fragment", true, all);
    g->shape_pipeline = make_pipeline(g->device, lib, @"shape_vertex", @"shape_fragment", true, all);
    g->copy_pipeline = make_pipeline(g->device, lib, @"glyph_vertex", @"copy_fragment", false, all);
    const MTLColorWriteMask masks[3] = {
        MTLColorWriteMaskRed, MTLColorWriteMaskGreen, MTLColorWriteMaskBlue,
    };
    bool ok = g->glyph_pipeline && g->shape_pipeline && g->copy_pipeline;
    for (int i = 0; i < 3; i++) {
        g->subpx_pipeline[i] =
            make_pipeline(g->device, lib, @"glyph_vertex", @"glyph_fragment", true, masks[i]);
        ok = ok && g->subpx_pipeline[i];
    }
    [lib release];
    return ok;
}

static bool gpu_disabled(void)
{
    const char *env = getenv("WSAP_RENDER_GPU");
    return env && (!strcmp(env, "0") || !strcmp(env, "off"));
}

struct render_gpu *render_gpu_new(void)
{
    if (gpu_disabled())
        return NULL;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        return NULL;
    struct render_gpu *g = calloc(1, sizeof(*g));
    if (!g) {
        [device release];
        return NULL;
    }
    g->device = device;
    g->queue = [device newCommandQueue];
    g->decor = decor_cache_create();
    g->atlas.map = calloc(ATLAS_MAP_SIZE, sizeof(*g->atlas.map));
    for (int i = 0; i < FRAME_SLOTS; i++)
        g->slots[i].ready = dispatch_semaphore_create(1);
    if (!g->queue || !g->decor || !g->atlas.map || !pipelines_create(g) ||
        !atlas_make_texture(g, ATLAS_START_SIZE)) {
        render_gpu_free(g);
        return NULL;
    }
    return g;
}

void render_gpu_free(struct render_gpu *g)
{
    if (!g)
        return;
    for (int i = 0; i < FRAME_SLOTS; i++) {
        struct frame_slot *slot = &g->slots[i];
        if (slot->ready) {
            /* A semaphore must have its first value when it is released, so a
             * command buffer that still reads the slot must finish first. Here
             * the wait is bounded by the GPU, and freeing a live buffer is
             * worse than a slow exit. */
            dispatch_semaphore_wait(slot->ready, DISPATCH_TIME_FOREVER);
            dispatch_semaphore_signal(slot->ready);
            dispatch_release(slot->ready);
        }
        [slot->instances release];
        [slot->upload release];
    }
    for (uint32_t i = 0; i < g->n_cmds; i++)
        [g->cmds[i].image release];
    for (int i = 0; i < IMAGE_CACHE; i++)
        [g->images[i].texture release];
    [g->atlas.texture release];
    [g->glyph_pipeline release];
    for (int i = 0; i < 3; i++)
        [g->subpx_pipeline[i] release];
    [g->shape_pipeline release];
    [g->copy_pipeline release];
    [g->queue release];
    [g->device release];
    for (int i = 0; i < ST_COUNT; i++)
        free(g->streams[i].data);
    free(g->cmds);
    free(g->clips);
    free(g->atlas.map);
    free(g->atlas.stage);
    free(g->atlas.uploads);
    decor_cache_destroy(g->decor);
    free(g);
}

void *render_gpu_native_device(struct render_gpu *g)
{
    return g ? (void *)g->device : NULL;
}
