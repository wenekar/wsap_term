#import <Metal/Metal.h>
#include "render/backends/metal/render.h"
#include "font/font.h"
#include "render/render.h"
#include "render/backend.h"
#include "render/cell.h"
#include "render/term_frame.h"
#include "paint/scene.h"
#include "font/cell_draw.h"
#include "font/glyph_cache.h"
#include "term_api.h"
#include "helpers/flood.h"
#include <assert.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct wsap_vtcore_cell TEST_ROW[] = {
    { .text = 'H', .fg = 0xFFFFFFFFu, .bg = 0xFF112233u, .width = 1 },
    { .text = 'i', .fg = 0xFFFFFFFFu, .bg = 0xFF112233u, .width = 1 },
};

static const struct wsap_vtcore_cell *dummy_row(const struct terminal_frame *f, int row)
{
    (void)f;
    return (row == 0) ? TEST_ROW : NULL;
}

static void dummy_selection(const struct terminal_frame *f, int row, int cols, int *c0, int *c1)
{
    (void)f; (void)row; (void)cols;
    *c0 = -1; *c1 = -2;
}

static struct wsap_vtcore_cell dummy_resolve_cell(const struct terminal_frame *f,
                                           const struct wsap_vtcore_cell *cells, int c,
                                           int sel_c0, int sel_c1, int cursor)
{
    (void)f; (void)sel_c0; (void)sel_c1; (void)cursor;
    return cells[c];
}

static uint64_t dummy_row_gen(const struct terminal_frame *f, int row)
{
    (void)f; (void)row;
    return 1;
}

static uint64_t dummy_font_gen(const struct font_instance *fi)
{
    (void)fi;
    return 1;
}

static const struct glyph *dummy_glyph(struct font_instance *fi, uint32_t cp, uint16_t flags)
{
    (void)fi; (void)flags;
    static uint32_t color[40];
    static struct glyph glyph = { .width = 2, .color = color, .has_color = true };
    for (int i = 0; i < 40; i++) color[i] = 0xFFF01020u;
    return cp == 'H' ? &glyph : NULL;
}

static void dummy_shape(struct font_instance *fi, const uint32_t *cps, int count,
                        uint16_t flags, glyph_span_fn emit, void *ctx)
{
    (void)fi; (void)cps; (void)count; (void)flags; (void)emit; (void)ctx;
}

static bool dummy_symbol(uint32_t cp)
{
    (void)cp;
    return false;
}

/* A two-row terminal. Row 0 never changes. Row 1 shows a new glyph in
 * every frame, so the glyphs outnumber the atlas cells. */
static uint32_t churn_cp = 0x100;
static uint64_t churn_gen = 1;
static struct wsap_vtcore_cell churn_rows[2][2];

static uint32_t churn_color(uint32_t cp)
{
    return 0xFF000000u | ((cp * 2654435761u) & 0x00FFFFFFu) | 0x00010101u;
}

static const struct wsap_vtcore_cell *churn_row(const struct terminal_frame *f, int row)
{
    (void)f;
    churn_rows[0][0] = (struct wsap_vtcore_cell){ .text = 'A', .fg = 0xFFFFFFFFu, .width = 1 };
    churn_rows[1][0] = (struct wsap_vtcore_cell){ .text = churn_cp, .fg = 0xFFFFFFFFu, .width = 1 };
    return churn_rows[row];
}

static uint64_t churn_row_gen(const struct terminal_frame *f, int row)
{
    (void)f;
    return row == 0 ? 1 : churn_gen;
}

static const struct glyph *churn_glyph(struct font_instance *fi, uint32_t cp, uint16_t flags)
{
    (void)fi; (void)flags;
    static uint32_t color[40];
    static struct glyph glyph = { .width = 2, .color = color, .has_color = true };
    for (int i = 0; i < 40; i++) color[i] = churn_color(cp);
    return &glyph;
}

