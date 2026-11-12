#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "helpers/render_frame_helpers.h"
#include "paint/framebuffer.h"
#include "paint/scene.h"
#include "paint/vulkan.h"
#include "render/gpu/build.h"
#include "render/gpu/gpu.h"
#include "render/render.h"
#include "render/surface.h"
#include "render/term_frame.h"
#include "ui/yasl/metal.h"
#include "util/damage.h"
#include "yasl/yasl.h"
#include <wsap_vtcore/wsap_vtcore.h>

/*
 * Tests of the Vulkan backend through render/gpu/gpu.h.
 *
 * The tests need a Vulkan device and a display. Without one of them, a test
 * prints a note and does nothing. The tests cannot read the pixels that the
 * GPU draws. They check the result of each call. Run them with the Khronos
 * validation layer to check the Vulkan calls too:
 * VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
 */

/* One window with a Vulkan surface, and the renderer state of that window. */
struct fixture {
    struct render_gpu *g;
    struct yasl_surface *ys;
    void *layer;
    struct render_surface rs;
};

/* main() opens the display one time, for all tests. */
static bool have_display;

static bool fixture_open(struct fixture *f, const char *title)
{
    *f = (struct fixture){0};
    f->g = render_gpu_new();
    if (!f->g) {
        printf("vulkan_render_test: no Vulkan device, %s skipped\n", title);
        return false;
    }
    if (!have_display ||
        !(f->ys = yasl_surface_create(&(struct yasl_surface_options){
              .title = title,
              .width = 640,
              .height = 480,
              .type = YASL_SURFACE_NATIVE,
          }))) {
        printf("vulkan_render_test: no display, %s skipped\n", title);
        render_gpu_free(f->g);
        return false;
    }
    f->layer = window_metal_attach(f->ys, render_gpu_native_device(f->g));
    assert(f->layer);
    return true;
}

static void fixture_close(struct fixture *f)
{
    render_gpu_surface_free(f->rs.gpu);
    window_metal_detach(f->layer, f->ys);
    yasl_surface_destroy(f->ys);
    render_gpu_free(f->g);
}

static void pump_events(int timeout_ms)
{
    struct yasl_event ev;
    while (yasl_wait_event(&ev, timeout_ms) > 0) {
    }
}

struct draw_call {
    struct fixture *f;
    const struct render_frame *frame;
    /* Present these CPU pixels and do not draw the frame. */
    uint32_t *pixels;
    const struct damage_list *damage;
    bool unpaced;
    /* The window has no drawable for the frame. */
    bool no_drawable;
    enum render_gpu_status status;
    bool presented;
    struct vector2i size;
};

static void *no_drawable(void *ud)
{
    (void)ud;
    return NULL;
}

static void draw_cb(const struct window_metal_frame *wf, void *ud)
{
    struct draw_call *call = ud;
    struct framebuffer fb = {
        .size = wf->size,
        .drawable = wf->drawable,
        .transaction = wf->transaction,
        .acquire = wf->acquire,
        .acquire_ud = wf->acquire_ud,
    };
    if (wf->drawable)
        ((struct vulkan_drawable *)wf->drawable)->unpaced = call->unpaced;
    if (call->no_drawable) {
        fb.drawable = NULL;
        fb.acquire = no_drawable;
    }
    call->size = wf->size;
    if (call->pixels) {
        fb.pixels = call->pixels;
        call->presented = render_gpu_present(call->f->g, &call->f->rs, &fb, call->damage);
    } else {
        call->status = render_gpu_draw(call->f->g, &call->f->rs, &fb, call->frame);
    }
}

/*
 * Draws the frame. A backend can drop a frame when the GPU is behind, so a
 * dropped frame gets more tries, with time for the GPU between them.
 */
static enum render_gpu_status draw(struct fixture *f, const struct render_frame *frame,
                                   bool unpaced)
{
    struct draw_call call = { .f = f, .frame = frame, .unpaced = unpaced };
    for (int attempt = 0; attempt < 200; attempt++) {
        bool drawn = window_metal_draw(f->layer, f->ys, (struct window_metal_watch){0}, draw_cb,
                                       &call);
        assert(drawn);
        if (call.status != RENDER_GPU_DROPPED)
            break;
        pump_events(5);
    }
    return call.status;
}

static struct vector2i surface_size(const struct fixture *f)
{
    struct yasl_surface_info info = yasl_surface_get_info(f->ys);
    return (struct vector2i){ .x = info.pixel_width, .y = info.pixel_height };
}

/* A terminal with some styled text, and its frame for the surface. */
static struct wsap_vtcore *text_terminal(struct terminal_frame *grid)
{
    struct wsap_vtcore *term =
        wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 80, .y = 25 } });
    assert(term);
    const char *text = "\033[1;32mHello \033[1;31mVulkan \033[4;34mRendering!\033[0m\r\n";
    wsap_vtcore_feed(term, text, strlen(text), 0);
    test_frame_init(grid, term, true);
    return term;
}

