/** @file Vulkan window surfaces; the application owns devices and swapchains. */
#ifndef YASL_VULKAN_H
#define YASL_VULKAN_H
#include "yasl/yasl.h"
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Instance extensions and flags YASL needs, borrowed until shutdown. Merge
 * them into your VkInstanceCreateInfo. */
struct yasl_vulkan_requirements {
    const char *const *extensions;
    uint32_t extension_count;
    VkInstanceCreateFlags flags;
};
/** Records the loader and reports the requirements, initializing YASL if needed.
 * Call before creating the instance. EBUSY while a Vulkan surface exists. */
YASL_API int yasl_vulkan_init(PFN_vkGetInstanceProcAddr loader, struct yasl_vulkan_requirements *out);
/** One surface per NATIVE window; returns the VkResult, with errno set on failure. */
YASL_API VkResult yasl_vulkan_create_surface(yasl_surface *surface, VkInstance instance,
                                             const VkAllocationCallbacks *allocator,
                                             VkSurfaceKHR *out);
/** Destroy swapchains first. Also works after DISPLAY_LOST; no surface is a no-op. */
YASL_API int yasl_vulkan_destroy_surface(yasl_surface *surface);
#ifdef __cplusplus
}
#endif
#endif
