#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helpers/render_frame_helpers.h"
#include "paint/framebuffer.h"
#include "paint/scene.h"
#include "render/gpu/build.h"
#include "render/gpu/gpu.h"
#include "render/render.h"
#include "render/surface.h"
#include "util/damage.h"

/*
 * Tests of the Metal backend through render/gpu/gpu.h.
 *
 * The frames draw into offscreen textures, so the tests need a Metal device
 * and no display. Without a device, a test prints a note and does nothing.
 * A test reads the pixels after render_gpu_free(), because that call waits
 * until the GPU finishes every frame.
 *
 * Run the tests with the Metal validation layer to check the Metal calls
 * too: MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1.
 */

/* A drawable with a texture that the test can read. */
@interface TestDrawable : NSObject <CAMetalDrawable>
- (instancetype)initWithDevice:(id<MTLDevice>)device size:(struct vector2i)size;
@end

@implementation TestDrawable {
    id<MTLTexture> _texture;
    CAMetalLayer *_layer;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device size:(struct vector2i)size
{
    self = [super init];
    if (!self)
        return nil;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)size.x
                                                          height:(NSUInteger)size.y
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    _texture = [device newTextureWithDescriptor:d];
    _layer = [[CAMetalLayer alloc] init];
    _layer.device = device;
    if (!_texture) {
        [self release];
        return nil;
    }
    return self;
}

- (void)dealloc
{
    [_texture release];
    [_layer release];
    [super dealloc];
}

- (id<MTLTexture>)texture { return _texture; }
- (CAMetalLayer *)layer { return _layer; }
- (NSUInteger)drawableID { return 1; }
- (CFTimeInterval)presentedTime { return 0; }
- (void)present {}
- (void)presentAtTime:(CFTimeInterval)time
{
    (void)time;
}
- (void)presentAfterMinimumDuration:(CFTimeInterval)duration
{
    (void)duration;
}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block
{
    (void)block;
}

/* Metal asks for this handler when a frame presents the drawable. The test
 * holds no presentation, so the handler runs at once. */
- (void)addPresentScheduledHandler:(MTLDrawablePresentedHandler)block
{
    if (block)
        block(self);
}

@end

enum { SHOTS_MAX = 8 };

/* One GPU, one render surface, and the drawables that the test reads. */
struct fixture {
    id<MTLDevice> device;
    struct render_gpu *g;
    struct render_surface rs;
    TestDrawable *shots[SHOTS_MAX];
    int n_shots;
};

static bool fixture_open(struct fixture *f, const char *title)
{
    *f = (struct fixture){0};
    f->g = render_gpu_new();
    if (!f->g) {
        printf("metal_render_test: no Metal device, %s skipped\n", title);
        return false;
    }
    f->device = [(id<MTLDevice>)render_gpu_native_device(f->g) retain];
    return true;
}

/* A new drawable of `size`. The fixture owns it. */
static TestDrawable *fixture_shot(struct fixture *f, struct vector2i size)
{
    assert(f->n_shots < SHOTS_MAX);
    TestDrawable *d = [[TestDrawable alloc] initWithDevice:f->device size:size];
    assert(d);
    f->shots[f->n_shots++] = d;
    return d;
}

/* Frees the GPU. After this, every frame is finished and its pixels can be read. */
static void fixture_finish(struct fixture *f)
{
    render_gpu_surface_free(f->rs.gpu);
    f->rs.gpu = NULL;
    render_gpu_free(f->g);
    f->g = NULL;
}

static void fixture_close(struct fixture *f)
{
    fixture_finish(f);
    for (int i = 0; i < f->n_shots; i++)
        [f->shots[i] release];
    [f->device release];
}

/* The pixels of a drawable, as 0xAARRGGBB. The caller frees them. */
static uint32_t *shot_pixels(TestDrawable *d)
{
    id<MTLTexture> t = d.texture;
    uint32_t *px = malloc(t.width * t.height * sizeof(*px));
    assert(px);
    [t getBytes:px
        bytesPerRow:t.width * sizeof(*px)
         fromRegion:MTLRegionMake2D(0, 0, t.width, t.height)
        mipmapLevel:0];
    return px;
}

struct acquire_call {
    TestDrawable *drawable;
    int count;
};

