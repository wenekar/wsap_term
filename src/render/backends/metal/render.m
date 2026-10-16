#import "render/backends/metal/render.h"

#include "util/hash.h"
#include "render/term_frame.h"
#include "render/backend.h"
#include "render/cell.h"
#include "paint/blend.h"
#include "font/glyph_cache.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "paint/scene.h"
#include "paint/shadow.h"
#include "paint/text_run.h"
#include "render/text_style.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>
#import <dispatch/dispatch.h>

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

#define GPU_GLYPH_SLOTS 8192
#define GPU_ATLAS_MAX_SIZE 4096
#define GPU_ATLAS_START_MAX 1024
#define GPU_ATLAS_MIN_SIZE 512
#define GPU_PROBE_MAX 8
#define GPU_SCAN_RETRY_MAX 4
#define GPU_IMAGE_CACHE 4
#define GPU_UPLOAD_ALIGN 16

#define ASCII_FIRST '!'
#define ASCII_GLYPHS ('~' - '!' + 1)
#define ASCII_STYLE_MASK FONT_STYLE_VARIANT_MASK
#define ASCII_CELLS (ASCII_GLYPHS * (ASCII_STYLE_MASK + 1))
#define ASCII_TABLE (128 * (ASCII_STYLE_MASK + 1))

_Static_assert(GPU_GLYPH_SLOTS <= UINT16_MAX,
               "atlas cells store their owner slot as uint16");
_Static_assert(ASCII_STYLE_MASK == 3,
               "the ASCII atlas indexes its styles by the low style bits");

enum glyph_mode {
    GLYPH_SOLID = 0,
    GLYPH_MONO = 1,
    GLYPH_COLOR = 2,
    GLYPH_IMAGE = 3,
    GLYPH_ASCII = 4,
    GLYPH_SUBPIXEL = 5,
};

struct atlas_glyph {
    uint16_t cell;
    uint16_t width;
    int16_t x_offset;
    uint8_t mode;
};

struct gpu_glyph_slot {
    uint64_t key;
    uint32_t generation;
    uint16_t style;
    struct atlas_glyph glyph;
};

_Static_assert(sizeof(struct gpu_glyph_slot) == 24,
               "gpu_glyph_slot must stay 24 bytes");

struct atlas_uploads {
    uint16_t *cells;
    uint8_t *pixels;
    uint32_t count, cap;
};

struct ascii_atlas {
    id<MTLTexture> texture;
    struct atlas_glyph glyphs[ASCII_TABLE];
    struct atlas_uploads uploads;
    int cols;
};

struct glyph_atlas {
    id<MTLTexture> texture;
    struct gpu_glyph_slot slots[GPU_GLYPH_SLOTS];
    uint32_t *cell_used;
    uint16_t *cell_owner;
    struct atlas_uploads uploads;
    struct ascii_atlas ascii;
    uint32_t reset_count;
    uint32_t frame;
    uint64_t font_version;
    struct vector2i cell_size;
    int size, cols, capacity, alloc, hand;
    bool overflow;
};

typedef const struct glyph *(*glyph_fetch_fn)(struct font_instance *fi, uint32_t cp,
                                            uint16_t flags);

struct gpu_draw {
    struct atlas_glyph glyph;
    uint16_t tex_x, tex_y;
    uint16_t flags;
    uint16_t col;
    uint32_t row;
    uint32_t fg;
};

_Static_assert(WSAP_VTCORE_MAX_COLS <= UINT16_MAX,
               "gpu_draw stores the column as uint16");
_Static_assert(GPU_ATLAS_MAX_SIZE <= UINT16_MAX,
               "gpu_draw stores texel origins as uint16");

struct gpu_bg_span {
    int col;
    int width;
    uint32_t bg;
};

struct metal_glyph_vertex {
    simd_float4 rect;
    simd_float4 uv;
    simd_float4 color;
    uint32_t mode;
};

struct metal_shape_vertex {
    simd_float4 rect;
    simd_float4 color;
    float radius;
    uint32_t mode;
    uint32_t alpha;
};

struct metal_uniforms {
    simd_float2 viewport_size;
};

enum metal_cmd_type {
    CMD_GLYPHS = 0,
    CMD_SHAPES = 1,
};

struct metal_cmd {
    enum metal_cmd_type type;
    struct rect2i scissor;
    uint32_t start;
    uint32_t count;
    id<MTLTexture> texture;
    id<MTLTexture> ascii;
};

#define METAL_FRAME_COUNT 3

struct metal_frame_resources {
    id<MTLBuffer> glyphs;
    id<MTLBuffer> shapes;
    id<MTLBuffer> upload;
    dispatch_semaphore_t ready;
};

struct image_entry {
    id<MTLTexture> texture;
    const struct ui_image *src;
    uint64_t gen;
    struct vector2i size;
    uint32_t used;
};

struct metal_output {
    struct gpu_draw *draws;
    struct gpu_bg_span *bg_spans;
    size_t draw_capacity;
    uint64_t *row_gen;
    int *row_draw_count;
    int *row_bg_count;
    int *row_sel_c0;
    int *row_sel_c1;
    size_t rows_capacity;
    int cached_cols, cached_rows;
    int prev_origin_y;
    int prev_cursor_x, prev_cursor_y;
    int prev_cursor_style;
    uint32_t prev_default_bg, prev_default_fg, prev_cursor_color;
    uint64_t prev_full_gen;
    bool prev_reverse_video;
    uint32_t atlas_frame;
};

struct metal_render {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> glyph_pipeline;
    id<MTLRenderPipelineState> shape_pipeline;
    id<MTLSamplerState> sampler_state;
    id<MTLTexture> solid_tex;

    struct metal_frame_resources frames[METAL_FRAME_COUNT];
    unsigned next_frame;
    uint32_t frame;
    bool failed;

    struct glyph_atlas term;
    struct glyph_atlas *ui;
    struct font_instance *ui_font;

    struct metal_glyph_vertex *glyph_verts;
    uint32_t glyph_count;
    uint32_t glyph_cap;

    struct metal_shape_vertex *shape_verts;
    uint32_t shape_count;
    uint32_t shape_cap;

    struct metal_cmd *cmds;
    uint32_t cmd_count;
    uint32_t cmd_cap;

    bool clear_pending;
    uint32_t clear_color;

    struct metal_output *output;

    uint32_t run_cps[WSAP_VTCORE_MAX_COLS];

    struct image_entry images[GPU_IMAGE_CACHE];

    struct rect2i *clip_stack;
    int clip_stack_cap;

    id<MTLTexture> pixels_tex;
    const uint32_t *pixels_src;
    struct vector2i pixels_size;
};

#include "metal_shader_source.h"

static inline simd_float4 make_color(uint32_t c, float alpha_mult)
{
    float a = (((c >> 24) & 0xff) / 255.0f) * alpha_mult;
    float r = ((c >> 16) & 0xff) / 255.0f;
    float g = ((c >> 8) & 0xff) / 255.0f;
    float b = (c & 0xff) / 255.0f;
    return simd_make_float4(r, g, b, a);
}

static bool ensure_glyph_verts(struct metal_render *r, uint32_t extra)
{
    if (r->glyph_count + extra <= r->glyph_cap)
        return true;
    uint32_t cap = r->glyph_cap ? r->glyph_cap * 2 : 256;
    while (cap < r->glyph_count + extra) cap *= 2;
    struct metal_glyph_vertex *v = realloc(r->glyph_verts, cap * sizeof(*v));
    if (!v) { r->failed = true; return false; }
    r->glyph_verts = v;
    r->glyph_cap = cap;
    return true;
}

static bool ensure_shape_verts(struct metal_render *r, uint32_t extra)
{
    if (r->shape_count + extra <= r->shape_cap)
        return true;
    uint32_t cap = r->shape_cap ? r->shape_cap * 2 : 256;
    while (cap < r->shape_count + extra) cap *= 2;
    struct metal_shape_vertex *v = realloc(r->shape_verts, cap * sizeof(*v));
    if (!v) { r->failed = true; return false; }
    r->shape_verts = v;
    r->shape_cap = cap;
    return true;
}

static struct metal_cmd *add_cmd(struct metal_render *r, enum metal_cmd_type type,
                                 struct rect2i scissor, id<MTLTexture> texture,
                                 id<MTLTexture> ascii)
{
    if (r->cmd_count > 0) {
        struct metal_cmd *prev = &r->cmds[r->cmd_count - 1];
        if (prev->type == type && rect2i_eq(prev->scissor, scissor) &&
            (!texture || !prev->texture || prev->texture == texture) &&
            (!ascii || !prev->ascii || prev->ascii == ascii)) {
            if (texture && !prev->texture) prev->texture = [texture retain];
            if (ascii && !prev->ascii) prev->ascii = [ascii retain];
            return prev;
        }
    }
    if (r->cmd_count >= r->cmd_cap) {
        uint32_t cap = r->cmd_cap ? r->cmd_cap * 2 : 32;
        struct metal_cmd *c = realloc(r->cmds, cap * sizeof(*c));
        if (!c) { r->failed = true; return NULL; }
        r->cmds = c;
        r->cmd_cap = cap;
    }
    struct metal_cmd *cmd = &r->cmds[r->cmd_count++];
    *cmd = (struct metal_cmd){
        .type = type,
        .scissor = scissor,
        .start = (type == CMD_GLYPHS) ? r->glyph_count : r->shape_count,
        .count = 0,
        .texture = [texture retain],
        .ascii = [ascii retain],
    };
    return cmd;
}

