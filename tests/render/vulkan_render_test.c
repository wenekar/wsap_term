#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "helpers/render_frame_helpers.h"
#include "paint/framebuffer.h"
#include "paint/scene.h"
#include "paint/vulkan.h"
#include "render/gpu/gpu.h"
#include "render/render.h"
#include "render/surface.h"
#include "render/term_frame.h"
#include "ui/yasl/metal.h"
#include "yasl/yasl.h"
#include "yasl/vulkan.h"
#include <wsap_vtcore/wsap_vtcore.h>

/*
 * Unit tests for the Vulkan rendering backend.
 * Verifies device initialization, synchronization features, swapchain presentation,
 * pipeline transitions, atlas shelf packing, dynamic resizing, and resource teardown.
 */

static void test_vulkan_lifecycle(void)
{
    struct render_gpu *g = render_gpu_new();
    if (!g) {
        printf("vulkan_render_test: Vulkan device not available or disabled, skipping\n");
        return;
    }

    void *dev_ptr = render_gpu_native_device(g);
    assert(dev_ptr != NULL);
    struct vulkan_device *vdev = dev_ptr;
    assert(vdev->instance != VK_NULL_HANDLE);
    assert(vdev->physical_device != VK_NULL_HANDLE);
    assert(vdev->device != VK_NULL_HANDLE);
    assert(vdev->queue != VK_NULL_HANDLE);

    /* Verify device synchronization2 feature. */
    VkPhysicalDeviceSynchronization2Features sync2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 feats = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &sync2,
    };
    vkGetPhysicalDeviceFeatures2(vdev->physical_device, &feats);
    assert(sync2.synchronization2 == VK_TRUE);

    /* Verify safe handling of empty or missing drawables. */
    struct render_surface surface = {0};
    struct framebuffer fb = {
        .size = { .x = 100, .y = 100 },
    };
    struct render_frame frame = {0};
    enum render_gpu_status status = render_gpu_draw(g, &surface, &fb, &frame);
    assert(status == RENDER_GPU_UNUSED);

    bool pres = render_gpu_present(g, &surface, &fb, NULL);
    assert(!pres);

    render_gpu_surface_free(NULL);
    render_gpu_free(g);
}

struct draw_test_context {
    struct render_gpu *g;
    struct render_surface *surface;
    const struct render_frame *frame;
    enum render_gpu_status result;
    bool unpaced;
};

static void test_metal_draw_cb(const struct window_metal_frame *f, void *ud)
{
    struct draw_test_context *ctx = ud;
    if (ctx->unpaced && f->drawable) {
        struct vulkan_drawable *vd = f->drawable;
        vd->unpaced = true;
    }
    struct framebuffer fb = {
        .size = f->size,
        .drawable = f->drawable,
        .transaction = f->transaction,
        .acquire = f->acquire,
        .acquire_ud = f->acquire_ud,
    };
    ctx->result = render_gpu_draw(ctx->g, ctx->surface, &fb, ctx->frame);
}

static void test_cpu_present_cb(const struct window_metal_frame *f, void *ud)
{
    struct draw_test_context *ctx = ud;
    size_t count = (size_t)f->size.x * (size_t)f->size.y;
    uint32_t *cpu_pixels = malloc(count * sizeof(uint32_t));
    assert(cpu_pixels != NULL);
    for (size_t i = 0; i < count; i++)
        cpu_pixels[i] = 0xFF223344u;
    struct framebuffer fb_cpu = {
        .size = f->size,
        .pixels = cpu_pixels,
        .drawable = f->drawable,
        .acquire = f->acquire,
        .acquire_ud = f->acquire_ud,
    };
    struct damage_list damage = {
        .n = 1,
        .rects = { { .pos = { .x = 0, .y = 0 }, .size = f->size } },
    };
    bool pres_ok = render_gpu_present(ctx->g, ctx->surface, &fb_cpu, &damage);
    assert(pres_ok);
    free(cpu_pixels);
}

