#ifndef WSAP_PAINT_VULKAN_H
#define WSAP_PAINT_VULKAN_H

/*
 * Vulkan structures for window presentation.
 * This header connects the Vulkan renderer and the window layer.
 */

#include <stdbool.h>
#include <vulkan/vulkan.h>

/* The native device details of the Vulkan renderer. */
struct vulkan_device {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
};

/* The surface target for window presentation. */
struct vulkan_drawable {
    VkSurfaceKHR surface;
    void *userdata;
    bool unpaced;
};

#endif