static void *acquire(void *ud)
{
    struct acquire_call *call = ud;
    call->count++;
    return call->drawable;
}

/*
 * Draws `frame`, or presents `pixels` when they are given, into `drawable`.
 * `drawable` nil means that the window has no drawable. A frame that finds
 * every slot in use gets more tries, with time for the GPU between them.
 */
static enum render_gpu_status draw_into(struct fixture *f, const struct render_frame *frame,
                                        uint32_t *pixels, const struct damage_list *damage,
                                        TestDrawable *drawable, struct vector2i size)
{
    struct acquire_call call = { .drawable = drawable };
    enum render_gpu_status status = RENDER_GPU_UNUSED;
    for (int attempt = 0; attempt < 2000; attempt++) {
        struct framebuffer fb = {
            .pixels = pixels,
            .size = size,
            .acquire = acquire,
            .acquire_ud = &call,
        };
        call.count = 0;
        if (pixels)
            status = render_gpu_present(f->g, &f->rs, &fb, damage) ? RENDER_GPU_PRESENTED
                                                                     : RENDER_GPU_DROPPED;
        else
            status = render_gpu_draw(f->g, &f->rs, &fb, frame);
        if (call.count || status != RENDER_GPU_DROPPED)
            return status;
        usleep(1000);
    }
    return status;
}

static enum render_gpu_status draw(struct fixture *f, const struct render_frame *frame,
                                   TestDrawable *drawable, struct vector2i size)
{
    return draw_into(f, frame, NULL, NULL, drawable, size);
}

/* The number of pixels that are neither `a` nor `b`. */
static size_t count_other(const uint32_t *px, size_t n, uint32_t a, uint32_t b)
{
    size_t count = 0;
    for (size_t i = 0; i < n; i++)
        count += px[i] != a && px[i] != b;
    return count;
}

/* The backend starts, gives its device, and refuses calls without a drawable. */
static void test_lifecycle(void)
{
    struct render_gpu *g = render_gpu_new();
    if (!g) {
        printf("metal_render_test: no Metal device, lifecycle skipped\n");
        return;
    }
    id<MTLDevice> device = render_gpu_native_device(g);
    assert(device && [device conformsToProtocol:@protocol(MTLDevice)]);

    struct render_surface surface = {0};
    struct framebuffer fb = { .size = { .x = 100, .y = 100 } };
    struct render_frame frame = {0};
    assert(render_gpu_draw(g, &surface, &fb, &frame) == RENDER_GPU_UNUSED);
    assert(!render_gpu_present(g, &surface, &fb, NULL));
    assert(render_gpu_native_device(NULL) == NULL);
    render_gpu_surface_free(NULL);
    render_gpu_free(NULL);
    render_gpu_free(g);
}

/* A terminal with styled text, decorations, wide glyphs, color glyphs, and
 * box drawing. */
static struct wsap_vtcore *styled_terminal(void)
{
    struct wsap_vtcore *term =
        wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 80, .y = 24 } });
    assert(term);
    const char *text =
        "\033[1;32mbold green \033[0;31mred \033[3mitalic\033[0m \033[7minverse\033[0m\r\n"
        "\033[4mul\033[0m \033[21mdouble\033[0m \033[4:3mcurly\033[0m \033[4:4mdotted\033[0m "
        "\033[4:5mdashed\033[0m \033[9mstrike\033[0m \033[53mover\033[0m\r\n"
        "\033[38;5;208m256 \033[48;2;40;80;160m truecolor \033[0m \033[2mdim\033[0m\r\n"
        "\xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x98\x80\xf0\x9f\x9a\x80 e\xcc\x81 "
        "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xe2\x96\x88\xe2\x96\x80\xe2\x96\x92 "
        "\xe2\xa0\x81\xe2\xa3\xbf\r\n"
        "prompt$ ";
    wsap_vtcore_feed(term, text, strlen(text), 0);
    return term;
}