static void test_atlas_reuse_keeps_cached_rows(id<MTLDevice> device)
{
    struct framebuffer_history history = {0};
    struct metal_render *r = metal_render_create(device);
    assert(r);
    static const int snapshot_tag;
    struct terminal_frame frame = {
        .source = (const struct wsap_vtcore_snapshot *)&snapshot_tag,
        .cols = 2, .rows = 2,
        .default_bg = 0xFF000000u, .default_fg = 0xFFFFFFFFu,
        .cursor_y = -1,
    };
    struct render_backend_view view = {
        .cols = 2, .rows = 2, .cell_w = 10, .cell_h = 20,
        .row = churn_row,
        .row_selection = dummy_selection,
        .resolve_cell = dummy_resolve_cell,
        .row_gen = churn_row_gen,
        .font_version = dummy_font_gen,
        .fetch_glyph = churn_glyph,
        .shape_segment = dummy_shape,
        .cell_aligned_symbol = dummy_symbol,
    };
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:20 height:40 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    uint32_t pixels[20 * 40];
    for (int i = 0; i < 9000; i++, churn_cp++, churn_gen++) {
        metal_render_begin(r);
        assert(metal_render_terminal(r, &history, &frame, &view) == RENDER_BACKEND_DREW);
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 40 }, false));
        if (i % 97 && i < 8990)
            continue;
        [target getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
              fromRegion:MTLRegionMake2D(0, 0, 20, 40) mipmapLevel:0];
        assert(pixels[0] == churn_color('A'));
        assert(pixels[20 * 20] == churn_color(churn_cp));
    }
    [target release];
    metal_render_destroy(r);
    framebuffer_history_release(&history);
}

static const int output_tags[2];

static const struct wsap_vtcore_cell *output_row(const struct terminal_frame *frame, int row)
{
    (void)row;
    static const struct wsap_vtcore_cell cells[2][2] = {
        {{ .text = 'A', .fg = 0xFFFFFFFFu, .width = 1 }},
        {{ .text = 'B', .fg = 0xFFFFFFFFu, .width = 1 }},
    };
    return cells[frame->source == (const struct wsap_vtcore_snapshot *)&output_tags[1]];
}