static void test_vulkan_presentation(void)
{
    struct render_gpu *g = render_gpu_new();
    if (!g)
        return;

    void *dev_ptr = render_gpu_native_device(g);
    assert(dev_ptr != NULL);

    /* Initialize display backend. */
    if (yasl_init(&(struct yasl_options){.backend = YASL_BACKEND_AUTO}) < 0) {
        printf("vulkan_render_test: display session unavailable, skipping presentation test\n");
        render_gpu_free(g);
        return;
    }

    struct yasl_surface *ys = yasl_surface_create(&(struct yasl_surface_options){
        .title = "vulkan_render_test",
        .width = 640,
        .height = 480,
        .type = YASL_SURFACE_NATIVE,
    });
    if (!ys) {
        printf("vulkan_render_test: cannot create native surface, skipping presentation test\n");
        render_gpu_free(g);
        return;
    }

    void *layer = window_metal_attach(ys, dev_ptr);
    assert(layer != NULL);

    struct render_surface rs = {0};
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 80, .y = 25 } });
    assert(term != NULL);
    wsap_vtcore_feed(term, "\033[1;32mHello \033[1;31mVulkan \033[4;34mRendering!\033[0m\r\n", 48, 0);

    struct terminal_frame grid;
    test_frame_init(&grid, term, true);
    struct render_frame rf = test_grid_frame(&grid, 640, 480);
    rf.base = 0xFF181824u;

    /* Build a UI scene covering all GPU shapes and images. */
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect(&scene, (struct rect2i){ .pos = { .x = 10, .y = 10 }, .size = { .x = 200, .y = 100 } }, 0xFF334455u));
    assert(ui_scene_blend_rect(&scene, (struct rect2i){ .pos = { .x = 20, .y = 20 }, .size = { .x = 180, .y = 80 } }, 0xFFAABBCCu, 160));
    assert(ui_scene_rounded_rect(&scene, (struct rect2i){ .pos = { .x = 250, .y = 10 }, .size = { .x = 150, .y = 100 } }, 12, 0xFF4488AAu));
    assert(ui_scene_circle(&scene, (struct vector2i){ .x = 480, .y = 60 }, 40, 0xFF66BBAAu, 200));
    assert(ui_scene_shadow(&scene, (struct rect2i){ .pos = { .x = 50, .y = 150 }, .size = { .x = 300, .y = 120 } }, 8, 12, 0xAA000000u));

    uint32_t img_pixels[32 * 32];
    for (int i = 0; i < 32 * 32; i++)
        img_pixels[i] = 0xFF00FF00u | (uint32_t)i;
    struct ui_image test_img = {
        .size = { .x = 32, .y = 32 },
        .pixels = img_pixels,
        .gen = 1,
    };
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 400, .y = 160 }, &test_img));
    rf.ui = &scene;

    /* Draw multiple frames to cycle through frame slots. */
    for (int frame_idx = 0; frame_idx < 6; frame_idx++) {
        struct draw_test_context ctx = {
            .g = g,
            .surface = &rs,
            .frame = &rf,
            .result = RENDER_GPU_UNUSED,
        };
        bool drawn = window_metal_draw(layer, ys, (struct window_metal_watch){0},
                                       test_metal_draw_cb, &ctx);
        assert(drawn);
        assert(ctx.result == RENDER_GPU_PRESENTED);
    }

    /* Test unpaced mode (tearing/uncapped present mode). */
    for (int frame_idx = 0; frame_idx < 3; frame_idx++) {
        struct draw_test_context ctx = {
            .g = g,
            .surface = &rs,
            .frame = &rf,
            .result = RENDER_GPU_UNUSED,
            .unpaced = true,
        };
        bool drawn = window_metal_draw(layer, ys, (struct window_metal_watch){0},
                                       test_metal_draw_cb, &ctx);
        assert(drawn);
        assert(ctx.result == RENDER_GPU_PRESENTED);
    }

    /* Test swapchain resize. */
    yasl_surface_request_size(ys, 800, 600);
    struct yasl_event ev;
    while (yasl_wait_event(&ev, 20) > 0);
    struct yasl_surface_info resize_info = yasl_surface_get_info(ys);
    struct render_frame rf_resize = test_grid_frame(&grid, resize_info.pixel_width, resize_info.pixel_height);
    rf_resize.base = 0xFF181824u;
    for (int frame_idx = 0; frame_idx < 3; frame_idx++) {
        struct draw_test_context ctx = {
            .g = g,
            .surface = &rs,
            .frame = &rf_resize,
            .result = RENDER_GPU_UNUSED,
        };
        bool drawn = window_metal_draw(layer, ys, (struct window_metal_watch){0},
                                       test_metal_draw_cb, &ctx);
        assert(drawn);
        assert(ctx.result == RENDER_GPU_PRESENTED);
    }

    /* Test CPU pixel presentation via render_gpu_present. */
    struct draw_test_context cpu_ctx = {
        .g = g,
        .surface = &rs,
    };
    bool cpu_drawn = window_metal_draw(layer, ys, (struct window_metal_watch){0},
                                       test_cpu_present_cb, &cpu_ctx);
    assert(cpu_drawn);

    /* Teardown and verify resource destruction. */
    render_gpu_surface_free(rs.gpu);
    rs.gpu = NULL;
    window_metal_detach(layer, ys);
    yasl_surface_destroy(ys);

    ui_scene_destroy(&scene);
    wsap_vtcore_free(term);
    render_gpu_free(g);
}