/* One op of each kind, a text run, and a clip. */
static void scene_fill(struct ui_scene *scene, const struct ui_image *image)
{
    assert(ui_scene_rect(scene, (struct rect2i){ .x = 10, .y = 300, .width = 200, .height = 100 },
                         0xFF334455u));
    assert(ui_scene_blend_rect(scene,
                               (struct rect2i){ .x = 20, .y = 310, .width = 180, .height = 80 },
                               0xFFAABBCCu, 160));
    assert(ui_scene_rounded_rect(scene,
                                 (struct rect2i){ .x = 250, .y = 300, .width = 150, .height = 100 },
                                 12, 0xFF4488AAu));
    assert(ui_scene_circle(scene, (struct vector2i){ .x = 480, .y = 350 }, 40, 0xFF66BBAAu, 200));
    assert(ui_scene_shadow(scene,
                           (struct rect2i){ .x = 50, .y = 430, .width = 300, .height = 120 }, 8,
                           12, 0xAA000000u));
    assert(ui_scene_image(scene, (struct vector2i){ .x = 600, .y = 300 }, image));
    const struct font_metrics *m = font_instance_metrics(test_font());
    struct ui_text_style style = { .advance = m->cell_w, .cell_h = m->cell_h };
    assert(ui_scene_push_clip(scene,
                              (struct rect2i){ .x = 600, .y = 420, .width = 120, .height = 30 }));
    const char *text = "clipped UI text \xe4\xbd\xa0\xe5\xa5\xbd";
    const struct rect2i bounds = { .x = 580, .y = 425, .width = 300, .height = m->cell_h };
    assert(ui_scene_text_run(scene, bounds, style, 0xFFFFFF00u, text, strlen(text)));
    assert(ui_scene_pop_clip(scene));
}

/* Fills the terminal with `count` wide glyphs that no earlier call used. */
static void feed_new_glyphs(struct wsap_vtcore *term, uint32_t *next_cp, int count)
{
    wsap_vtcore_feed(term, "\033[H", 3, 0);
    for (int i = 0; i < count; i++) {
        uint32_t cp = (*next_cp)++;
        char utf8[3] = {
            (char)(0xE0 | (cp >> 12)),
            (char)(0x80 | ((cp >> 6) & 0x3F)),
            (char)(0x80 | (cp & 0x3F)),
        };
        wsap_vtcore_feed(term, utf8, sizeof(utf8), 0);
    }
}

/*
 * A frame shows the same pixels whatever the atlas held before: with new
 * glyphs, with known glyphs, and after a flood of new glyphs. The flood is
 * more than the atlas keeps, so the atlas grows and starts again.
 */
static void test_same_frame_same_pixels(void)
{
    struct fixture f;
    if (!fixture_open(&f, "same frame"))
        return;
    const struct vector2i size = { .x = 1000, .y = 640 };
    TestDrawable *cold = fixture_shot(&f, size);
    TestDrawable *warm = fixture_shot(&f, size);
    TestDrawable *after = fixture_shot(&f, size);
    TestDrawable *scratch = fixture_shot(&f, size);

    struct wsap_vtcore *term = styled_terminal();
    uint32_t image_pixels[32 * 32];
    for (int i = 0; i < 32 * 32; i++)
        image_pixels[i] = 0xFF00FF00u | (uint32_t)i;
    struct ui_image image = { .size = { .x = 32, .y = 32 }, .pixels = image_pixels, .gen = 1 };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    scene_fill(&scene, &image);

    struct terminal_frame grid;
    test_frame_init(&grid, term, true);
    struct render_frame rf = test_grid_frame(&grid, size.x, size.y);
    rf.grid.rect = (struct rect2i){ .x = 7, .y = 5, .width = size.x - 20, .height = size.y - 12 };
    rf.base = 0xFF181824u;
    rf.ui = &scene;
    assert(draw(&f, &rf, cold, size) == RENDER_GPU_PRESENTED);
    assert(draw(&f, &rf, warm, size) == RENDER_GPU_PRESENTED);

    struct wsap_vtcore *flood =
        wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 100, .y = 30 } });
    assert(flood);
    uint32_t next_cp = 0x4E00;
    for (int batch = 0; batch < 14; batch++) {
        feed_new_glyphs(flood, &next_cp, 50 * 30);
        struct terminal_frame flood_grid;
        test_frame_init(&flood_grid, flood, true);
        struct render_frame ff = test_grid_frame(&flood_grid, size.x, size.y);
        assert(draw(&f, &ff, scratch, size) == RENDER_GPU_PRESENTED);
    }

    /* The flood took the snapshot of the grid, so take it again. */
    test_frame_init(&grid, term, true);
    assert(draw(&f, &rf, after, size) == RENDER_GPU_PRESENTED);
    fixture_finish(&f);

    size_t n = (size_t)size.x * (size_t)size.y;
    uint32_t *a = shot_pixels(cold), *b = shot_pixels(warm), *c = shot_pixels(after);
    assert(a[0] == rf.base);
    /* The frame drew more than the base and the grid background. */
    uint32_t grid_bg = a[(size_t)(size.y - 10) * (size_t)size.x + 20];
    assert(grid_bg != rf.base);
    assert(count_other(a, n, rf.base, grid_bg) > 10000);
    assert(!memcmp(a, b, n * sizeof(*a)));
    assert(!memcmp(a, c, n * sizeof(*a)));
    free(a);
    free(b);
    free(c);

    ui_scene_destroy(&scene);
    wsap_vtcore_free(flood);
    wsap_vtcore_free(term);
    fixture_close(&f);
}