static inline void emit_glyph_quad(struct metal_render *r,
                                   float x0, float y0, float x1, float y1,
                                   float u0, float v0, float u1, float v1,
                                   simd_float4 color, enum glyph_mode mode)
{
    r->glyph_verts[r->glyph_count++] = (struct metal_glyph_vertex){
        .rect = simd_make_float4(x0, y0, x1 - x0, y1 - y0),
        .uv = simd_make_float4(u0, v0, u1, v1),
        .color = color,
        .mode = mode,
    };
}

static void batch_glyph_quad(struct metal_render *r, struct rect2i scissor,
                             float x0, float y0, float x1, float y1,
                             float u0, float v0, float u1, float v1,
                             simd_float4 color, enum glyph_mode mode, id<MTLTexture> tex)
{
    bool ascii = mode == GLYPH_ASCII;
    struct metal_cmd *cmd = add_cmd(r, CMD_GLYPHS, scissor, ascii ? nil : tex,
                                    ascii ? tex : nil);
    if (!cmd || !ensure_glyph_verts(r, 1)) return;
    emit_glyph_quad(r, x0, y0, x1, y1, u0, v0, u1, v1, color, mode);
    cmd->count++;
}

static void batch_shape_quad(struct metal_render *r, struct rect2i scissor,
                             struct rect2i rect, simd_float4 color,
                             float radius, uint32_t mode, uint32_t alpha)
{
    struct metal_cmd *cmd = add_cmd(r, CMD_SHAPES, scissor, nil, nil);
    if (!cmd || !ensure_shape_verts(r, 1)) return;
    r->shape_verts[r->shape_count++] = (struct metal_shape_vertex){
        .rect = simd_make_float4(rect.x, rect.y, rect.width, rect.height),
        .color = color,
        .radius = radius,
        .mode = mode,
        .alpha = alpha,
    };
    cmd->count++;
}

static int pick_atlas_size(struct vector2i cell, size_t cell_count)
{
    if (cell.x <= 0 || cell.y <= 0)
        return GPU_ATLAS_MIN_SIZE;
    size_t target = cell_count < GPU_ATLAS_START_MAX
        ? GPU_ATLAS_START_MAX : cell_count;
    if (target > GPU_GLYPH_SLOTS)
        target = GPU_GLYPH_SLOTS;
    int size = GPU_ATLAS_MIN_SIZE;
    while (size < GPU_ATLAS_MAX_SIZE &&
           (size_t)(size / cell.x) * (size_t)(size / cell.y) < target)
        size *= 2;
    return size;
}

static inline size_t upload_align(size_t bytes)
{
    return (bytes + GPU_UPLOAD_ALIGN - 1) & ~(size_t)(GPU_UPLOAD_ALIGN - 1);
}

static void *uploads_add(struct atlas_uploads *u, int cell, size_t cell_bytes)
{
    if (u->count == u->cap) {
        uint32_t cap = u->cap ? u->cap * 2 : 64;
        uint16_t *cells = realloc(u->cells, cap * sizeof(*cells));
        if (!cells) return NULL;
        u->cells = cells;
        uint8_t *pixels = realloc(u->pixels, cap * cell_bytes);
        if (!pixels) return NULL;
        u->pixels = pixels;
        u->cap = cap;
    }
    u->cells[u->count] = (uint16_t)cell;
    return u->pixels + (size_t)u->count++ * cell_bytes;
}

static void uploads_free(struct atlas_uploads *u)
{
    free(u->cells);
    free(u->pixels);
    *u = (struct atlas_uploads){0};
}

static void ascii_reset(struct ascii_atlas *s)
{
    memset(s->glyphs, 0, sizeof(s->glyphs));
    s->uploads.count = 0;
}

static void ascii_release(struct ascii_atlas *s)
{
    [s->texture release];
    uploads_free(&s->uploads);
    *s = (struct ascii_atlas){0};
}

static void ascii_layout(struct ascii_atlas *s, id<MTLDevice> device, struct vector2i cell)
{
    ascii_release(s);
    int cols = GPU_ATLAS_MAX_SIZE / cell.x;
    if (cols > ASCII_GLYPHS)
        cols = ASCII_GLYPHS;
    if (cols <= 0)
        return;
    int height = (ASCII_CELLS + cols - 1) / cols * cell.y;
    if (height > GPU_ATLAS_MAX_SIZE)
        return;
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:(NSUInteger)(cols * cell.x)
                                                          height:(NSUInteger)height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    s->texture = [device newTextureWithDescriptor:desc];
    s->cols = cols;
}

static void atlas_reset(struct glyph_atlas *a)
{
    if (++a->reset_count == 0) {
        memset(a->slots, 0, sizeof(a->slots));
        a->reset_count = 1;
    }
    a->alloc = 0;
    a->hand = 0;
    a->uploads.count = 0;
    a->overflow = false;
    if (a->capacity > 0) {
        memset(a->cell_used, 0, (size_t)a->capacity * sizeof(*a->cell_used));
        memset(a->cell_owner, 0, (size_t)a->capacity * sizeof(*a->cell_owner));
    }
    ascii_reset(&a->ascii);
}

static void atlas_release(struct glyph_atlas *a)
{
    [a->texture release];
    free(a->cell_used);
    free(a->cell_owner);
    uploads_free(&a->uploads);
    ascii_release(&a->ascii);
    a->texture = nil;
    a->cell_used = NULL;
    a->cell_owner = NULL;
    a->capacity = 0;
}

static bool atlas_layout(struct glyph_atlas *a, id<MTLDevice> device,
                         struct vector2i cell, int size)
{
    if (a->texture && vector2i_eq(a->cell_size, cell) && a->size == size)
        return true;
    int cols = size / cell.x;
    int capacity = cols * (size / cell.y);
    if (capacity > GPU_GLYPH_SLOTS)
        capacity = GPU_GLYPH_SLOTS;

    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)size
                                                          height:(NSUInteger)size
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    uint32_t *used = calloc((size_t)capacity, sizeof(*used));
    uint16_t *owner = calloc((size_t)capacity, sizeof(*owner));
    if (!tex || !used || !owner) {
        [tex release];
        free(used);
        free(owner);
        return false;
    }
    if (!vector2i_eq(a->cell_size, cell))
        ascii_layout(&a->ascii, device, cell);
    [a->texture release];
    free(a->cell_used);
    free(a->cell_owner);
    uploads_free(&a->uploads);
    a->texture = tex;
    a->cell_used = used;
    a->cell_owner = owner;
    a->cell_size = cell;
    a->size = size;
    a->cols = cols;
    a->capacity = capacity;
    atlas_reset(a);
    return true;
}

static inline struct rect2i grid_rect(int cols, struct vector2i cell_size, int cell, int width)
{
    return (struct rect2i){
        .pos = { .x = (cell % cols) * cell_size.x, .y = (cell / cols) * cell_size.y },
        .size = { .x = width, .y = cell_size.y },
    };
}

static inline struct rect2i atlas_rect(const struct glyph_atlas *a, const struct atlas_glyph *g)
{
    int cols = g->mode == GLYPH_ASCII ? a->ascii.cols : a->cols;
    return grid_rect(cols, a->cell_size, g->cell, g->width);
}

static inline void atlas_mark(struct glyph_atlas *a, int cell)
{
    a->cell_used[cell] = a->frame;
}

__attribute__((cold, noinline))
static struct gpu_glyph_slot *atlas_victim(struct glyph_atlas *a, unsigned base)
{
    struct gpu_glyph_slot *victim = &a->slots[base & (GPU_GLYPH_SLOTS - 1)];
    for (unsigned i = 1; i < GPU_PROBE_MAX; i++) {
        struct gpu_glyph_slot *slot = &a->slots[(base + i) & (GPU_GLYPH_SLOTS - 1)];
        if (a->cell_used[slot->glyph.cell] < a->cell_used[victim->glyph.cell])
            victim = slot;
    }
    return victim;
}

static inline struct gpu_glyph_slot *atlas_lookup(struct glyph_atlas *a, uint64_t key,
                                                  uint16_t style,
                                                  struct gpu_glyph_slot **fill)
{
    unsigned base = (unsigned)hash64_finish(key ^ ((uint64_t)style << 56));
    uint32_t generation = a->reset_count;
    for (unsigned i = 0; i < GPU_PROBE_MAX; i++) {
        struct gpu_glyph_slot *slot = &a->slots[(base + i) & (GPU_GLYPH_SLOTS - 1)];
        if (slot->generation != generation) {
            *fill = slot;
            return NULL;
        }
        if (slot->key == key && slot->style == style)
            return slot;
    }
    *fill = atlas_victim(a, base);
    return NULL;
}