static struct render_frame frame_for(const struct fixture *f, const struct terminal_frame *grid)
{
    struct vector2i size = surface_size(f);
    struct render_frame frame = test_grid_frame(grid, size.x, size.y);
    frame.base = 0xFF181824u;
    return frame;
}

/* The backend starts, gives its device, and refuses calls without a drawable. */
static void test_lifecycle(void)
{
    struct render_gpu *g = render_gpu_new();
    if (!g) {
        printf("vulkan_render_test: no Vulkan device, lifecycle skipped\n");
        return;
    }
    struct vulkan_device *dev = render_gpu_native_device(g);
    assert(dev && dev->instance && dev->physical_device && dev->device && dev->queue);

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

/* A grid and every kind of UI op, drawn in each present mode and across resizes. */
static void test_presentation(void)
{
    struct fixture f;
    if (!fixture_open(&f, "presentation"))
        return;
    struct terminal_frame grid;
    struct wsap_vtcore *term = text_terminal(&grid);
    struct render_frame rf = frame_for(&f, &grid);

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect(&scene, (struct rect2i){ .x = 10, .y = 10, .width = 200, .height = 100 },
                         0xFF334455u));
    assert(ui_scene_blend_rect(&scene,
                               (struct rect2i){ .x = 20, .y = 20, .width = 180, .height = 80 },
                               0xFFAABBCCu, 160));
    assert(ui_scene_rounded_rect(&scene,
                                 (struct rect2i){ .x = 250, .y = 10, .width = 150, .height = 100 },
                                 12, 0xFF4488AAu));
    assert(ui_scene_circle(&scene, (struct vector2i){ .x = 480, .y = 60 }, 40, 0xFF66BBAAu, 200));
    assert(ui_scene_shadow(&scene,
                           (struct rect2i){ .x = 50, .y = 150, .width = 300, .height = 120 }, 8,
                           12, 0xAA000000u));
    uint32_t image_pixels[32 * 32];
    for (int i = 0; i < 32 * 32; i++)
        image_pixels[i] = 0xFF00FF00u | (uint32_t)i;
    struct ui_image image = { .size = { .x = 32, .y = 32 }, .pixels = image_pixels, .gen = 1 };
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 400, .y = 160 }, &image));
    rf.ui = &scene;

    /* More frames than frame slots. */
    for (int i = 0; i < 6; i++)
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);
    /* The present mode that does not wait for the display, and back. */
    for (int i = 0; i < 3; i++)
        assert(draw(&f, &rf, true) == RENDER_GPU_PRESENTED);
    for (int i = 0; i < 3; i++)
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);

    /* A new size, when the compositor permits one. */
    yasl_surface_request_size(f.ys, 800, 600);
    pump_events(20);
    rf = frame_for(&f, &grid);
    rf.ui = &scene;
    for (int i = 0; i < 3; i++)
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);
    /* A new size for each frame, as in a live resize. */
    for (int i = 0; i < 6; i++) {
        yasl_surface_request_size(f.ys, 640 + (i + 1) * 16, 480 + (i + 1) * 12);
        pump_events(5);
        rf = frame_for(&f, &grid);
        rf.ui = &scene;
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);
    }

    ui_scene_destroy(&scene);
    wsap_vtcore_free(term);
    fixture_close(&f);
}

/* Presents CPU pixels: the whole canvas, then only a damaged part. */
static void test_cpu_pixels(void)
{
    struct fixture f;
    if (!fixture_open(&f, "cpu pixels"))
        return;
    struct vector2i size = surface_size(&f);
    size_t count = (size_t)size.x * (size_t)size.y;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    assert(pixels);
    for (size_t i = 0; i < count; i++)
        pixels[i] = 0xFF223344u;

    struct damage_list part = {
        .any = true,
        .n = 1,
        .rects = { { .x = 8, .y = 8, .width = 64, .height = 32 } },
        .bounds = { .x = 8, .y = 8, .width = 64, .height = 32 },
    };
    const struct damage_list *damages[] = { NULL, &part, &part };
    for (size_t i = 0; i < sizeof(damages) / sizeof(damages[0]); i++) {
        struct draw_call call = { .f = &f, .pixels = pixels, .damage = damages[i] };
        for (int attempt = 0; attempt < 200 && !call.presented; attempt++) {
            assert(window_metal_draw(f.layer, f.ys, (struct window_metal_watch){0}, draw_cb,
                                     &call));
            assert(vector2i_eq(call.size, size));
            if (!call.presented)
                pump_events(5);
        }
        assert(call.presented);
    }
    free(pixels);
    fixture_close(&f);
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
 * Many new glyphs in each frame fill the atlas, so it must grow and then
 * start again. A UI image stays on screen all the time. Each frame must
 * still reach the GPU.
 */
static void test_atlas_growth(void)
{
    struct fixture f;
    if (!fixture_open(&f, "atlas growth"))
        return;
    struct wsap_vtcore *term =
        wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 100, .y = 30 } });
    assert(term);
    uint32_t image_pixels[16 * 16];
    for (int i = 0; i < 16 * 16; i++)
        image_pixels[i] = 0xFF8040C0u;
    struct ui_image image = { .size = { .x = 16, .y = 16 }, .pixels = image_pixels, .gen = 7 };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 4, .y = 4 }, &image));

    /* CJK ideographs: two cells wide, and thousands of them exist. */
    uint32_t next_cp = 0x4E00;
    for (int batch = 0; batch < 14; batch++) {
        feed_new_glyphs(term, &next_cp, 50 * 30);
        struct terminal_frame grid;
        test_frame_init(&grid, term, true);
        struct render_frame rf = frame_for(&f, &grid);
        rf.ui = &scene;
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);
    }
    ui_scene_destroy(&scene);
    wsap_vtcore_free(term);
    fixture_close(&f);
}

