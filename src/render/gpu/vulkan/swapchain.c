#include "render/gpu/vulkan/internal.h"

#include <stdlib.h>
#include <string.h>

#include "render/gpu/gpu.h"
#include "render/surface.h"

/* The state of one surface: its swapchain and the texture of its CPU pixels. */

/* Destroys swapchain semaphores, image views, and handle. */
static void chain_destroy(struct render_gpu *g, struct vulkan_swapchain *c)
{
    VkDevice device = g->dev.device;
    for (uint32_t i = 0; i < c->image_count; i++) {
        if (c->rendered[i])
            vkDestroySemaphore(device, c->rendered[i], NULL);
        if (c->views[i])
            vkDestroyImageView(device, c->views[i], NULL);
    }
    if (c->handle)
        vkDestroySwapchainKHR(device, c->handle, NULL);
    *c = (struct vulkan_swapchain){0};
}

struct gpu_surface *vulkan_surface_of(struct render_gpu *g, struct render_surface *s)
{
    struct gpu_surface *gs = s->gpu;
    if (gs && gs->gpu == g)
        return gs;
    if (gs && gs->gpu)
        vulkan_surface_release(gs);
    if (!gs && !(gs = calloc(1, sizeof(*gs))))
        return NULL;
    gs->gpu = g;
    gs->next = g->surfaces;
    g->surfaces = gs;
    s->gpu = gs;
    return gs;
}

void vulkan_surface_release(struct gpu_surface *gs)
{
    struct render_gpu *g = gs->gpu;
    if (!g)
        return;
    vulkan_wait_idle(g);
    chain_destroy(g, &gs->retired);
    chain_destroy(g, &gs->chain);
    vulkan_texture_destroy(g, &gs->pixels);
    for (struct gpu_surface **link = &g->surfaces; *link; link = &(*link)->next) {
        if (*link == gs) {
            *link = gs->next;
            break;
        }
    }
    *gs = (struct gpu_surface){0};
}

void render_gpu_surface_free(void *gpu_surface)
{
    struct gpu_surface *gs = gpu_surface;
    if (!gs)
        return;
    vulkan_surface_release(gs);
    free(gs);
}

/* Queries and caches surface formats and present modes supported by physical device. */
static bool traits_ensure(struct render_gpu *g, struct gpu_surface *gs, VkSurfaceKHR surface)
{
    if (gs->traits.surface == surface)
        return true;
    memset(&gs->traits, 0, sizeof(gs->traits));
    VkPhysicalDevice pd = g->dev.physical_device;

    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(pd, g->dev.queue_family, surface, &supported) !=
            VK_SUCCESS ||
        !supported)
        return false;

    /* Vulkan defines fewer present modes than this. VK_INCOMPLETE is not an error. */
    VkPresentModeKHR modes[16];
    uint32_t mode_count = 16;
    VkResult vr = vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &mode_count, modes);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        mode_count = 0;
    for (uint32_t i = 0; i < mode_count; i++) {
        gs->traits.immediate |= modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        gs->traits.mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
        gs->traits.fifo_relaxed |= modes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &format_count, NULL);
    VkSurfaceFormatKHR *formats = format_count ? calloc(format_count, sizeof(*formats)) : NULL;
    if (!formats)
        return false;
    vr = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &format_count, formats);
    bool ok = (vr == VK_SUCCESS || vr == VK_INCOMPLETE) && format_count;
    if (ok) {
        gs->traits.format = formats[0];
        for (uint32_t i = 0; i < format_count; i++) {
            if (formats[i].format == TEXEL_FORMAT) {
                gs->traits.format = formats[i];
                break;
            }
        }
        gs->traits.surface = surface;
    }
    free(formats);
    return ok;
}