static int atlas_take_cell(struct glyph_atlas *a)
{
    if (a->alloc < a->capacity)
        return a->alloc++;
    for (int i = 0; i < a->capacity; i++) {
        int cell = a->hand;
        a->hand = (a->hand + 1) % a->capacity;
        if (a->cell_used[cell] + 1 >= a->frame)
            continue;
        uint16_t owner = a->cell_owner[cell];
        if (owner) {
            struct gpu_glyph_slot *slot = &a->slots[owner - 1];
            if (slot->generation == a->reset_count && slot->glyph.cell == cell)
                slot->generation = 0;
            a->cell_owner[cell] = 0;
        }
        return cell;
    }
    return -1;
}

static inline uint32_t straight_color(uint32_t c)
{
    uint32_t a = (c >> 24) & 0xff;
    if (a == 0 || a == 255)
        return c;
    uint32_t r = (((c >> 16) & 0xff) * 255 + a / 2) / a;
    uint32_t g = (((c >> 8) & 0xff) * 255 + a / 2) / a;
    uint32_t b = ((c & 0xff) * 255 + a / 2) / a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void pack_alpha(uint32_t *dst, const uint8_t *alpha, int width)
{
    int x = 0;
#if defined(__aarch64__)
    uint32x4_t white = vdupq_n_u32(0x00ffffffu);
    for (; x + 16 <= width; x += 16) {
        uint8x16_t a = vld1q_u8(alpha + x);
        uint16x8_t lo = vmovl_u8(vget_low_u8(a));
        uint16x8_t hi = vmovl_u8(vget_high_u8(a));
        vst1q_u32(dst + x, vorrq_u32(white, vshlq_n_u32(vmovl_u16(vget_low_u16(lo)), 24)));
        vst1q_u32(dst + x + 4, vorrq_u32(white, vshlq_n_u32(vmovl_u16(vget_high_u16(lo)), 24)));
        vst1q_u32(dst + x + 8, vorrq_u32(white, vshlq_n_u32(vmovl_u16(vget_low_u16(hi)), 24)));
        vst1q_u32(dst + x + 12, vorrq_u32(white, vshlq_n_u32(vmovl_u16(vget_high_u16(hi)), 24)));
    }
#elif defined(__AVX512F__) && defined(__AVX512BW__)
    for (; x + 16 <= width; x += 16) {
        __m512i a = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *)(alpha + x)));
        _mm512_storeu_si512(dst + x, _mm512_or_si512(_mm512_slli_epi32(a, 24),
                                                   _mm512_set1_epi32(0x00ffffff)));
    }
#elif defined(__AVX2__)
    for (; x + 8 <= width; x += 8) {
        __m256i a = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(alpha + x)));
        _mm256_storeu_si256((__m256i *)(dst + x), _mm256_or_si256(_mm256_slli_epi32(a, 24),
                                                               _mm256_set1_epi32(0x00ffffff)));
    }
#endif
    for (; x < width; x++)
        dst[x] = ((uint32_t)alpha[x] << 24) | 0x00ffffffu;
}

