#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>
#import <simd/simd.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint/framebuffer.h"
#include "render/gpu/build.h"
#include "render/gpu/gpu.h"
#include "render/render.h"
#include "render/surface.h"
#include "util/damage.h"
#include "util/geom.h"

#include "metal_shader_source.h"

/*
 * The Metal backend. See render/gpu/gpu.h for the rules of a backend, and
 * render/gpu/build.h for the frame data that it draws.
 *
 * One frame is one command buffer. render_gpu_draw() builds the frame data
 * on the CPU. Then a blit pass copies the new glyph pixels into the atlas
 * texture, and one render pass draws the commands of the frame in order.
 * The frame owns a slot of three: its instance buffer, its upload buffer,
 * and a semaphore that the command buffer signals.
 *
 * Textures. The atlas is one texture, and each UI image slot of the builder
 * has one. A command buffer keeps the textures that it uses, so a texture
 * can be replaced while a frame is in flight.
 *
 * Failure. When a built frame does not reach the GPU, the backend calls
 * gpu_build_abort(). The frame is then RENDER_GPU_UNUSED and the CPU paints
 * it, or it is RENDER_GPU_DROPPED and the window asks again.
 *
 * Blending. All pipelines write premultiplied color and blend in the
 * sRGB space of the drawable. Subpixel glyphs draw in three passes, one for
 * each color write mask.
 */

#define FRAME_SLOTS 3
#define BUFFER_MIN 65536

struct uniforms {
    simd_float2 viewport_size;
};

struct frame_slot {
    id<MTLBuffer> instances;
    id<MTLBuffer> upload;
    dispatch_semaphore_t ready;
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

    struct gpu_build build;
    /* The atlas texture, and the texture of each image slot of the builder. */
    id<MTLTexture> atlas;
    id<MTLTexture> images[GPU_IMAGES];
};

/* The state of one render surface: the CPU pixels, on the GPU. */
struct gpu_surface {
    id<MTLTexture> pixels;
    struct vector2i size;
};

/* ---- Textures and buffers ----------------------------------------------- */

static id<MTLTexture> texture_new(struct render_gpu *g, struct vector2i size,
                                  MTLStorageMode storage)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)size.x
                                                          height:(NSUInteger)size.y
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = storage;
    return [g->device newTextureWithDescriptor:d];
}

/*
 * Makes each texture of the frame fit what the builder asks for: an atlas
 * of the size of the builder, and the pixels of each new image. In-flight
 * frames keep the textures that they use.
 */
static bool textures_sync(struct render_gpu *g)
{
    const struct gpu_build *b = &g->build;
    if (!g->atlas || g->atlas.width != (NSUInteger)b->atlas.size) {
        const struct vector2i size = { .x = b->atlas.size, .y = b->atlas.size };
        id<MTLTexture> t = texture_new(g, size, MTLStorageModePrivate);
        if (!t)
            return false;
        [g->atlas release];
        g->atlas = t;
    }
    for (int i = 0; i < GPU_IMAGES; i++) {
        if (!gpu_image_needs_upload(b, i))
            continue;
        /* New pixels get a new texture, because an in-flight frame can
         * still read the old one. */
        const struct gpu_image *image = &b->images[i];
        id<MTLTexture> t = texture_new(g, image->size, MTLStorageModeShared);
        if (!t)
            return false;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image->size.x,
                                         (NSUInteger)image->size.y)
             mipmapLevel:0
               withBytes:image->pixels
             bytesPerRow:(NSUInteger)image->size.x * 4];
        [g->images[i] release];
        g->images[i] = t;
    }
    return true;
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

/* ---- Frame slots and submission ----------------------------------------- */

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
    const struct gpu_atlas *a = &g->build.atlas;
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
    const struct gpu_atlas *a = &g->build.atlas;
    if (!a->n_uploads)
        return true;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!blit)
        return false;
    for (uint32_t i = 0; i < a->n_uploads; i++) {
        const struct gpu_upload *u = &a->uploads[i];
        [blit copyFromBuffer:slot->upload
                sourceOffset:u->offset
           sourceBytesPerRow:(NSUInteger)u->w * 4
         sourceBytesPerImage:(NSUInteger)u->w * (NSUInteger)u->h * 4
                  sourceSize:MTLSizeMake(u->w, u->h, 1)
                   toTexture:g->atlas
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(u->x, u->y, 0)];
    }
    [blit endEncoding];
    return true;
}