/* Creates image views and rendered semaphores for all swapchain images. */
static bool chain_images(struct render_gpu *g, struct vulkan_swapchain *c, VkFormat format)
{
    VkDevice device = g->dev.device;
    uint32_t count = SWAPCHAIN_IMAGES_MAX;
    /* VK_INCOMPLETE: more images than the arrays hold. An image index could
     * then point outside the arrays, so the swapchain is not usable. */
    if (vkGetSwapchainImagesKHR(device, c->handle, &count, c->images) != VK_SUCCESS)
        return false;
    c->image_count = count;
    for (uint32_t i = 0; i < count; i++) {
        VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = c->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (vkCreateSemaphore(device, &semaphore, NULL, &c->rendered[i]) != VK_SUCCESS ||
            vkCreateImageView(device, &view, NULL, &c->views[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

static VkPresentModeKHR present_mode_of(const struct gpu_surface *gs, bool tearing)
{
    if (tearing && gs->traits.immediate)
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (gs->traits.mailbox)
        return VK_PRESENT_MODE_MAILBOX_KHR;
    if (tearing && gs->traits.fifo_relaxed)
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

/* Records requested surface dimensions and presentation flags for `gs`. */
static void record_request(struct gpu_surface *gs, struct vector2i size, bool tearing)
{
    gs->request_size = (struct vector2i){ .x = (int)gs->extent.width,
                                          .y = (int)gs->extent.height };
    gs->request_tearing = tearing;
    gs->recheck = !vector2i_eq(gs->request_size, size);
}

bool vulkan_swapchain_ensure(struct render_gpu *g, struct gpu_surface *gs, VkSurfaceKHR surface,
                             struct vector2i size, bool tearing, bool force)
{
    bool same_surface = gs->surface == surface;
    bool replace = force || gs->out_of_date || !gs->chain.handle || !same_surface;

    /* Most frames keep the surface, the size, and the pacing. Ask the driver
     * nothing for these frames. */
    if (!replace && !gs->recheck && vector2i_eq(gs->request_size, size) &&
        gs->request_tearing == tearing)
        return true;

    VkPhysicalDevice pd = g->dev.physical_device;
    VkSurfaceCapabilitiesKHR caps;
    if (!traits_ensure(g, gs, surface) ||
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps) != VK_SUCCESS)
        return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        /* The surface takes the size of the swapchain. */
        extent.width = (uint32_t)geom_clamp_i(size.x, (int)caps.minImageExtent.width,
                                              (int)caps.maxImageExtent.width);
        extent.height = (uint32_t)geom_clamp_i(size.y, (int)caps.minImageExtent.height,
                                               (int)caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height)
        return false;
    VkPresentModeKHR present_mode = present_mode_of(gs, tearing);

    if (!replace && gs->extent.width == extent.width && gs->extent.height == extent.height &&
        gs->present_mode == present_mode) {
        record_request(gs, size, tearing);
        return true;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
        image_count = caps.minImageCount;
    else if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR)
        image_count = caps.minImageCount > 3 ? caps.minImageCount : 3;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = gs->traits.format.format,
        .imageColorSpace = gs->traits.format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = same_surface ? gs->chain.handle : VK_NULL_HANDLE,
    };
    struct vulkan_swapchain next = {0};
    VkResult vr = vkCreateSwapchainKHR(g->dev.device, &info, NULL, &next.handle);

    /*
     * The old swapchain is retired now, also when the call failed. Frames
     * that are submitted can still use it, so it waits in `retired`. Only a
     * second replacement in a row must wait for the queue.
     */
    if (gs->chain.handle) {
        if (gs->retired.handle) {
            vulkan_wait_idle(g);
            chain_destroy(g, &gs->retired);
        }
        gs->retired = gs->chain;
        gs->chain = (struct vulkan_swapchain){0};
    }
    if (vr != VK_SUCCESS) {
        /* The surface can have changed. Query it again for the next frame. */
        memset(&gs->traits, 0, sizeof(gs->traits));
        return false;
    }
    if (!chain_images(g, &next, info.imageFormat) ||
        !vulkan_pipelines_ensure(g, info.imageFormat)) {
        /* No frame used the new swapchain. The next frame makes another. */
        chain_destroy(g, &next);
        return false;
    }
    gs->chain = next;
    gs->surface = surface;
    gs->format = info.imageFormat;
    gs->extent = extent;
    gs->present_mode = present_mode;
    gs->out_of_date = false;
    record_request(gs, size, tearing);
    return true;
}

bool vulkan_swapchain_acquire(struct render_gpu *g, struct gpu_surface *gs,
                              struct frame_slot *slot, VkSurfaceKHR surface,
                              struct vector2i size, bool tearing, uint64_t wait_ns,
                              uint32_t *image_index)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!gs->chain.handle)
            return false;
        VkResult vr = vkAcquireNextImageKHR(g->dev.device, gs->chain.handle, wait_ns,
                                            slot->acquired, VK_NULL_HANDLE, image_index);
        if (vr == VK_SUCCESS)
            return true;
        if (vr == VK_SUBOPTIMAL_KHR) {
            /* The image is acquired, but the swapchain no longer matches
             * the surface. Keep the image when the check keeps the swapchain. */
            VkSwapchainKHR before = gs->chain.handle;
            gs->recheck = true;
            if (attempt)
                return true;
            bool ensured = vulkan_swapchain_ensure(g, gs, surface, size, tearing, false);
            if (ensured && gs->chain.handle == before)
                return true;
            /* The image belongs to a retired swapchain. The semaphore holds
             * its signal, so the next acquire needs a new one. */
            if (!vulkan_slot_renew_semaphore(g, slot) || !ensured)
                return false;
        } else if (vr == VK_ERROR_OUT_OF_DATE_KHR && !attempt) {
            if (!vulkan_swapchain_ensure(g, gs, surface, size, tearing, true))
                return false;
        } else {
            /* VK_TIMEOUT and VK_NOT_READY: no image is free. The frame is dropped. */
            return false;
        }
    }
    return false;
}