__attribute__((cold, noinline))
static const struct atlas_glyph *atlas_insert(struct glyph_atlas *a, struct gpu_glyph_slot *slot,
                                              uint64_t key, uint16_t style,
                                              const struct glyph *glyph, int rows)
{
    if (!glyph || !glyph->width || a->capacity <= 0)
        return NULL;
    if (slot->generation == a->reset_count) {
        if (a->cell_owner[slot->glyph.cell] == (uint16_t)(slot - a->slots) + 1)
            a->cell_owner[slot->glyph.cell] = 0;
        slot->generation = 0;
    }
    int cell = atlas_take_cell(a);
    if (cell < 0) {
        a->overflow = true;
        return NULL;
    }
    uint32_t *dst = uploads_add(&a->uploads, cell, (size_t)a->cell_size.x *
                                (size_t)a->cell_size.y * sizeof(uint32_t));
    if (!dst) {
        a->overflow = true;
        return NULL;
    }

    int cw = a->cell_size.x, ch = a->cell_size.y;
    int glyph_w = glyph->width > cw ? cw : glyph->width;
    int glyph_h = ch < rows ? ch : rows;
    for (int y = 0; y < ch; y++, dst += cw) {
        if (y < glyph_h) {
            if (glyph->has_color && glyph->color) {
                for (int x = 0; x < glyph_w; x++)
                    dst[x] = straight_color(*glyph_color_at(glyph, y, x));
            } else if (glyph->has_subpx_ink && glyph->subpx) {
                for (int x = 0; x < glyph_w; x++) {
                    uint8_t r = glyph->subpx[y][x * 3 + 0];
                    uint8_t g = glyph->subpx[y][x * 3 + 1];
                    uint8_t b = glyph->subpx[y][x * 3 + 2];
                    uint8_t a = glyph->alpha ? glyph->alpha[y][x] : (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                    dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                }
            } else {
                pack_alpha(dst, glyph->alpha[y], glyph_w);
            }
        } else {
            memset(dst, 0, (size_t)glyph_w * sizeof(*dst));
        }
        memset(dst + glyph_w, 0, (size_t)(cw - glyph_w) * sizeof(*dst));
    }

    uint8_t gmode = glyph->has_color ? GLYPH_COLOR
                  : (glyph->has_subpx_ink ? GLYPH_SUBPIXEL : GLYPH_MONO);

    *slot = (struct gpu_glyph_slot){
        .key = key,
        .style = style,
        .generation = a->reset_count,
        .glyph = {
            .cell = (uint16_t)cell,
            .width = (uint16_t)glyph_w,
            .x_offset = (int16_t)glyph->x_offset,
            .mode = gmode,
        },
    };
    a->cell_owner[cell] = (uint16_t)(slot - a->slots) + 1;
    atlas_mark(a, cell);
    return &slot->glyph;
}

static inline const struct atlas_glyph *atlas_hit_slot(struct glyph_atlas *a,
                                                       struct gpu_glyph_slot *slot)
{
    atlas_mark(a, slot->glyph.cell);
    return &slot->glyph;
}

static inline const struct atlas_glyph *atlas_fetch_glyph(struct glyph_atlas *a, uint64_t key,
                                                          uint16_t style,
                                                          const struct glyph *glyph, int rows)
{
    struct gpu_glyph_slot *fill;
    struct gpu_glyph_slot *slot = atlas_lookup(a, key, style, &fill);
    return slot ? atlas_hit_slot(a, slot) : atlas_insert(a, fill, key, style, glyph, rows);
}

__attribute__((cold, noinline))
static const struct atlas_glyph *ascii_fill(struct glyph_atlas *a, uint32_t cp, uint16_t style,
                                            glyph_fetch_fn get, struct font_instance *font,
                                            int rows)
{
    struct ascii_atlas *s = &a->ascii;
    struct atlas_glyph *entry = &s->glyphs[cp | (uint32_t)style << 7];
    entry->mode = GLYPH_MONO;
    if (!s->texture || cp - ASCII_FIRST >= ASCII_GLYPHS)
        return NULL;
    const struct glyph *glyph = get(font, cp, style);
    if (!glyph || !glyph->width || glyph->has_color || glyph->has_subpx_ink)
        return NULL;
    int cell = (int)(style * ASCII_GLYPHS + cp - ASCII_FIRST);
    int cw = a->cell_size.x, ch = a->cell_size.y;
    uint8_t *dst = uploads_add(&s->uploads, cell, (size_t)cw * (size_t)ch);
    if (!dst)
        return NULL;
    int glyph_w = glyph->width > cw ? cw : glyph->width;
    int glyph_h = ch < rows ? ch : rows;
    for (int y = 0; y < ch; y++, dst += cw) {
        if (y < glyph_h)
            memcpy(dst, glyph->alpha[y], (size_t)glyph_w);
        else
            memset(dst, 0, (size_t)glyph_w);
        memset(dst + glyph_w, 0, (size_t)(cw - glyph_w));
    }
    *entry = (struct atlas_glyph){
        .cell = (uint16_t)cell,
        .width = (uint16_t)glyph_w,
        .x_offset = (int16_t)glyph->x_offset,
        .mode = GLYPH_ASCII,
    };
    return entry;
}

static inline const struct atlas_glyph *ascii_ensure(struct glyph_atlas *a, uint32_t cp,
                                                     uint16_t style, glyph_fetch_fn get,
                                                     struct font_instance *font, int rows)
{
    if ((cp | (uint32_t)(style & ~ASCII_STYLE_MASK) << 7) >= 128)
        return NULL;
    const struct atlas_glyph *g = &a->ascii.glyphs[cp | (uint32_t)style << 7];
    if (__builtin_expect(g->mode == GLYPH_ASCII, 1))
        return g;
    return g->mode == GLYPH_SOLID ? ascii_fill(a, cp, style, get, font, rows) : NULL;
}

static inline const struct atlas_glyph *atlas_fetch_cp(struct glyph_atlas *a, uint64_t key,
                                                       uint32_t cp, uint16_t style,
                                                       glyph_fetch_fn get,
                                                      struct font_instance *font, int rows)
{
    const struct atlas_glyph *g = ascii_ensure(a, cp, style, get, font, rows);
    if (g)
        return g;
    struct gpu_glyph_slot *fill;
    struct gpu_glyph_slot *slot = atlas_lookup(a, key, style, &fill);
    return slot ? atlas_hit_slot(a, slot) : atlas_insert(a, fill, key, style,
                                                         get(font, cp, style), rows);
}

static size_t uploads_bytes(const struct atlas_uploads *u, struct vector2i cell, size_t bpp)
{
    return upload_align((size_t)u->count * (size_t)cell.x * (size_t)cell.y * bpp);
}

static size_t uploads_encode(struct atlas_uploads *u, id<MTLTexture> texture, int cols,
                             struct vector2i cell, size_t bpp,
                             id<MTLBlitCommandEncoder> blit, id<MTLBuffer> staging,
                             size_t offset)
{
    if (!u->count)
        return offset;
    size_t row_bytes = (size_t)cell.x * bpp;
    size_t cell_bytes = row_bytes * (size_t)cell.y;
    memcpy((char *)staging.contents + offset, u->pixels, cell_bytes * u->count);
    for (uint32_t i = 0; i < u->count; i++) {
        struct rect2i dst = grid_rect(cols, cell, u->cells[i], cell.x);
        [blit copyFromBuffer:staging
                sourceOffset:offset + (size_t)i * cell_bytes
           sourceBytesPerRow:row_bytes
         sourceBytesPerImage:cell_bytes
                  sourceSize:MTLSizeMake((NSUInteger)dst.width, (NSUInteger)dst.height, 1)
                   toTexture:texture
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake((NSUInteger)dst.x, (NSUInteger)dst.y, 0)];
    }
    return offset + upload_align(cell_bytes * u->count);
}

static size_t atlas_upload_bytes(const struct glyph_atlas *a)
{
    return a ? uploads_bytes(&a->uploads, a->cell_size, sizeof(uint32_t)) +
               uploads_bytes(&a->ascii.uploads, a->cell_size, 1) : 0;
}

static size_t atlas_encode_uploads(struct glyph_atlas *a, id<MTLBlitCommandEncoder> blit,
                                   id<MTLBuffer> staging, size_t offset)
{
    if (!a)
        return offset;
    offset = uploads_encode(&a->uploads, a->texture, a->cols, a->cell_size,
                            sizeof(uint32_t), blit, staging, offset);
    return uploads_encode(&a->ascii.uploads, a->ascii.texture, a->ascii.cols,
                          a->cell_size, 1, blit, staging, offset);
}

static void atlas_uploads_done(struct glyph_atlas *a)
{
    if (!a)
        return;
    a->uploads.count = 0;
    a->ascii.uploads.count = 0;
}

static void batch_atlas_glyph(struct metal_render *r, struct glyph_atlas *a,
                              struct rect2i scissor, struct vector2i pos,
                              const struct atlas_glyph *g, uint32_t fg, float alpha)
{
    bool ascii = g->mode == GLYPH_ASCII;
    if (!ascii)
        atlas_mark(a, g->cell);
    struct rect2i src = atlas_rect(a, g);
    struct vector2i end = rect2i_end(src);
    float x0 = (float)(pos.x + g->x_offset), y0 = (float)pos.y;
    simd_float4 color = g->mode == GLYPH_COLOR ? simd_make_float4(1, 1, 1, alpha)
                                               : make_color(fg, alpha);
    batch_glyph_quad(r, scissor, x0, y0, x0 + (float)src.width, y0 + (float)src.height,
                     (float)src.x, (float)src.y, (float)end.x, (float)end.y, color,
                     (enum glyph_mode)g->mode, ascii ? a->ascii.texture : a->texture);
}

static void discard_pending(struct metal_render *r)
{
    for (uint32_t i = 0; i < r->cmd_count; i++) {
        [r->cmds[i].texture release];
        [r->cmds[i].ascii release];
    }
    r->failed = false;
    r->glyph_count = 0;
    r->shape_count = 0;
    r->cmd_count = 0;
    r->clear_pending = false;
    r->pixels_src = NULL;
}

void metal_render_begin(struct metal_render *r)
{
    if (r) discard_pending(r);
}

void metal_render_clear(struct metal_render *r, uint32_t color)
{
    if (!r)
        return;
    r->clear_pending = true;
    r->clear_color = color;
}

#define GPU_CELL_QUADS_MAX 6

static inline void emit_cell(struct metal_render *r, struct glyph_atlas *a,
                             const struct gpu_draw *d, int cw, int ch, int origin_y)
{
    uint16_t attrs = d->flags;
    float alpha = (attrs & WSAP_VTCORE_ATTR_FAINT) ? 0.5f : 1.0f;

    if (d->glyph.width) {
        uint32_t sink, *used = d->glyph.mode == GLYPH_ASCII ? &sink
                                                             : a->cell_used + d->glyph.cell;
        *used = a->frame;
        float x0 = (float)(d->col * cw + d->glyph.x_offset);
        float y0 = (float)(origin_y + (int)d->row * ch);
        float w = (float)d->glyph.width, h = (float)a->cell_size.y;
        float u0 = (float)d->tex_x, v0 = (float)d->tex_y;
        simd_float4 color = d->glyph.mode == GLYPH_COLOR ? simd_make_float4(1, 1, 1, alpha)
                                                         : make_color(d->fg, alpha);
        emit_glyph_quad(r, x0, y0, x0 + w, y0 + h, u0, v0, u0 + w, v0 + h, color,
                        (enum glyph_mode)d->glyph.mode);
    }

    if (!render_cell_decorated(attrs))
        return;
    int ys[5], n = 0;
    enum wsap_vtcore_underline ul = wsap_vtcore_attr_underline(attrs);
    if ((ul != WSAP_VTCORE_UNDERLINE_NONE && ul != WSAP_VTCORE_UNDERLINE_DOUBLE) ||
        (attrs & WSAP_VTCORE_ATTR_LINK))
        ys[n++] = ch - 2;
    if (ul == WSAP_VTCORE_UNDERLINE_DOUBLE) { ys[n++] = ch - 3; ys[n++] = ch - 1; }
    if (attrs & WSAP_VTCORE_ATTR_STRIKE) ys[n++] = ch / 2;
    if (attrs & WSAP_VTCORE_ATTR_OVERLINE) ys[n++] = 1;
    simd_float4 dec_color = make_color(d->fg, alpha);
    for (int i = 0; i < n; i++) {
        float lx0 = (float)(d->col * cw);
        float ly0 = (float)(origin_y + d->row * ch + ys[i]);
        emit_glyph_quad(r, lx0, ly0, lx0 + (float)cw, ly0 + 1.0f,
                        0, 0, 0, 0, dec_color, GLYPH_SOLID);
    }
}

static struct gpu_draw gpu_draw_from(const struct glyph_atlas *a,
                                     const struct atlas_glyph *glyph, int col,
                                     int row, uint32_t fg, uint16_t attrs)
{
    struct rect2i src = atlas_rect(a, glyph);
    return (struct gpu_draw){
        .glyph = *glyph,
        .tex_x = (uint16_t)src.x,
        .tex_y = (uint16_t)src.y,
        .flags = attrs,
        .col = (uint16_t)col,
        .row = (uint32_t)row,
        .fg = fg,
    };
}

struct row_scan {
    struct metal_render *r;
    const struct terminal_frame *frame;
    const struct render_backend_view *view;
    const struct wsap_vtcore_cell *cells;
    struct gpu_draw *draws;
    int row, sel0, sel1, cursor, glyph_rows, draw_count;
    int base, next;
};

static inline __attribute__((always_inline)) bool scan_cell(struct glyph_atlas *a, const struct terminal_frame *frame,
                             const struct render_backend_view *view,
                             const struct wsap_vtcore_cell *cells, int col, int row,
                             int sel0, int sel1, int cursor, int glyph_rows,
                             struct gpu_draw *out)
{
    const struct wsap_vtcore_cell *cell = &cells[col];
    if (!(cell->text > ' ' || render_cell_decorated(cell->attrs)) || cell->width == 0)
        return false;
    uint32_t cp = cell->text & WSAP_VTCORE_TEXT_CLUSTER
        ? terminal_frame_glyph(frame, cell->text) : cell->text;
    const struct atlas_glyph *glyph = atlas_fetch_cp(a, cp, cp, render_cell_style(cell),
                                                     view->fetch_glyph, view->font, glyph_rows);
    if (!glyph && !render_cell_decorated(cell->attrs))
        return false;
    static const struct atlas_glyph empty = {0};
    if (!glyph)
        glyph = &empty;
    uint32_t fg = view->resolve_cell(frame, cells, col, sel0, sel1, cursor).fg;
    *out = gpu_draw_from(a, glyph, col, row, fg, cell->attrs);
    return true;
}

static bool scan_pending(struct row_scan *s, int col)
{
    return scan_cell(&s->r->term, s->frame, s->view, s->cells, col, s->row, s->sel0,
                     s->sel1, s->cursor, s->glyph_rows, &s->draws[s->draw_count]);
}

static void scan_span(void *ctx, const struct glyph_span *span)
{
    struct row_scan *s = ctx;
    int start = s->base + span->start;
    for (; s->next < start; s->next++)
        s->draw_count += scan_pending(s, s->next);
    struct glyph_atlas *a = &s->r->term;
    for (int i = 0; i < span->count; i++) {
        int c = start + i;
        const struct wsap_vtcore_cell *ci = &s->cells[c];
        uint64_t key = UINT64_C(0x8000000000000000) |
            (span->id * UINT64_C(0x9E3779B97F4A7C15) + (uint64_t)i);
        const struct atlas_glyph *glyph =
            atlas_fetch_glyph(a, key, render_cell_style(ci), &span->cells[i], s->glyph_rows);
        if (!glyph)
            continue;
        uint32_t fg = s->view->resolve_cell(s->frame, s->cells, c, s->sel0, s->sel1,
                                            s->cursor).fg;
        s->draws[s->draw_count++] = gpu_draw_from(a, glyph, c, s->row, fg, ci->attrs);
    }
    s->next = start + span->count;
}

static void metal_render_scan_row(
    struct metal_render *r, const struct terminal_frame *frame,
    const struct render_backend_view *view, int row, int cols,
    int sel0, int sel1, int cursor_x, int cursor_y, uint32_t default_bg)
{
    const struct wsap_vtcore_cell *cells = view->row(frame, row);
    if (!cells) {
        r->output->row_draw_count[row] = 0;
        r->output->row_bg_count[row] = 0;
        return;
    }

    int cursor = (row == cursor_y) ? cursor_x : -1;
    struct gpu_bg_span *spans = r->output->bg_spans + (size_t)row * cols;
    int bg_count = 0;
    int span_start = -1;
    uint32_t span_bg = 0;

    for (int col = 0; col < cols; col++) {
        uint32_t bg = view->resolve_cell(frame, cells, col,
                                         sel0, sel1, cursor).bg;
        bool same = span_start >= 0 && bg == span_bg;
        if (span_start >= 0 && !same) {
            spans[bg_count++] = (struct gpu_bg_span){
                .col = span_start, .width = col - span_start, .bg = span_bg,
            };
            span_start = -1;
        }
        if (bg != default_bg && span_start < 0) {
            span_start = col;
            span_bg = bg;
        }
    }
    if (span_start >= 0) {
        spans[bg_count++] = (struct gpu_bg_span){
            .col = span_start, .width = cols - span_start, .bg = span_bg,
        };
    }
    r->output->row_bg_count[row] = bg_count;

    struct glyph_atlas *a = &r->term;
    struct gpu_draw *draws = r->output->draws + (size_t)row * cols;
    int glyph_rows = (int)view->cell_h;
    int draw_count = 0;
    for (int col = 0; col < cols;) {
        int end = render_segment_end(cells, col, cols, view->cell_aligned_symbol);
        if (end - col > 1) {
            for (int i = col; i < end; i++)
                r->run_cps[i - col] = cells[i].text;
            struct row_scan scan = {
                .r = r, .frame = frame, .view = view, .cells = cells, .draws = draws,
                .row = row, .sel0 = sel0, .sel1 = sel1, .cursor = cursor,
                .glyph_rows = glyph_rows, .draw_count = draw_count,
                .base = col, .next = col,
            };
            view->shape_segment(view->font, r->run_cps, end - col, render_cell_style(&cells[col]),
                                scan_span, &scan);
            draw_count = scan.draw_count;
            col = scan.next;
        }
        for (; col < end; col++)
            draw_count += scan_cell(a, frame, view, cells, col, row, sel0, sel1, cursor,
                                    glyph_rows, &draws[draw_count]);
    }
    r->output->row_draw_count[row] = draw_count;
}

static bool ensure_row_storage(struct metal_render *r, int cols, int rows)
{
    size_t cell_count = (size_t)cols * rows;
    if (cell_count > r->output->draw_capacity) {
        struct gpu_draw *draws = realloc(r->output->draws, cell_count * sizeof(*draws));
        if (!draws) return false;
        r->output->draws = draws;
        struct gpu_bg_span *bg = realloc(r->output->bg_spans, cell_count * sizeof(*bg));
        if (!bg) return false;
        r->output->bg_spans = bg;
        r->output->draw_capacity = cell_count;
    }
    if ((size_t)rows > r->output->rows_capacity) {
        uint64_t *gens = realloc(r->output->row_gen, (size_t)rows * sizeof(*gens));
        if (!gens) return false;
        r->output->row_gen = gens;
        int *dc = realloc(r->output->row_draw_count, (size_t)rows * sizeof(*dc));
        if (!dc) return false;
        r->output->row_draw_count = dc;
        int *bc = realloc(r->output->row_bg_count, (size_t)rows * sizeof(*bc));
        if (!bc) return false;
        r->output->row_bg_count = bc;
        int *s0 = realloc(r->output->row_sel_c0, (size_t)rows * sizeof(*s0));
        if (!s0) return false;
        r->output->row_sel_c0 = s0;
        int *s1 = realloc(r->output->row_sel_c1, (size_t)rows * sizeof(*s1));
        if (!s1) return false;
        r->output->row_sel_c1 = s1;
        r->output->rows_capacity = (size_t)rows;
    }
    return true;
}

static void metal_output_destroy(void *data)
{
    struct metal_output *output = data;
    free(output->draws);
    free(output->bg_spans);
    free(output->row_gen);
    free(output->row_draw_count);
    free(output->row_bg_count);
    free(output->row_sel_c0);
    free(output->row_sel_c1);
    free(output);
}

static bool metal_output_bind(struct metal_render *r,
                              struct framebuffer_history *history)
{
    if (!history)
        return false;
    if (!history->gpu_data) {
        history->gpu_data = calloc(1, sizeof(struct metal_output));
        if (!history->gpu_data)
            return false;
        history->gpu_destroy = metal_output_destroy;
    }
    r->output = history->gpu_data;
    return true;
}

enum render_backend_result metal_render_terminal(
    struct metal_render *r, struct framebuffer_history *history,
    const struct terminal_frame *frame, const struct render_backend_view *view)
{
    if (!r || !frame || !view || !metal_output_bind(r, history))
        return RENDER_BACKEND_FAILED;
    int cols = view->cols, rows = view->rows;
    int origin_y = frame->origin_y > 0 ? frame->origin_y : 0;
    int cw = (int)view->cell_w, ch = (int)view->cell_h;
    if (cols <= 0 || cols > WSAP_VTCORE_MAX_COLS || rows <= 0 ||
        cw <= 0 || ch <= 0 || cw > GPU_ATLAS_MAX_SIZE / 2 ||
        ch > GPU_ATLAS_MAX_SIZE)
        return RENDER_BACKEND_FAILED;

    struct glyph_atlas *a = &r->term;
    struct vector2i cell = { .x = cw * 2, .y = ch };
    int atlas_size = pick_atlas_size(cell, (size_t)cols * rows);
    if (vector2i_eq(a->cell_size, cell) && a->size > atlas_size)
        atlas_size = a->size;
    uint32_t resets_before = a->reset_count;
    bool full = r->output->atlas_frame != a->frame;
    if (!atlas_layout(a, r->device, cell, atlas_size))
        return RENDER_BACKEND_FAILED;
    if (++a->frame == 0) {
        a->frame = 1;
        atlas_reset(a);
    }
    uint64_t version = view->font_version(view->font);
    if (version != a->font_version) {
        atlas_reset(a);
        a->font_version = version;
    }
    full = full || a->reset_count != resets_before;
    if (!ensure_row_storage(r, cols, rows))
        return RENDER_BACKEND_FAILED;

    uint32_t default_bg = frame->reverse_video ? frame->default_fg : frame->default_bg;
    int cursor_x = frame->cursor_x, cursor_y = frame->cursor_y;
    full = full || !terminal_frame_valid(frame) || !view->row_gen ||
        r->output->cached_cols != cols || r->output->cached_rows != rows ||
        r->output->prev_default_bg != default_bg || r->output->prev_default_fg != frame->default_fg ||
        r->output->prev_cursor_color != frame->cursor_color ||
        r->output->prev_cursor_style != frame->cursor_style ||
        r->output->prev_reverse_video != frame->reverse_video ||
        r->output->prev_origin_y != origin_y || r->output->prev_full_gen != frame->full_gen;
    bool cursor_moved = cursor_x != r->output->prev_cursor_x || cursor_y != r->output->prev_cursor_y;
    for (int retry = 0; retry < GPU_SCAN_RETRY_MAX; retry++) {
        a->overflow = false;
        for (int row = 0; row < rows; row++) {
            int sel0 = -1, sel1 = -2;
            if (view->row_selection)
                view->row_selection(frame, row, cols, &sel0, &sel1);
            uint64_t generation = view->row_gen ? view->row_gen(frame, row) : 0;
            bool cursor_dirty = cursor_moved && (row == cursor_y || row == r->output->prev_cursor_y);
            if (!full && !cursor_dirty && r->output->row_gen[row] == generation &&
                r->output->row_sel_c0[row] == sel0 && r->output->row_sel_c1[row] == sel1)
                continue;
            metal_render_scan_row(r, frame, view, row, cols, sel0, sel1,
                                  cursor_x, cursor_y, default_bg);
            r->output->row_gen[row] = generation;
            r->output->row_sel_c0[row] = sel0;
            r->output->row_sel_c1[row] = sel1;
            if (a->overflow) break;
        }
        if (!a->overflow) break;
        if (a->size < GPU_ATLAS_MAX_SIZE) {
            if (!atlas_layout(a, r->device, cell, a->size * 2))
                return RENDER_BACKEND_FAILED;
        } else {
            atlas_reset(a);
        }
        full = true;
    }
    if (a->overflow) {
        r->output->cached_cols = 0;
        return RENDER_BACKEND_FAILED;
    }

    struct rect2i full_scissor = {
        .pos = { .x = 0, .y = 0 },
        .size = { .x = cols * cw, .y = rows * ch + origin_y },
    };

    metal_render_clear(r, default_bg);

    struct metal_cmd *cmd = add_cmd(r, CMD_GLYPHS, full_scissor, a->texture, a->ascii.texture);
    if (!cmd)
        return RENDER_BACKEND_FAILED;
    uint32_t cmd_index = (uint32_t)(cmd - r->cmds);
    uint32_t first_vertex = r->glyph_count;

    uint32_t total_verts = 0;
    for (int row = 0; row < rows; row++)
        total_verts += (uint32_t)r->output->row_bg_count[row] +
                       (uint32_t)r->output->row_draw_count[row] * GPU_CELL_QUADS_MAX;
    if (total_verts && !ensure_glyph_verts(r, total_verts))
        return RENDER_BACKEND_FAILED;

    for (int row = 0; row < rows; row++) {
        int bg_count = r->output->row_bg_count[row];
        const struct gpu_bg_span *spans = r->output->bg_spans + (size_t)row * cols;
        for (int i = 0; i < bg_count; i++) {
            float x0 = (float)(spans[i].col * cw);
            float y0 = (float)(origin_y + row * ch);
            emit_glyph_quad(r, x0, y0, x0 + (float)(spans[i].width * cw), y0 + (float)ch,
                            0, 0, 0, 0, make_color(spans[i].bg, 1.0f), GLYPH_SOLID);
        }
    }

    for (int row = 0; row < rows; row++) {
        int draw_count = r->output->row_draw_count[row];
        const struct gpu_draw *draws = r->output->draws + (size_t)row * cols;
        for (int i = 0; i < draw_count; i++)
            emit_cell(r, a, &draws[i], cw, ch, origin_y);
    }
    r->cmds[cmd_index].count += r->glyph_count - first_vertex;

    if (cursor_y >= 0 && cursor_y < rows && frame->cursor_style != WSAP_VTCORE_CURSOR_BLOCK) {
        float x0 = (float)(cursor_x * cw), y0 = (float)(origin_y + cursor_y * ch);
        bool bar = frame->cursor_style == WSAP_VTCORE_CURSOR_BAR;
        float x1 = bar ? x0 + 2.0f : x0 + (float)cw;
        float y1 = y0 + (float)ch;
        if (!bar) y0 = y1 - 2.0f;
        batch_glyph_quad(r, full_scissor, x0, y0, x1, y1, 0, 0, 0, 0,
                         make_color(frame->cursor_color, 1.0f), GLYPH_SOLID, nil);
    }

    r->output->cached_cols = cols;
    r->output->cached_rows = rows;
    r->output->prev_cursor_x = cursor_x;
    r->output->prev_cursor_y = cursor_y;
    r->output->prev_cursor_style = frame->cursor_style;
    r->output->prev_default_bg = default_bg;
    r->output->prev_default_fg = frame->default_fg;
    r->output->prev_cursor_color = frame->cursor_color;
    r->output->prev_reverse_video = frame->reverse_video;
    r->output->prev_origin_y = origin_y;
    r->output->prev_full_gen = frame->full_gen;
    r->output->atlas_frame = a->frame;

    return r->failed ? RENDER_BACKEND_FAILED : RENDER_BACKEND_DREW;
}

#define UI_GLYPH_KEY(cp) ((uint64_t)(cp) | UINT64_C(0x4000000000000000))

static void stage_scene_glyphs(struct glyph_atlas *a, const struct ui_scene *scene,
                               struct font_instance *font)
{
    int rows = font_instance_metrics(font)->cell_h;
    for (int i = 0; i < scene->n_ops && !a->overflow; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind != UI_SCENE_OP_TEXT) continue;
        const char *text = ui_scene_op_text(scene, op);
        if (!text || op->style.advance <= 0 || op->style.cell_h <= 0) continue;
        uint16_t flags = render_text_style(op->style.flags);
        for (uint32_t n = 0; n < op->text_len;) {
            uint32_t cp;
            int cells;
            n += (uint32_t)ui_text_run_next(text + n, op->text_len - n, &cp, &cells);
            (void)atlas_fetch_cp(a, UI_GLYPH_KEY(cp), cp, flags, glyph_cache_fetch, font, rows);
        }
    }
}

