#include "ui/yasl/metal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "loop/latency_probe.h"
#include "paint/vulkan.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/stats.h"
#include "yasl/yasl.h"
#include "yasl/vulkan.h"

/*
 * Vulkan window surface attachment.
 * This file joins yasl native windows with the Vulkan renderer.
 */

struct window_vulkan {
    struct yasl_surface *surface;
    VkSurfaceKHR vk_surface;
    struct vulkan_drawable drawable;
    struct window_metal_watch watch;
    struct vector2i last_size;
    bool has_size;
};

static bool vulkan_loader_initialized;

void *window_metal_attach(struct yasl_surface *s, void *device)
{
    if (!s || !device)
        return NULL;
    const struct vulkan_device *vdev = device;
    if (!vdev->instance)
        return NULL;
    if (!vulkan_loader_initialized) {
        struct yasl_vulkan_requirements reqs = {0};
        if (yasl_vulkan_init(vkGetInstanceProcAddr, &reqs) != 0 && errno != EBUSY)
            return NULL;
        vulkan_loader_initialized = true;
    }
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    VkResult vr = yasl_vulkan_create_surface(s, vdev->instance, NULL, &vk_surface);
    if (vr != VK_SUCCESS)
        return NULL;
    struct window_vulkan *wv = calloc(1, sizeof(*wv));
    if (!wv) {
        yasl_vulkan_destroy_surface(s);
        return NULL;
    }
    wv->surface = s;
    wv->vk_surface = vk_surface;
    wv->drawable.surface = vk_surface;
    wv->drawable.userdata = wv;
    return wv;
}

void window_metal_detach(void *layer, struct yasl_surface *s)
{
    struct window_vulkan *wv = layer;
    if (s)
        yasl_vulkan_destroy_surface(s);
    free(wv);
}

static void *acquire_vulkan_drawable(void *ud)
{
    struct window_vulkan *wv = ud;
    if (!wv)
        return NULL;
    if (wv->surface) {
        struct window *w = (struct window *)yasl_surface_get_userdata(wv->surface);
        if (w && w->surface == wv->surface)
            wv->drawable.unpaced = w->unpaced;
    }
    return &wv->drawable;
}

bool window_metal_draw(void *layer, struct yasl_surface *s,
                       struct window_metal_watch watch,
                       window_metal_draw_fn draw, void *ud)
{
    struct window_vulkan *wv = layer;
    if (!wv || !s || !draw)
        return false;
    struct yasl_surface_info info = yasl_surface_get_info(s);
    struct vector2i size = { .x = info.pixel_width, .y = info.pixel_height };
    if (size.x <= 0 || size.y <= 0)
        return false;
    struct window *w = (struct window *)yasl_surface_get_userdata(s);
    if (w && w->surface == s)
        wv->drawable.unpaced = w->unpaced;
    /* A size change starts a resize transaction. */
    bool transaction = wv->has_size && !vector2i_eq(wv->last_size, size);
    wv->last_size = size;
    wv->has_size = true;
    wv->watch = watch;
    struct window_metal_frame frame = {
        .drawable = &wv->drawable,
        .size = size,
        .transaction = transaction,
        .acquire = acquire_vulkan_drawable,
        .acquire_ud = wv,
    };
    draw(&frame, ud);
    if (watch.presents)
        present_watch_note_presented(watch.presents);
    if (watch.sample) {
        uint64_t sample = latency_probe_frame_target();
        uint64_t now_ns = yasl_time_ns();
        uint32_t interval_ns = (uint32_t)yasl_surface_frame_interval_ns(s);
        latency_probe_presented(sample, now_ns, now_ns, interval_ns, LATENCY_PRESENTATION_VSYNC);
    }
    return true;
}