/* True when the drawable shows `src` at `pos` as an opaque copy. */
static bool shows_image(const uint32_t *px, int stride, struct vector2i pos, const uint32_t *src,
                        struct vector2i size)
{
    for (int y = 0; y < size.y; y++)
        for (int x = 0; x < size.x; x++)
            if (px[(size_t)(pos.y + y) * (size_t)stride + (size_t)(pos.x + x)] !=
                (src[y * size.x + x] | 0xFF000000u))
                return false;
    return true;
}

/*
 * An image draws as an opaque copy of its pixels. An image that changes in
 * place shows its new pixels. A frame with more images than the backend
 * keeps goes to the CPU, and a frame that fits works again after it.
 */
static void test_images(void)
{
    struct fixture f;
    if (!fixture_open(&f, "images"))
        return;
    const struct vector2i size = { .x = 320, .y = 200 };
    const struct vector2i pos = { .x = 40, .y = 30 };
    enum { SIDE = 24 };
    TestDrawable *first = fixture_shot(&f, size);
    TestDrawable *changed = fixture_shot(&f, size);

    /* Translucent source pixels: the copy is opaque. */
    uint32_t pixels[SIDE * SIDE], first_pixels[SIDE * SIDE];
    for (int i = 0; i < SIDE * SIDE; i++)
        pixels[i] = 0x80000000u | (uint32_t)(i * 2654435761u >> 8);
    memcpy(first_pixels, pixels, sizeof(pixels));
    struct ui_image image = { .size = { .x = SIDE, .y = SIDE }, .pixels = pixels, .gen = 1 };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, pos, &image));
    struct render_frame rf = { .ui = &scene, .ui_font = test_font(), .base = 0xFF102030u };
    assert(draw(&f, &rf, first, size) == RENDER_GPU_PRESENTED);
    for (int i = 0; i < SIDE * SIDE; i++)
        pixels[i] = 0xFF000000u | (uint32_t)(i * 40503u);
    image.gen++;
    assert(draw(&f, &rf, changed, size) == RENDER_GPU_PRESENTED);
    ui_scene_destroy(&scene);

    enum { IMAGES = GPU_IMAGES + 1 };
    static uint32_t many_pixels[IMAGES][8 * 8];
    struct ui_image many[IMAGES];
    for (int i = 0; i < IMAGES; i++) {
        for (int p = 0; p < 8 * 8; p++)
            many_pixels[i][p] = 0xFF000000u | (uint32_t)(i * 40);
        many[i] = (struct ui_image){
            .size = { .x = 8, .y = 8 }, .pixels = many_pixels[i], .gen = 1,
        };
    }
    TestDrawable *scratch = fixture_shot(&f, size);
    for (int n = IMAGES; n >= GPU_IMAGES; n--) {
        ui_scene_init(&scene);
        for (int i = 0; i < n; i++)
            assert(ui_scene_image(&scene, (struct vector2i){ .x = 10 + i * 12, .y = 10 },
                                  &many[i]));
        rf.ui = &scene;
        enum render_gpu_status want = n <= GPU_IMAGES ? RENDER_GPU_PRESENTED : RENDER_GPU_UNUSED;
        assert(draw(&f, &rf, scratch, size) == want);
        ui_scene_destroy(&scene);
    }
    fixture_finish(&f);

    uint32_t *a = shot_pixels(first), *b = shot_pixels(changed);
    assert(a[0] == rf.base && b[0] == rf.base);
    assert(shows_image(a, size.x, pos, first_pixels, image.size));
    assert(shows_image(b, size.x, pos, pixels, image.size));
    free(a);
    free(b);
    fixture_close(&f);
}