static bool prepare_ui_atlas(struct metal_render *r, const struct ui_scene *scene,
                             struct font_instance *font)
{
    if (!r->ui) {
        r->ui = calloc(1, sizeof(*r->ui));
        if (!r->ui) return false;
    }
    struct glyph_atlas *a = r->ui;
    a->frame = r->frame;
    const struct font_metrics *metrics = font_instance_metrics(font);
    struct vector2i cell = { .x = metrics->cell_w * 2, .y = metrics->cell_h };
    if (cell.x <= 0 || cell.y <= 0 || cell.x > GPU_ATLAS_MAX_SIZE || cell.y > GPU_ATLAS_MAX_SIZE)
        return false;
    int size = pick_atlas_size(cell, 1024);
    if (vector2i_eq(a->cell_size, cell) && a->size > size)
        size = a->size;
    if (!atlas_layout(a, r->device, cell, size))
        return false;
    uint64_t version = font_instance_version(font);
    if (font != r->ui_font || version != a->font_version) {
        atlas_reset(a);
        font_instance_unref(r->ui_font);
        r->ui_font = font_instance_ref(font);
        a->font_version = version;
    }
    for (int retry = 0; retry < GPU_SCAN_RETRY_MAX; retry++) {
        a->overflow = false;
        stage_scene_glyphs(a, scene, font);
        if (!a->overflow)
            return true;
        if (a->size >= GPU_ATLAS_MAX_SIZE)
            return false;
        if (!atlas_layout(a, r->device, cell, a->size * 2))
            return false;
    }
    return false;
}