/*
 * The backend keeps a small number of UI images. A frame with more gives
 * the frame to the CPU, and it never draws one image in place of another.
 * A frame that fits works again after that.
 */
static void test_image_slots(void)
{
    struct fixture f;
    if (!fixture_open(&f, "image slots"))
        return;
    struct terminal_frame grid;
    struct wsap_vtcore *term = text_terminal(&grid);
    struct render_frame rf = frame_for(&f, &grid);

    enum { IMAGES = GPU_IMAGES + 1, SIDE = 8 };
    static uint32_t pixels[IMAGES][SIDE * SIDE];
    struct ui_image images[IMAGES];
    for (int i = 0; i < IMAGES; i++) {
        for (int p = 0; p < SIDE * SIDE; p++)
            pixels[i][p] = 0xFF000000u | (uint32_t)(i * 40);
        images[i] = (struct ui_image){
            .size = { .x = SIDE, .y = SIDE }, .pixels = pixels[i], .gen = 1,
        };
    }
    for (int n = 1; n <= IMAGES; n++) {
        struct ui_scene scene = {0};
        ui_scene_init(&scene);
        for (int i = 0; i < n; i++)
            assert(ui_scene_image(&scene, (struct vector2i){ .x = 10 + i * 12, .y = 10 },
                                  &images[i]));
        rf.ui = &scene;
        enum render_gpu_status want = n <= GPU_IMAGES ? RENDER_GPU_PRESENTED : RENDER_GPU_UNUSED;
        assert(draw(&f, &rf, false) == want);
        ui_scene_destroy(&scene);
    }

    /* An image that changes in place: the same buffer with a new generation. */
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 10, .y = 10 }, &images[0]));
    rf.ui = &scene;
    for (int i = 0; i < 4; i++) {
        pixels[0][0] = 0xFF000000u | (uint32_t)i;
        images[0].gen++;
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);
    }
    ui_scene_destroy(&scene);
    wsap_vtcore_free(term);
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
    struct terminal_frame grid;
    struct wsap_vtcore *term = text_terminal(&grid);
    struct render_frame rf = frame_for(&f, &grid);

    uint32_t *image_pixels = malloc(64 * 64 * sizeof(*image_pixels));
    assert(image_pixels);
    memset(image_pixels, 0x80, 64 * 64 * sizeof(*image_pixels));
    struct ui_image image = { .size = { .x = 64, .y = 64 }, .pixels = image_pixels, .gen = 3 };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 30, .y = 30 }, &image));
    rf.ui = &scene;

    struct draw_call dropped = { .f = &f, .frame = &rf, .no_drawable = true };
    assert(window_metal_draw(f.layer, f.ys, (struct window_metal_watch){0}, draw_cb, &dropped));
    assert(dropped.status == RENDER_GPU_DROPPED);

    free(image_pixels);
    rf.ui = NULL;
    for (int i = 0; i < 3; i++)
        assert(draw(&f, &rf, false) == RENDER_GPU_PRESENTED);

    ui_scene_destroy(&scene);
    wsap_vtcore_free(term);
    fixture_close(&f);
}

int main(void)
{
    /* Without the font system a frame has no grid and no UI. */
    bool fonts = render_init();
    assert(fonts && font_instance_default());
    have_display = yasl_init(&(struct yasl_options){ .backend = YASL_BACKEND_AUTO }) >= 0;
    test_lifecycle();
    test_presentation();
    test_cpu_pixels();
    test_atlas_growth();
    test_image_slots();
    test_dropped_frame_image();
    if (have_display)
        yasl_shutdown();
    render_shutdown();
    printf("vulkan_render_test passed\n");
    return 0;
}
