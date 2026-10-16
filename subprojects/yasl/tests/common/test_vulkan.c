/* Public Vulkan WSI integration test. The launcher supplies a real Wayland
 * or X11 session (normally Weston/Xwayland); no internal YASL headers are
 * used here. */
#include "yasl/yasl.h"
#include "yasl/vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_vk(const char *what, VkResult result)
{
    fprintf(stderr, "vulkan integration: %s failed (VkResult %d)\n", what, result);
    return 1;
}

static bool has_device_extension(VkPhysicalDevice device, const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS)
        return false;
    VkExtensionProperties *extensions = count ? calloc(count, sizeof(*extensions)) : NULL;
    if (count && !extensions) return false;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; i++)
            if (strcmp(extensions[i].extensionName, name) == 0) found = true;
    free(extensions);
    return found;
}

static int run_test(enum yasl_backend backend)
{
    int status = 1;
    yasl_surface *surface = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;

    if (yasl_init(&(struct yasl_options){.backend = backend}) < 0) {
        fprintf(stderr, "vulkan integration: cannot select requested backend %d (YASL error %d)\n",
                backend, errno);
        goto cleanup;
    }
    struct yasl_vulkan_requirements requirements;
    if (yasl_vulkan_init(vkGetInstanceProcAddr, &requirements) < 0) {
        fprintf(stderr, "vulkan integration: loader initialization failed (YASL error %d)\n",
                errno);
        goto cleanup;
    }
    surface = yasl_surface_create(&(struct yasl_surface_options){.title="yasl Vulkan WSI integration", .width=256, .height=192, .type=YASL_SURFACE_NATIVE});
    if (!surface) {
        fprintf(stderr, "vulkan integration: surface creation failed (YASL error %d)\n",
                errno);
        goto cleanup;
    }
    if (!(yasl_get_features() & YASL_FEATURE_VULKAN)) {
        fprintf(stderr, "vulkan integration: requested backend does not advertise Vulkan WSI\n");
        goto cleanup;
    }

    uint32_t extension_count = 0;
    extension_count = requirements.extension_count;
    const char *const *required = requirements.extensions;
    if (!required || !extension_count) {
        fprintf(stderr, "vulkan integration: surface returned no required instance extensions\n");
        goto cleanup;
    }
    uint32_t available_count = 0;
    VkResult vr = vkEnumerateInstanceExtensionProperties(NULL, &available_count, NULL);
    if (vr != VK_SUCCESS) { status = fail_vk("vkEnumerateInstanceExtensionProperties", vr); goto cleanup; }
    VkExtensionProperties *available = available_count ? calloc(available_count, sizeof(*available)) : NULL;
    if (available_count && !available) { fprintf(stderr, "vulkan integration: out of memory\n"); goto cleanup; }
    vr = vkEnumerateInstanceExtensionProperties(NULL, &available_count, available);
    if (vr != VK_SUCCESS) {
        free(available); status = fail_vk("vkEnumerateInstanceExtensionProperties", vr); goto cleanup;
    }
    for (uint32_t i = 0; i < extension_count; i++) {
        bool found = false;
        for (uint32_t j = 0; j < available_count; j++)
            if (strcmp(required[i], available[j].extensionName) == 0) found = true;
        if (!found) {
            fprintf(stderr, "vulkan integration: required instance extension unavailable: %s\n", required[i]);
            free(available); goto cleanup;
        }
    }
    free(available);

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "yasl Vulkan WSI integration",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "yasl",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .flags = requirements.flags,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = required,
    };
    vr = vkCreateInstance(&instance_info, NULL, &instance);
    if (vr != VK_SUCCESS) { status = fail_vk("vkCreateInstance", vr); goto cleanup; }
    vr = yasl_vulkan_create_surface(surface, instance, NULL, &vk_surface);
    if (vr != VK_SUCCESS) { status = fail_vk("yasl_vulkan_create_surface", vr); goto cleanup; }

    VkSurfaceKHR duplicate = VK_NULL_HANDLE;
    if (yasl_vulkan_create_surface(surface, instance, NULL, &duplicate) != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR ||
        duplicate != VK_NULL_HANDLE || yasl_surface_destroy(surface) != -EBUSY ||
        yasl_shutdown() != -EBUSY || yasl_vulkan_init(vkGetInstanceProcAddr, &requirements) != -EBUSY) {
        fprintf(stderr, "vulkan integration: live WSI ownership was not enforced\n");
        goto cleanup;
    }
    if (yasl_vulkan_destroy_surface(surface) != 0 || yasl_vulkan_destroy_surface(surface) != 0) goto cleanup;
    vk_surface = VK_NULL_HANDLE;
    vr = yasl_vulkan_create_surface(surface, instance, NULL, &vk_surface);
    if (vr != VK_SUCCESS) { status = fail_vk("recreate Vulkan surface", vr); goto cleanup; }

    uint32_t physical_count = 0;
    vr = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (vr != VK_SUCCESS || !physical_count) {
        status = vr == VK_SUCCESS ? 1 : fail_vk("vkEnumeratePhysicalDevices", vr);
        if (vr == VK_SUCCESS) fprintf(stderr, "vulkan integration: no physical devices found\n");
        goto cleanup;
    }
    VkPhysicalDevice *physical = calloc(physical_count, sizeof(*physical));
    if (!physical) { fprintf(stderr, "vulkan integration: out of memory\n"); goto cleanup; }
    vr = vkEnumeratePhysicalDevices(instance, &physical_count, physical);
    if (vr != VK_SUCCESS) { free(physical); status = fail_vk("vkEnumeratePhysicalDevices", vr); goto cleanup; }

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t p = 0; p < physical_count && !selected; p++) {
        if (!has_device_extension(physical[p], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[p], &family_count, NULL);
        VkQueueFamilyProperties *families = family_count ? calloc(family_count, sizeof(*families)) : NULL;
        if (family_count && !families) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[p], &family_count, families);
        for (uint32_t q = 0; q < family_count; q++) {
            VkBool32 present = VK_FALSE;
            if ((families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                vkGetPhysicalDeviceSurfaceSupportKHR(physical[p], q, vk_surface, &present) == VK_SUCCESS && present) {
                selected = physical[p];
                queue_family = q;
                break;
            }
        }
        free(families);
    }
    free(physical);
    if (!selected) {
        fprintf(stderr, "vulkan integration: no graphics queue with VK_KHR_swapchain presentation support\n");
        goto cleanup;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(selected, &properties);
    fprintf(stderr, "vulkan integration: using %s\n", properties.deviceName);
    VkSurfaceCapabilitiesKHR capabilities;
    vr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(selected, vk_surface, &capabilities);
    if (vr != VK_SUCCESS) { status = fail_vk("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", vr); goto cleanup; }
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "vulkan integration: surface images cannot be transfer destinations\n");
        goto cleanup;
    }
    uint32_t format_count = 0;
    vr = vkGetPhysicalDeviceSurfaceFormatsKHR(selected, vk_surface, &format_count, NULL);
    if (vr != VK_SUCCESS || !format_count) {
        status = vr == VK_SUCCESS ? 1 : fail_vk("vkGetPhysicalDeviceSurfaceFormatsKHR", vr);
        if (vr == VK_SUCCESS) fprintf(stderr, "vulkan integration: surface has no formats\n");
        goto cleanup;
    }
    VkSurfaceFormatKHR *formats = calloc(format_count, sizeof(*formats));
    if (!formats) { fprintf(stderr, "vulkan integration: out of memory\n"); goto cleanup; }
    vr = vkGetPhysicalDeviceSurfaceFormatsKHR(selected, vk_surface, &format_count, formats);
    if (vr != VK_SUCCESS) { free(formats); status = fail_vk("vkGetPhysicalDeviceSurfaceFormatsKHR", vr); goto cleanup; }
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < format_count; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format = formats[i]; break; }
    if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
    free(formats);

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        int width = yasl_surface_get_info(surface).pixel_width;
        int height = yasl_surface_get_info(surface).pixel_height;
        if (width <= 0 || height <= 0) {
            fprintf(stderr, "vulkan integration: surface has no drawable framebuffer extent\n");
            goto cleanup;
        }
        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;
        if (extent.width < capabilities.minImageExtent.width) extent.width = capabilities.minImageExtent.width;
        if (extent.width > capabilities.maxImageExtent.width) extent.width = capabilities.maxImageExtent.width;
        if (extent.height < capabilities.minImageExtent.height) extent.height = capabilities.minImageExtent.height;
        if (extent.height > capabilities.maxImageExtent.height) extent.height = capabilities.maxImageExtent.height;
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & alpha)) {
        const VkCompositeAlphaFlagBitsKHR alphas[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        bool found = false;
        for (size_t i = 0; i < sizeof(alphas) / sizeof(alphas[0]); i++)
            if (capabilities.supportedCompositeAlpha & alphas[i]) { alpha = alphas[i]; found = true; break; }
        if (!found) { fprintf(stderr, "vulkan integration: no supported composite alpha mode\n"); goto cleanup; }
    }
    uint32_t present_mode_count = 0;
    vr = vkGetPhysicalDeviceSurfacePresentModesKHR(selected, vk_surface, &present_mode_count, NULL);
    if (vr != VK_SUCCESS || !present_mode_count) {
        status = vr == VK_SUCCESS ? 1 : fail_vk("vkGetPhysicalDeviceSurfacePresentModesKHR", vr);
        if (vr == VK_SUCCESS) fprintf(stderr, "vulkan integration: surface has no present modes\n");
        goto cleanup;
    }
    VkPresentModeKHR *present_modes = calloc(present_mode_count, sizeof(*present_modes));
    if (!present_modes) { fprintf(stderr, "vulkan integration: out of memory\n"); goto cleanup; }
    vr = vkGetPhysicalDeviceSurfacePresentModesKHR(selected, vk_surface, &present_mode_count, present_modes);
    free(present_modes);
    if (vr != VK_SUCCESS) { status = fail_vk("vkGetPhysicalDeviceSurfacePresentModesKHR", vr); goto cleanup; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
    };
    vr = vkCreateDevice(selected, &device_info, NULL, &device);
    if (vr != VK_SUCCESS) { status = fail_vk("vkCreateDevice", vr); goto cleanup; }
    VkQueue queue;
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkSwapchainCreateInfoKHR swap_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = vk_surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    vr = vkCreateSwapchainKHR(device, &swap_info, NULL, &swapchain);
    if (vr != VK_SUCCESS) { status = fail_vk("vkCreateSwapchainKHR", vr); goto cleanup; }
    uint32_t swap_image_count = 0;
    vr = vkGetSwapchainImagesKHR(device, swapchain, &swap_image_count, NULL);
    if (vr != VK_SUCCESS || !swap_image_count) {
        status = vr == VK_SUCCESS ? 1 : fail_vk("vkGetSwapchainImagesKHR", vr);
        if (vr == VK_SUCCESS) fprintf(stderr, "vulkan integration: swapchain returned no images\n");
        goto cleanup;
    }
    VkImage *images = calloc(swap_image_count, sizeof(*images));
    if (!images) { fprintf(stderr, "vulkan integration: out of memory\n"); goto cleanup; }
    vr = vkGetSwapchainImagesKHR(device, swapchain, &swap_image_count, images);
    if (vr != VK_SUCCESS) { free(images); status = fail_vk("vkGetSwapchainImagesKHR", vr); goto cleanup; }

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queue_family,
    };
    vr = vkCreateCommandPool(device, &pool_info, NULL, &command_pool);
    if (vr != VK_SUCCESS) { free(images); status = fail_vk("vkCreateCommandPool", vr); goto cleanup; }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vr = vkAllocateCommandBuffers(device, &command_info, &command);
    if (vr != VK_SUCCESS) { free(images); status = fail_vk("vkAllocateCommandBuffers", vr); goto cleanup; }
    VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vr = vkCreateSemaphore(device, &semaphore_info, NULL, &acquired);
    if (vr != VK_SUCCESS) { free(images); status = fail_vk("vkCreateSemaphore(acquired)", vr); goto cleanup; }
    vr = vkCreateSemaphore(device, &semaphore_info, NULL, &rendered);
    if (vr != VK_SUCCESS) { free(images); status = fail_vk("vkCreateSemaphore(rendered)", vr); goto cleanup; }

    uint32_t image_index = 0;
    vr = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image_index);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        free(images); status = fail_vk("vkAcquireNextImageKHR", vr); goto cleanup;
    }
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vr = vkBeginCommandBuffer(command, &begin);
    if (vr != VK_SUCCESS) { free(images); status = fail_vk("vkBeginCommandBuffer", vr); goto cleanup; }
    VkImageMemoryBarrier to_clear = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[image_index],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_clear);
    VkClearColorValue color = {.float32 = {0.08f, 0.18f, 0.32f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
    vr = vkEndCommandBuffer(command);
    free(images);
    if (vr != VK_SUCCESS) { status = fail_vk("vkEndCommandBuffer", vr); goto cleanup; }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &rendered,
    };
    vr = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) { status = fail_vk("vkQueueSubmit", vr); goto cleanup; }
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };
    vr = vkQueuePresentKHR(queue, &present);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        status = fail_vk("vkQueuePresentKHR", vr); goto cleanup;
    }
    vr = vkDeviceWaitIdle(device);
    if (vr != VK_SUCCESS) { status = fail_vk("vkDeviceWaitIdle", vr); goto cleanup; }
    fprintf(stderr, "vulkan integration: created surface, cleared and presented %ux%u swapchain image\n",
            extent.width, extent.height);
    status = 0;

cleanup:
    if (device) vkDeviceWaitIdle(device);
    if (rendered) vkDestroySemaphore(device, rendered, NULL);
    if (acquired) vkDestroySemaphore(device, acquired, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (vk_surface) yasl_vulkan_destroy_surface(surface);
    if (instance) vkDestroyInstance(instance, NULL);
    if (surface) yasl_surface_destroy(surface);
    yasl_shutdown();
    return status;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s wayland|x11\n", argv[0]);
        return 2;
    }
    enum yasl_backend backend;
    if (strcmp(argv[1], "wayland") == 0) backend = YASL_BACKEND_WAYLAND;
    else if (strcmp(argv[1], "x11") == 0) backend = YASL_BACKEND_X11;
    else {
        fprintf(stderr, "vulkan integration: unsupported backend '%s'\n", argv[1]);
        return 2;
    }
    return run_test(backend);
}