static id<MTLTexture> scene_image_texture(struct metal_render *r, const struct ui_image *image)
{
    struct image_entry *slot = &r->images[0];
    for (int i = 0; i < GPU_IMAGE_CACHE; i++) {
        struct image_entry *e = &r->images[i];
        if (e->texture && e->src == image && e->gen == image->gen &&
            vector2i_eq(e->size, image->size)) {
            e->used = r->frame + 1;
            return e->texture;
        }
        if (!e->texture || e->used < slot->used)
            slot = e;
    }
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:(NSUInteger)image->size.x height:(NSUInteger)image->size.y mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [r->device newTextureWithDescriptor:desc];
    if (!texture)
        return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->size.x,
                                           (NSUInteger)image->size.y)
               mipmapLevel:0 withBytes:image->pixels
               bytesPerRow:(size_t)image->size.x * sizeof(uint32_t)];
    [slot->texture release];
    *slot = (struct image_entry){
        .texture = texture,
        .src = image,
        .gen = image->gen,
        .size = image->size,
        .used = r->frame + 1,
    };
    return texture;
}

bool metal_render_scene(struct metal_render *r, const struct ui_scene *scene,
                        struct font_instance *font)
{
    if (!r || !ui_scene_is_valid(scene))
        return false;
    if (font && !prepare_ui_atlas(r, scene, font))
        return false;
    struct glyph_atlas *atlas = font ? r->ui : NULL;
    int glyph_rows = font ? font_instance_metrics(font)->cell_h : 0;