/* Stress test atlas shelf packing and dynamic resizing under high glyph load. */
static void test_vulkan_atlas_packing(void)
{
    struct render_gpu *g = render_gpu_new();
    if (!g)
        return;

    void *dev_ptr = render_gpu_native_device(g);
    assert(dev_ptr != NULL);

    if (yasl_init(&(struct yasl_options){.backend = YASL_BACKEND_AUTO}) < 0) {
        render_gpu_free(g);
        return;
    }

    struct yasl_surface *ys = yasl_surface_create(&(struct yasl_surface_options){
        .title = "atlas_stress_test",
        .width = 640,
        .height = 480,
        .type = YASL_SURFACE_NATIVE,
    });
    if (!ys) {
        render_gpu_free(g);
        return;
    }

    void *layer = window_metal_attach(ys, dev_ptr);
    assert(layer != NULL);
    struct render_surface rs = {0};

    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){ .size = { .x = 100, .y = 30 } });
    assert(term != NULL);

    /* Churn thousands of distinct characters to force shelf allocation, texture doubling, and recycling. */
    for (int batch = 0; batch < 20; batch++) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf), "\033[HBatch %03d: ", batch);
        for (int c = 0; c < 80; c++) {
            uint32_t cp = 0x100 + (uint32_t)(batch * 80 + c);
            /* Append UTF-8 character. */
            buf[len++] = (char)(0xC0 | (cp >> 6));
            buf[len++] = (char)(0x80 | (cp & 0x3F));
        }
        buf[len++] = '\r';
        buf[len++] = '\n';
        wsap_vtcore_feed(term, buf, (size_t)len, 0);

        struct terminal_frame grid;
        test_frame_init(&grid, term, true);
        struct render_frame rf = test_grid_frame(&grid, 640, 480);
        rf.base = 0xFF101018u;

        struct draw_test_context ctx = {
            .g = g,
            .surface = &rs,
            .frame = &rf,
            .result = RENDER_GPU_UNUSED,
        };
        bool drawn = window_metal_draw(layer, ys, (struct window_metal_watch){0},
                                       test_metal_draw_cb, &ctx);
        assert(drawn);
        assert(ctx.result == RENDER_GPU_PRESENTED);
    }

    render_gpu_surface_free(rs.gpu);
    rs.gpu = NULL;
    window_metal_detach(layer, ys);
    yasl_surface_destroy(ys);
    wsap_vtcore_free(term);
    render_gpu_free(g);
}

int main(void)
{
    test_vulkan_lifecycle();
    test_vulkan_presentation();
    test_vulkan_atlas_packing();
    printf("vulkan_render_test passed\n");
    return 0;
}