static void test_alternating_outputs(id<MTLDevice> device)
{
    struct metal_render *r = metal_render_create(device);
    assert(r);
    struct framebuffer_history histories[2] = {0};
    struct render_backend_view view = {
        .cols = 2, .rows = 1, .cell_w = 10, .cell_h = 20,
        .row = output_row, .row_selection = dummy_selection,
        .resolve_cell = dummy_resolve_cell, .row_gen = dummy_row_gen,
        .font_version = dummy_font_gen, .fetch_glyph = churn_glyph,
        .shape_segment = dummy_shape, .cell_aligned_symbol = dummy_symbol,
    };
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:20 height:20 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    assert(target);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    for (int i = 0; i < 6; i++) {
        int output = i % 2;
        struct terminal_frame frame = {
            .source = (const struct wsap_vtcore_snapshot *)&output_tags[output],
            .cols = 2, .rows = 1, .cursor_y = -1,
            .default_bg = 0xFF000000u, .default_fg = 0xFFFFFFFFu,
        };
        metal_render_begin(r);
        assert(metal_render_terminal(r, &histories[output], &frame, &view) ==
               RENDER_BACKEND_DREW);
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
        uint32_t pixels[400];
        [target getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
              fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
        assert(pixels[0] == churn_color(output ? 'B' : 'A'));
    }
    [target release];
    framebuffer_history_release(&histories[0]);
    framebuffer_history_release(&histories[1]);
    metal_render_destroy(r);
}

/* Mono glyphs whose ink width depends on the code point, the style, and
 * the font generation. Columns before the ink width have full coverage,
 * the column at the ink width has half coverage. */
static uint64_t mono_font_gen = 1;

static uint64_t mono_font_generation(const struct font_instance *fi)
{
    (void)fi;
    return mono_font_gen;
}

static int mono_ink(uint32_t cp, uint16_t flags)
{
    if (cp != 'A')
        return 7;
    return (flags & FONT_STYLE_BOLD) ? 5 : 2 + (int)mono_font_gen;
}

static const struct glyph *mono_glyph(struct font_instance *fi, uint32_t cp, uint16_t flags)
{
    (void)fi;
    static uint8_t pixels[20][10];
    static uint8_t *rows[20];
    static struct glyph glyph = { .width = 10, .alpha = rows };
    int ink = mono_ink(cp, flags);
    for (int y = 0; y < 20; y++) {
        rows[y] = pixels[y];
        for (int x = 0; x < 10; x++)
            pixels[y][x] = x < ink ? 255 : x == ink ? 128 : 0;
    }
    return &glyph;
}

static const struct wsap_vtcore_cell MONO_ROW[] = {
    { .text = 'A', .fg = 0xFFFFFFFFu, .width = 1 },
    { .text = 'A', .fg = 0xFFFFFFFFu, .attrs = WSAP_VTCORE_ATTR_BOLD, .width = 1 },
    { .text = 0xE9, .fg = 0xFFFFFFFFu, .width = 1 },
};

static const struct wsap_vtcore_cell *mono_row(const struct terminal_frame *f, int row)
{
    (void)f; (void)row;
    return MONO_ROW;
}

/* Checks each cell of the middle pixel row: ink[i] full pixels, then one
 * pixel near half intensity, then the background. */
static void check_mono_row(const uint32_t *pixels, const int ink[3])
{
    for (int c = 0; c < 3; c++) {
        const uint32_t *px = pixels + 10 * 30 + c * 10;
        for (int x = 0; x < 10; x++) {
            uint32_t blue = px[x] & 0xFFu;
            if (x < ink[c])
                assert(px[x] == 0xFFFFFFFFu);
            else if (x == ink[c])
                assert(blue >= 0x78 && blue <= 0x88);
            else
                assert(px[x] == 0xFF000000u);
        }
    }
}

static void test_ascii_mono_glyphs(id<MTLDevice> device)
{
    struct framebuffer_history history = {0};
    struct metal_render *r = metal_render_create(device);
    assert(r);
    struct terminal_frame frame = {
        .cols = 3, .rows = 1,
        .default_bg = 0xFF000000u, .default_fg = 0xFFFFFFFFu,
        .cursor_y = -1,
    };
    struct render_backend_view view = {
        .cols = 3, .rows = 1, .cell_w = 10, .cell_h = 20,
        .row = mono_row,
        .row_selection = dummy_selection,
        .resolve_cell = dummy_resolve_cell,
        .row_gen = dummy_row_gen,
        .font_version = mono_font_generation,
        .fetch_glyph = mono_glyph,
        .shape_segment = dummy_shape,
        .cell_aligned_symbol = dummy_symbol,
    };
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:30 height:20 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    uint32_t pixels[30 * 20];
    for (int i = 0; i < 6; i++) {
        /* The font changes after the third frame, so the glyphs change. */
        if (i == 3)
            mono_font_gen++;
        metal_render_begin(r);
        assert(metal_render_terminal(r, &history, &frame, &view) == RENDER_BACKEND_DREW);
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 30, .y = 20 }, false));
        [target getBytes:pixels bytesPerRow:30 * sizeof(uint32_t)
              fromRegion:MTLRegionMake2D(0, 0, 30, 20) mipmapLevel:0];
        int ink[3] = { mono_ink('A', 0), mono_ink('A', FONT_STYLE_BOLD), mono_ink(0xE9, 0) };
        check_mono_row(pixels, ink);
    }
    [target release];
    metal_render_destroy(r);
    framebuffer_history_release(&history);
}

static double bench_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Times the Metal path through a flood, the way the app draws: one
 * snapshot, about 8 KiB of new output per frame. The time is the scan of
 * the terminal on the main thread. The submit, which also waits for a
 * free frame buffer, is reported apart. */