    struct rect2i surface_clip = {
        .pos = { .x = 0, .y = 0 },
        .size = { .x = 32767, .y = 32767 },
    };
    int need_cap = scene->clips_cap + 1;
    if (need_cap > r->clip_stack_cap) {
        struct rect2i *stack = realloc(r->clip_stack, (size_t)need_cap * sizeof(*stack));
        if (!stack) return false;
        r->clip_stack = stack;
        r->clip_stack_cap = need_cap;
    }
    struct rect2i *clip_stack = r->clip_stack;
    int clip_n = 0;
    struct rect2i clip = surface_clip;

    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_PUSH_CLIP) {
            clip_stack[clip_n++] = clip;
            clip = rect2i_intersect(rect2i_intersect(clip, op->rect), surface_clip);
            continue;
        } else if (op->kind == UI_SCENE_OP_POP_CLIP) {
            if (clip_n > 0) clip = clip_stack[--clip_n];
            continue;
        }
        if (rect2i_is_empty(clip))
            continue;

        if (op->kind == UI_SCENE_OP_RECT || op->kind == UI_SCENE_OP_BLEND_RECT) {
            simd_float4 color = make_color(op->color, 1.0f);
            if (op->kind == UI_SCENE_OP_BLEND_RECT) color.w = op->alpha / 255.0f;
            batch_shape_quad(r, clip, op->rect, color, 0.0f, 0, (uint32_t)(color.w * 255.0f));
        } else if (op->kind == UI_SCENE_OP_ROUNDED_RECT) {
            simd_float4 color = make_color(op->color, 1.0f);
            batch_shape_quad(r, clip, op->rect, color, (float)op->radius, 1, (uint32_t)(color.w * 255.0f));
        } else if (op->kind == UI_SCENE_OP_CIRCLE) {
            simd_float4 color = make_color(op->color, 1.0f);
            color.w = op->alpha / 255.0f;
            batch_shape_quad(r, clip, op->rect, color, (float)op->radius, 2, op->alpha);
        } else if (op->kind == UI_SCENE_OP_SHADOW) {
            simd_float4 color = make_color(op->color, 1.0f);
            batch_shape_quad(r, clip, op->rect, color, (float)op->radius, 3, (uint32_t)op->blur);
        } else if (op->kind == UI_SCENE_OP_IMAGE) {
            const struct ui_image *image = op->image;
            if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
                continue;
            id<MTLTexture> texture = scene_image_texture(r, image);
            if (!texture) { r->failed = true; break; }
            batch_glyph_quad(r, clip, op->rect.x, op->rect.y,
                op->rect.x + op->rect.width, op->rect.y + op->rect.height,
                0, 0, 1, 1, simd_make_float4(1, 1, 1, 1), GLYPH_IMAGE, texture);
        } else if (op->kind == UI_SCENE_OP_TEXT) {
            if (!atlas) continue;
            const char *text = ui_scene_op_text(scene, op);
            if (!text || op->style.advance <= 0 || op->style.cell_h <= 0) continue;
            struct rect2i text_clip = rect2i_intersect(clip, ui_scene_text_ink(scene, op));
            if (rect2i_is_empty(text_clip)) continue;
            uint16_t flags = render_text_style(op->style.flags);
            int tx = op->rect.pos.x;
            for (uint32_t n = 0; n < op->text_len;) {
                uint32_t cp;
                int cells;
                n += (uint32_t)ui_text_run_next(text + n, op->text_len - n, &cp, &cells);
                const struct atlas_glyph *g = atlas_fetch_cp(atlas, UI_GLYPH_KEY(cp), cp, flags,
                                                             glyph_cache_fetch, font, glyph_rows);
                if (g) {
                    struct vector2i pos = { .x = tx, .y = op->rect.pos.y };
                    batch_atlas_glyph(r, atlas, text_clip, pos, g, op->color, 1.0f);
                }
                tx += cells * op->style.advance;
            }
        }
    }
    return !r->failed;
}

bool metal_render_pixels(struct metal_render *r, const uint32_t *pixels,
                         struct vector2i size)
{
    if (!r || !pixels || size.x <= 0 || size.y <= 0)
        return false;
    if (!r->pixels_tex || !vector2i_eq(r->pixels_size, size)) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:(NSUInteger)size.x height:(NSUInteger)size.y mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [r->device newTextureWithDescriptor:desc];
        if (!texture)
            return false;
        [r->pixels_tex release];
        r->pixels_tex = texture;
        r->pixels_size = size;
    }
    r->pixels_src = pixels;
    struct rect2i all = { .pos = { .x = 0, .y = 0 }, .size = size };
    batch_glyph_quad(r, all, 0, 0, (float)size.x, (float)size.y, 0, 0, 1, 1,
                     simd_make_float4(1, 1, 1, 1), GLYPH_IMAGE, r->pixels_tex);
    return !r->failed;
}

static bool ensure_buffer(id<MTLDevice> device, id<MTLBuffer> *buffer, size_t bytes)
{
    if (*buffer && (*buffer).length >= bytes)
        return true;
    size_t cap = *buffer ? (*buffer).length : 65536;
    while (cap < bytes) cap *= 2;
    id<MTLBuffer> next = [device newBufferWithLength:cap options:MTLResourceStorageModeShared];
    if (!next)
        return false;
    [*buffer release];
    *buffer = next;
    return true;
}

static bool encode_uploads(struct metal_render *r, struct metal_frame_resources *frame,
                           id<MTLCommandBuffer> cmd)
{
    size_t pixel_bytes = r->pixels_src
        ? (size_t)r->pixels_size.x * (size_t)r->pixels_size.y * sizeof(uint32_t) : 0;
    size_t bytes = atlas_upload_bytes(&r->term) + atlas_upload_bytes(r->ui) + pixel_bytes;
    if (!bytes)
        return true;
    if (!ensure_buffer(r->device, &frame->upload, bytes))
        return false;
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    if (!blit)
        return false;
    size_t offset = atlas_encode_uploads(&r->term, blit, frame->upload, 0);
    offset = atlas_encode_uploads(r->ui, blit, frame->upload, offset);
    if (pixel_bytes) {
        size_t row_bytes = (size_t)r->pixels_size.x * sizeof(uint32_t);
        memcpy((char *)frame->upload.contents + offset, r->pixels_src, pixel_bytes);
        [blit copyFromBuffer:frame->upload
                sourceOffset:offset
           sourceBytesPerRow:row_bytes
         sourceBytesPerImage:pixel_bytes
                  sourceSize:MTLSizeMake((NSUInteger)r->pixels_size.x,
                                         (NSUInteger)r->pixels_size.y, 1)
                   toTexture:r->pixels_tex
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [blit endEncoding];
    return true;
}

bool metal_render_submit(struct metal_render *r, mtl_drawable_t drawable,
                         mtl_pass_desc_t pass_desc, struct vector2i viewport,
                         bool transaction)
{
    if (!r) return false;
    if (!pass_desc || !pass_desc.colorAttachments[0].texture ||
        viewport.x <= 0 || viewport.y <= 0 || r->failed) {
        discard_pending(r);
        return false;
    }
    struct metal_frame_resources *frame = &r->frames[r->next_frame];
    dispatch_time_t timeout = transaction
        ? dispatch_time(DISPATCH_TIME_NOW, 16000000)
        : dispatch_time(DISPATCH_TIME_NOW, 16000000);
    if (dispatch_semaphore_wait(frame->ready, timeout) != 0) {
        discard_pending(r);
        return false;
    }
    size_t glyph_bytes = (size_t)r->glyph_count * sizeof(*r->glyph_verts);
    size_t shape_bytes = (size_t)r->shape_count * sizeof(*r->shape_verts);
    id<MTLCommandBuffer> cmd = nil;
    if ((glyph_bytes && !ensure_buffer(r->device, &frame->glyphs, glyph_bytes)) ||
        (shape_bytes && !ensure_buffer(r->device, &frame->shapes, shape_bytes)) ||
        !(cmd = [r->queue commandBuffer]) || !encode_uploads(r, frame, cmd)) {
        dispatch_semaphore_signal(frame->ready);
        discard_pending(r);
        return false;
    }
    if (glyph_bytes)
        memcpy(frame->glyphs.contents, r->glyph_verts, glyph_bytes);
    if (shape_bytes)
        memcpy(frame->shapes.contents, r->shape_verts, shape_bytes);
    if (r->clear_pending) {
        simd_float4 cc = make_color(r->clear_color, 1.0f);
        pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(cc.x, cc.y, cc.z, cc.w);
    }

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass_desc];
    if (!enc) {
        dispatch_semaphore_signal(frame->ready);
        discard_pending(r);
        return false;
    }
    [enc setFragmentSamplerState:r->sampler_state atIndex:0];
    struct metal_uniforms uniforms = {
        .viewport_size = simd_make_float2((float)viewport.x, (float)viewport.y),
    };
    [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];

    enum metal_cmd_type bound = (enum metal_cmd_type)-1;
    for (uint32_t i = 0; i < r->cmd_count; i++) {
        struct metal_cmd *c = &r->cmds[i];
        if (c->count == 0) continue;
        struct rect2i clip = c->scissor;
        if (!rect2i_clip(&clip, viewport))
            continue;
        [enc setScissorRect:(MTLScissorRect){ (NSUInteger)clip.x, (NSUInteger)clip.y,
                                              (NSUInteger)clip.width, (NSUInteger)clip.height }];
        if (c->type != bound) {
            bool glyphs = c->type == CMD_GLYPHS;
            [enc setRenderPipelineState:glyphs ? r->glyph_pipeline : r->shape_pipeline];
            [enc setVertexBuffer:glyphs ? frame->glyphs : frame->shapes offset:0 atIndex:0];
            bound = c->type;
        }
        if (c->type == CMD_GLYPHS) {
            [enc setFragmentTexture:c->texture ? c->texture : r->solid_tex atIndex:0];
            [enc setFragmentTexture:c->ascii ? c->ascii : r->solid_tex atIndex:1];
        }
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
              instanceCount:c->count baseInstance:c->start];
    }
    [enc endEncoding];

    dispatch_semaphore_t ready = frame->ready;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError)
            fprintf(stderr, "[metal] frame failed: %s\n",
                    completed.error.localizedDescription.UTF8String);
        dispatch_semaphore_signal(ready);
    }];
    if (drawable && !transaction)
        [cmd presentDrawable:drawable];
    [cmd commit];
    if (drawable && transaction) {
        [cmd waitUntilScheduled];
        [drawable present];
    }
    atlas_uploads_done(&r->term);
    atlas_uploads_done(r->ui);
    r->next_frame = (r->next_frame + 1) % METAL_FRAME_COUNT;
    if (++r->frame == 0)
        r->frame = 1;
    bool ok = true;
    if (!drawable) {
        [cmd waitUntilCompleted];
        ok = cmd.status != MTLCommandBufferStatusError;
    }
    discard_pending(r);
    return ok;
}

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device, id<MTLLibrary> lib,
                                                NSString *vertex_name, NSString *fragment_name)
{
    MTLRenderPipelineDescriptor *pdesc = [[MTLRenderPipelineDescriptor alloc] init];
    pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pdesc.colorAttachments[0].blendingEnabled = YES;
    pdesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pdesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pdesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pdesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLFunction> vertex = [lib newFunctionWithName:vertex_name];
    id<MTLFunction> fragment = [lib newFunctionWithName:fragment_name];
    pdesc.vertexFunction = vertex;
    pdesc.fragmentFunction = fragment;
    NSError *err = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:pdesc error:&err];
    if (!pipeline)
        fprintf(stderr, "[metal] pipeline %s failed: %s\n", vertex_name.UTF8String,
                err.localizedDescription.UTF8String);
    [vertex release];
    [fragment release];
    [pdesc release];
    return pipeline;
}