static MTLClearColor clear_color_of(uint32_t argb)
{
    float c[4];
    gpu_color(argb, 1.0f, c);
    return MTLClearColorMake(c[0], c[1], c[2], c[3]);
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

/*
 * Moves the frame data of the builder into the slot and the textures, and
 * writes the offset of each instance stream. No drawable is held while
 * this runs.
 */
static bool frame_stage(struct render_gpu *g, struct frame_slot *slot,
                        size_t offsets[GPU_ST_COUNT])
{
    size_t bytes = gpu_build_instance_layout(&g->build, offsets);
    if (!textures_sync(g) || !ensure_buffer(g, &slot->instances, bytes) ||
        !stage_uploads(g, slot))
        return false;
    gpu_build_copy_instances(&g->build, offsets, slot->instances.contents);
    return true;
}

/* Encodes the staged frame into the drawable of `fb`, and commits it. */
static bool encode_frame(struct render_gpu *g, struct frame_slot *slot, struct framebuffer *fb,
                         const size_t offsets[GPU_ST_COUNT], uint32_t base)
{
    const struct gpu_build *b = &g->build;
    id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)fb->drawable;
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
    [enc setFragmentTexture:g->atlas atIndex:0];

    const struct rect2i canvas_rect = { .size = fb->size };
    id<MTLRenderPipelineState> bound = nil;
    for (uint32_t i = 0; i < b->n_cmds; i++) {
        const struct gpu_cmd *c = &b->cmds[i];
        struct rect2i scissor = c->scissor;
        if (!c->count || !rect2i_clip(&scissor, canvas_rect.size))
            continue;
        set_scissor(enc, scissor);
        [enc setFragmentTexture:c->image ? g->images[c->image - 1] : g->atlas atIndex:1];
        [enc setVertexBuffer:slot->instances
                      offset:offsets[c->stream] + (size_t)c->start * gpu_stream_stride(c->stream)
                     atIndex:0];
        int passes = c->kind == GPU_CMD_SUBPX ? 3 : 1;
        for (int ch = 0; ch < passes; ch++) {
            id<MTLRenderPipelineState> pipeline = c->kind == GPU_CMD_SHAPE ? g->shape_pipeline
                : c->kind == GPU_CMD_SUBPX ? g->subpx_pipeline[ch] : g->glyph_pipeline;
            if (pipeline != bound) {
                [enc setRenderPipelineState:pipeline];
                bound = pipeline;
            }
            if (c->kind != GPU_CMD_SHAPE) {
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
        if (gpu_build_frame(&g->build, frame, fb->size)) {
            size_t offsets[GPU_ST_COUNT];
            if (frame_stage(g, slot, offsets)) {
                id<CAMetalDrawable> drawable = frame_drawable(fb);
                /* The drawable is gone, or a resize changed its size after
                 * the frame data was built. Nothing is drawn or presented. */
                if (!drawable || !drawable_size_ok(fb, drawable))
                    status = RENDER_GPU_DROPPED;
                else if (encode_frame(g, slot, fb, offsets, frame->base))
                    status = RENDER_GPU_PRESENTED;
            }
            if (status == RENDER_GPU_PRESENTED)
                gpu_build_commit(&g->build);
            else
                gpu_build_abort(&g->build);
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
    id<MTLTexture> t = texture_new(g, size, MTLStorageModePrivate);
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
    struct rect2i rects[DAMAGE_RECTS_MAX];
    size_t bytes = 0;
    int n = gpu_pixels_rects(fb, damage, fresh, rects, &bytes);

    /* The quad of the copy shares the instance buffer of the slot. */
    struct gpu_glyph_inst quad;
    gpu_pixels_quad(fb->size, &quad);
    if (!ensure_buffer(g, &slot->instances, sizeof(quad)) ||
        (bytes && !ensure_buffer(g, &slot->upload, bytes)))
        return false;
    memcpy(slot->instances.contents, &quad, sizeof(quad));

    id<MTLCommandBuffer> cb = [g->queue commandBuffer];
    if (!cb)
        return false;
    if (bytes) {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        if (!blit)
            return false;
        gpu_pixels_pack(fb, rects, n, slot->upload.contents);
        size_t offset = 0;
        for (int i = 0; i < n; i++) {
            const struct rect2i *r = &rects[i];
            size_t row_bytes = (size_t)r->width * 4;
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
    for (int i = 0; i < FRAME_SLOTS; i++)
        g->slots[i].ready = dispatch_semaphore_create(1);
    /* textures_sync() makes the first atlas texture. */
    if (!g->queue || !gpu_build_init(&g->build) || !pipelines_create(g) || !textures_sync(g)) {
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
    for (int i = 0; i < GPU_IMAGES; i++)
        [g->images[i] release];
    [g->atlas release];
    [g->glyph_pipeline release];
    for (int i = 0; i < 3; i++)
        [g->subpx_pipeline[i] release];
    [g->shape_pipeline release];
    [g->copy_pipeline release];
    [g->queue release];
    [g->device release];
    gpu_build_release(&g->build);
    free(g);
}

void *render_gpu_native_device(struct render_gpu *g)
{
    return g ? (void *)g->device : NULL;
}