static void metal_flood_benchmark(id<MTLDevice> device, enum flood_kind kind, const char *name)
{
    int cols = 160, rows = 50;
    int frames = getenv("FLOOD_FRAMES") ? atoi(getenv("FLOOD_FRAMES")) : 200;
    struct font_instance *fi = font_instance_default();
    const struct font_metrics *m = font_instance_metrics(fi);
    struct framebuffer_history history = {0};
    struct metal_render *r = metal_render_create(device);
    struct wsap_vtcore *t = tt_new(cols, rows);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(r && t && snap);
    tt_puts(t, "\033[?25l");
    int w = cols * m->cell_w, h = rows * m->cell_h;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    const struct render_backend_view view = {
        .font = fi, .cols = cols, .rows = rows,
        .cell_w = (float)m->cell_w, .cell_h = (float)m->cell_h,
        .cell_size = sizeof(struct wsap_vtcore_cell),
        .row = terminal_frame_row,
        .row_selection = terminal_frame_row_selection,
        .resolve_cell = render_resolve_cell,
        .row_gen = terminal_frame_row_key,
        .font_version = font_instance_version,
        .fetch_glyph = glyph_cache_fetch,
        .shape_segment = glyph_cache_shape_segment,
        .cell_aligned_symbol = is_cell_aligned_symbol,
    };
    uint32_t seed = 0x9e3779b9u;
    static char chunk[8192];
    double total = 0, worst = 0, submit = 0;
    for (int f = 0; f < frames + 10; f++) {
        @autoreleasepool {
            tt_write(t, chunk, flood_chunk(kind, &seed, chunk, sizeof(chunk)));
            assert(wsap_vtcore_snapshot_update(snap, t));
            struct terminal_frame frame;
            terminal_frame_init(&frame, snap, false);
            glyph_cache_begin_frame(fi, frame.default_fg, frame.default_bg);
            metal_render_begin(r);
            double begin = bench_now();
            assert(metal_render_terminal(r, &history, &frame, &view) == RENDER_BACKEND_DREW);
            double dt = bench_now() - begin;
            begin = bench_now();
            assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = w, .y = h },
                                       false));
            if (f >= 10) {
                total += dt;
                submit += bench_now() - begin;
                if (dt > worst) worst = dt;
            }
        }
    }
    printf("metal-flood-%-10s %.3f ms/frame worst=%.3f ms submit=%.3f ms/frame\n", name,
           total * 1000 / frames, worst * 1000, submit * 1000 / frames);
    [target release];
    wsap_vtcore_snapshot_free(snap);
    wsap_vtcore_free(t);
    metal_render_destroy(r);
    framebuffer_history_release(&history);
}