/*
 * Presents CPU pixels: first every pixel, then only a damaged part. The
 * drawable shows the pixels as they are.
 */
static void test_cpu_pixels(void)
{
    struct fixture f;
    if (!fixture_open(&f, "cpu pixels"))
        return;
    const struct vector2i size = { .x = 320, .y = 200 };
    size_t n = (size_t)size.x * (size_t)size.y;
    TestDrawable *whole = fixture_shot(&f, size);
    TestDrawable *part = fixture_shot(&f, size);
    uint32_t *pixels = malloc(n * sizeof(*pixels));
    uint32_t *first = malloc(n * sizeof(*first));
    assert(pixels && first);
    for (size_t i = 0; i < n; i++)
        pixels[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8);
    memcpy(first, pixels, n * sizeof(*pixels));
    assert(draw_into(&f, NULL, pixels, NULL, whole, size) == RENDER_GPU_PRESENTED);

    const struct rect2i r = { .x = 8, .y = 8, .width = 64, .height = 32 };
    for (int y = r.y; y < r.y + r.height; y++)
        for (int x = r.x; x < r.x + r.width; x++)
            pixels[(size_t)y * (size_t)size.x + (size_t)x] = 0xFFFF8000u;
    struct damage_list damage = { .any = true, .n = 1, .rects = { r }, .bounds = r };
    assert(draw_into(&f, NULL, pixels, &damage, part, size) == RENDER_GPU_PRESENTED);
    fixture_finish(&f);

    uint32_t *a = shot_pixels(whole), *b = shot_pixels(part);
    assert(!memcmp(a, first, n * sizeof(*a)));
    assert(!memcmp(b, pixels, n * sizeof(*b)));
    free(a);
    free(b);
    free(first);
    free(pixels);
    fixture_close(&f);
}

/*
 * A frame with a new image is dropped, because the window has no drawable.
 * Then the image goes away. The backend must not read its pixels later.
 * A sanitizer build shows a read of the freed pixels.
 */
static void test_dropped_frame_image(void)
{
    struct fixture f;
    if (!fixture_open(&f, "dropped frame"))
        return;
    const struct vector2i size = { .x = 320, .y = 200 };
    TestDrawable *drawable = fixture_shot(&f, size);
    struct wsap_vtcore *term = styled_terminal();
    struct terminal_frame grid;
    test_frame_init(&grid, term, true);
    struct render_frame rf = test_grid_frame(&grid, size.x, size.y);

    uint32_t *image_pixels = malloc(64 * 64 * sizeof(*image_pixels));
    assert(image_pixels);
    memset(image_pixels, 0x80, 64 * 64 * sizeof(*image_pixels));
    struct ui_image image = { .size = { .x = 64, .y = 64 }, .pixels = image_pixels, .gen = 3 };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 30, .y = 30 }, &image));
    rf.ui = &scene;
    assert(draw(&f, &rf, nil, size) == RENDER_GPU_DROPPED);

    free(image_pixels);
    rf.ui = NULL;
    for (int i = 0; i < 3; i++)
        assert(draw(&f, &rf, drawable, size) == RENDER_GPU_PRESENTED);

    ui_scene_destroy(&scene);
    wsap_vtcore_free(term);
    fixture_close(&f);
}

int main(void)
{
    /* Without the font system a frame has no grid and no UI. */
    bool fonts = render_init();
    assert(fonts && font_instance_default());
    test_lifecycle();
    test_same_frame_same_pixels();
    test_images();
    test_cpu_pixels();
    test_dropped_frame_image();
    test_frame_helpers_fini();
    render_shutdown();
    printf("metal_render_test passed\n");
    return 0;
}