static bool pipelines_init(struct metal_render *r, id<MTLDevice> device)
{
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:kMetalShaderSource options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "[metal] shader compilation failed: %s\n",
                err.localizedDescription.UTF8String);
        return false;
    }
    id<MTLRenderPipelineState> glyph = make_pipeline(device, lib, @"glyph_vertex", @"glyph_fragment");
    id<MTLRenderPipelineState> shape = make_pipeline(device, lib, @"ui_shape_vertex", @"ui_shape_fragment");
    [lib release];

    MTLSamplerDescriptor *sdesc = [[MTLSamplerDescriptor alloc] init];
    sdesc.minFilter = MTLSamplerMinMagFilterNearest;
    sdesc.magFilter = MTLSamplerMinMagFilterNearest;
    sdesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sdesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sdesc];
    [sdesc release];

    MTLTextureDescriptor *tdesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:1 height:1 mipmapped:NO];
    tdesc.storageMode = MTLStorageModeShared;
    id<MTLTexture> solid = [device newTextureWithDescriptor:tdesc];
    if (solid) {
        uint32_t white = UINT32_MAX;
        [solid replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                   withBytes:&white bytesPerRow:sizeof(white)];
    }
    if (!glyph || !shape || !sampler || !solid) {
        [glyph release];
        [shape release];
        [sampler release];
        [solid release];
        return false;
    }
    r->glyph_pipeline = glyph;
    r->shape_pipeline = shape;
    r->sampler_state = sampler;
    r->solid_tex = solid;
    return true;
}

struct metal_render *metal_render_create(mtl_device_t device)
{
    if (!device)
        return NULL;
    struct metal_render *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->device = [device retain];
    if (!pipelines_init(r, device)) {
        metal_render_destroy(r);
        return NULL;
    }
    r->frame = 1;
    for (unsigned i = 0; i < METAL_FRAME_COUNT; i++)
        r->frames[i].ready = dispatch_semaphore_create(1);
    r->queue = [device newCommandQueue];
    if (!r->queue) {
        metal_render_destroy(r);
        return NULL;
    }
    return r;
}

void metal_render_destroy(struct metal_render *r)
{
    if (!r)
        return;
    for (unsigned i = 0; i < METAL_FRAME_COUNT; i++) {
        struct metal_frame_resources *frame = &r->frames[i];
        if (frame->ready) {
            dispatch_semaphore_wait(frame->ready, DISPATCH_TIME_FOREVER);
            dispatch_semaphore_signal(frame->ready);
            dispatch_release(frame->ready);
        }
        [frame->glyphs release];
        [frame->shapes release];
        [frame->upload release];
    }
    discard_pending(r);
    atlas_release(&r->term);
    if (r->ui) {
        atlas_release(r->ui);
        free(r->ui);
    }
    font_instance_unref(r->ui_font);
    for (int i = 0; i < GPU_IMAGE_CACHE; i++)
        [r->images[i].texture release];
    [r->pixels_tex release];
    [r->solid_tex release];
    [r->sampler_state release];
    [r->glyph_pipeline release];
    [r->shape_pipeline release];
    [r->queue release];
    [r->device release];
    free(r->glyph_verts);
    free(r->shape_verts);
    free(r->cmds);
    free(r->clip_stack);
    free(r);
}

enum render_backend_result render_gpu_terminal(struct framebuffer_gpu *g,
                                               const struct terminal_frame *frame,
                                               const struct render_backend_view *view)
{
    if (g->disabled)
        return RENDER_BACKEND_SKIP;
    if (metal_render_terminal(g->metal, g->history, frame, view) == RENDER_BACKEND_DREW) {
        g->presented = true;
        return RENDER_BACKEND_DREW;
    }
    g->disabled = true;
    g->presented = false;
    return RENDER_BACKEND_SKIP;
}

bool render_gpu_ui_scene(struct framebuffer_gpu *g, const struct ui_scene *scene,
                         struct font_instance *font)
{
    if (g->disabled)
        return false;
    if (!g->presented)
        metal_render_clear(g->metal, g->clear_color);
    if (metal_render_scene(g->metal, scene, font)) {
        g->presented = true;
        return true;
    }
    g->disabled = true;
    g->redraw = g->presented;
    return false;
}

struct framebuffer_gpu *render_gpu_create(void)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        return NULL;
    struct framebuffer_gpu *g = calloc(1, sizeof(*g));
    if (g)
        g->metal = metal_render_create(device);
    [device release];
    if (g && !g->metal) {
        free(g);
        g = NULL;
    }
    return g;
}

void render_gpu_destroy(struct framebuffer_gpu *g)
{
    if (!g)
        return;
    metal_render_destroy(g->metal);
    free(g);
}

void *render_gpu_device(struct framebuffer_gpu *g)
{
    return g ? g->metal->device : NULL;
}

bool render_gpu_begin(struct framebuffer_gpu *g, struct framebuffer *fb,
                      uint32_t base)
{
    if (!g || !fb || !fb->drawable)
        return false;
    g->history = fb->history;
    g->clear_color = base;
    g->presented = false;
    g->disabled = false;
    g->redraw = false;
    fb->presented = false;
    metal_render_begin(g->metal);
    return true;
}

bool render_gpu_redraw(struct framebuffer_gpu *g)
{
    return g && g->redraw;
}

bool render_gpu_submit(struct framebuffer_gpu *g, struct framebuffer *fb)
{
    if (!g || !fb || !fb->drawable)
        return false;
    if (g->disabled || !g->presented) {
        metal_render_begin(g->metal);
        if (!metal_render_pixels(g->metal, fb->pixels, fb->size))
            return false;
    }
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)fb->drawable;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(
        ((g->clear_color >> 16) & 0xff) / 255.0,
        ((g->clear_color >> 8) & 0xff) / 255.0,
        (g->clear_color & 0xff) / 255.0, 1.0);
    fb->presented = metal_render_submit(g->metal, drawable, pass, fb->size,
                                        fb->transaction);
    pass.colorAttachments[0].texture = nil;
    return fb->presented;
}