static int metal_benchmark(id<MTLDevice> device)
{
    assert(render_init());
    const char *only = getenv("FLOOD");
    for (unsigned i = 0; i < sizeof(flood_kinds) / sizeof(flood_kinds[0]); i++)
        if (!only || !strcmp(only, flood_kinds[i].name))
            metal_flood_benchmark(device, flood_kinds[i].kind, flood_kinds[i].name);
    render_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            printf("SKIPPED: No Metal device available\n");
            return 77;
        }
        if (argc > 1 && !strcmp(argv[1], "--bench"))
            return metal_benchmark(device);

        test_atlas_reuse_keeps_cached_rows(device);
        test_ascii_mono_glyphs(device);
        test_alternating_outputs(device);

        struct framebuffer_history history = {0};
        struct metal_render *r = metal_render_create(device);
        assert(r);

        struct terminal_frame frame = {
            .cols = 2,
            .rows = 1,
            .default_bg = 0xFF000000u,
            .default_fg = 0xFFFFFFFFu,
        };

        struct render_backend_view view = {
            .font = NULL,
            .cols = 2,
            .rows = 1,
            .cell_w = 10,
            .cell_h = 20,
            .row = dummy_row,
            .row_selection = dummy_selection,
            .resolve_cell = dummy_resolve_cell,
            .row_gen = dummy_row_gen,
            .font_version = dummy_font_gen,
            .fetch_glyph = dummy_glyph,
            .shape_segment = dummy_shape,
            .cell_aligned_symbol = dummy_symbol,
        };

        enum render_backend_result res = metal_render_terminal(r, &history, &frame, &view);
        assert(res == RENDER_BACKEND_DREW);

        struct ui_scene scene = {0};
        ui_scene_init(&scene);
        struct rect2i clip_box = (struct rect2i){ .pos = { .x = 0, .y = 0 }, .size = { .x = 20, .y = 20 } };
        assert(ui_scene_push_clip(&scene, clip_box));
        struct rect2i rbox = (struct rect2i){ .pos = { .x = 2, .y = 2 }, .size = { .x = 16, .y = 16 } };
        assert(ui_scene_rect(&scene, rbox, 0xFFFF0000u));
        assert(ui_scene_pop_clip(&scene));

        assert(metal_render_scene(r, &scene, NULL));
        ui_scene_destroy(&scene);

        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:20
                                                              height:20
                                                           mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> target_tex = [device newTextureWithDescriptor:td];
        assert(target_tex);

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target_tex;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
        uint32_t pixels[400];
        [target_tex getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
            fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
        assert(pixels[0] == 0xFFF01020u);
        assert(pixels[2] == 0xFF112233u);
        assert(pixels[3 * 20 + 3] == 0xFFFF0000u);

        /* A negative clip must keep its original right edge. */
        ui_scene_init(&scene);
        metal_render_begin(r);
        metal_render_clear(r, 0xFF000000u);
        struct rect2i negative = { .x = -5, .y = -5, .width = 10, .height = 10 };
        assert(ui_scene_push_clip(&scene, negative));
        assert(ui_scene_rect(&scene, clip_box, 0xFF00FF00u));
        assert(ui_scene_pop_clip(&scene));
        assert(metal_render_scene(r, &scene, NULL));
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
        [target_tex getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
            fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
        assert(pixels[4 * 20 + 4] == 0xFF00FF00u);
        assert(pixels[5 * 20 + 5] == 0xFF000000u);

        /* Images use XRGB pixels and ignore the source alpha byte. */
        uint32_t image_pixels[] = {0x00FF0000u, 0x000000FFu};
        struct ui_image image = { .pixels = image_pixels, .size = { .x = 2, .y = 1 } };
        ui_scene_init(&scene);
        metal_render_begin(r);
        metal_render_clear(r, 0xFF000000u);
        assert(ui_scene_image(&scene, (struct vector2i){0}, &image));
        assert(metal_render_scene(r, &scene, NULL));
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
        [target_tex getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
            fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
        assert(pixels[0] == 0xFFFF0000u);
        assert(pixels[1] == 0xFF0000FFu);
        /* Nested clips must restore each parent clip. */
        ui_scene_init(&scene);
        metal_render_begin(r);
        metal_render_clear(r, 0xFF000000u);
        for (int i = 0; i < 40; i++)
            assert(ui_scene_push_clip(&scene, i == 39 ? rbox : clip_box));
        assert(ui_scene_pop_clip(&scene));
        assert(ui_scene_blend_rect(&scene, clip_box, 0xFFFF0000u, 128));
        for (int i = 0; i < 39; i++) assert(ui_scene_pop_clip(&scene));
        assert(metal_render_scene(r, &scene, NULL));
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
        [target_tex getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
            fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
        assert(pixels[0] == 0xFF800000u);
        ui_scene_destroy(&scene);

        /* Repeated frames must preserve the glyph atlas and draw order. */
        for (int i = 0; i < 12; i++) {
            assert(metal_render_terminal(r, &history, &frame, &view) == RENDER_BACKEND_DREW);
            assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
            [target_tex getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
                fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
            assert(pixels[0] == 0xFFF01020u);
            assert(pixels[2] == 0xFF112233u);
        }
        ui_scene_destroy(&scene);

        metal_render_destroy(r);
        framebuffer_history_release(&history);
        r = metal_render_create(device);
        assert(r);
        assert(render_init());
        struct font_instance *font = font_instance_default();
        const struct font_metrics *metrics = font_instance_metrics(font);
        ui_scene_init(&scene);
        assert(ui_scene_text_run(&scene, clip_box,
            (struct ui_text_style){ .advance = metrics->cell_w, .cell_h = metrics->cell_h },
            0xFFFFFFFFu, "H", 1));
        metal_render_clear(r, 0xFF000000u);
        assert(metal_render_scene(r, &scene, font));
        assert(metal_render_submit(r, nil, pass, (struct vector2i){ .x = 20, .y = 20 }, false));
        [target_tex getBytes:pixels bytesPerRow:20 * sizeof(uint32_t)
            fromRegion:MTLRegionMake2D(0, 0, 20, 20) mipmapLevel:0];
        bool ink = false;
        for (int i = 0; i < 400; i++) ink |= (pixels[i] & 0x00FFFFFFu) != 0;
        assert(ink);
        ui_scene_destroy(&scene);
        metal_render_destroy(r);
        framebuffer_history_release(&history);
        render_shutdown();
        [target_tex release];
        printf("PASS: Metal renderer and scene test\n");
        return 0;
    }
}
