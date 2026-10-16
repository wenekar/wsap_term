#if YASL_HAS_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#if YASL_HAS_X11
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#if YASL_HAS_COCOA
#define VK_USE_PLATFORM_METAL_EXT
#endif
#include "yasl/vulkan.h"
#include "public_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static PFN_vkGetInstanceProcAddr get_proc;
static bool portability;
void yasl_private_vulkan_reset(void)
{
    get_proc = NULL;
    portability = false;
}
struct yasl_vulkan_binding {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkAllocationCallbacks allocator;
    bool has_allocator;
    PFN_vkDestroySurfaceKHR destroy;
};
static bool backend_requirements(struct yasl_vulkan_requirements *out)
{
    static const char *const wayland[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    static const char *const x11[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    static const char *const metal[] = {"VK_KHR_surface", "VK_EXT_metal_surface",
                                        "VK_KHR_portability_enumeration"};
    switch (yasl_public_display()->ops->type) {
    case YASL_PRIVATE_BACKEND_WAYLAND:
        *out = (struct yasl_vulkan_requirements){wayland, 2, 0};
        return true;
    case YASL_PRIVATE_BACKEND_X11:
        *out = (struct yasl_vulkan_requirements){x11, 2, 0};
        return true;
    case YASL_PRIVATE_BACKEND_COCOA:
        *out = (struct yasl_vulkan_requirements){
            metal, portability ? 3 : 2,
            portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0};
        return true;
    default:
        return yasl_public_fail(ENOTSUP);
    }
}
int yasl_vulkan_init(PFN_vkGetInstanceProcAddr loader, struct yasl_vulkan_requirements *out)
{
    if (!out)
        return yasl_public_error(EINVAL);
    *out = (struct yasl_vulkan_requirements){0};
    if (!loader)
        return yasl_public_error(EINVAL);
    if (!yasl_public_initialize())
        return -errno;
    for (yasl_surface *s = yasl_public_display()->surfaces; s; s = s->event_next)
        if (s->vulkan)
            return yasl_public_error(EBUSY);
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)loader(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!enumerate)
        return yasl_public_error(ENOTSUP);
    uint32_t count = 0;
    VkResult result = enumerate(NULL, &count, NULL);
    VkExtensionProperties *ext = NULL;
    if (result == VK_SUCCESS && count) {
        if (!(ext = calloc(count, sizeof(*ext))))
            return yasl_public_error(ENOMEM);
        result = enumerate(NULL, &count, ext);
    }
    bool next_portability = false;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++)
        if (!strcmp(ext[i].extensionName, "VK_KHR_portability_enumeration"))
            next_portability = true;
    free(ext);
    if (result != VK_SUCCESS)
        return yasl_public_error(EIO);
    get_proc = loader;
    portability = next_portability;
    return backend_requirements(out) ? 0 : -errno;
}
int yasl_vulkan_destroy_surface(yasl_surface *s)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    struct yasl_vulkan_binding *binding = s->vulkan;
    if (!binding)
        return 0;
    binding->destroy(binding->instance, binding->surface,
                     binding->has_allocator ? &binding->allocator : NULL);
    free(binding);
    s->vulkan = NULL;
    s->presenter = YASL_PRIVATE_PRESENTER_NONE;
    return 0;
}
VkResult yasl_vulkan_create_surface(yasl_surface *s, VkInstance instance,
                                    const VkAllocationCallbacks *allocator, VkSurfaceKHR *out)
{
    if (out)
        *out = VK_NULL_HANDLE;
    if (!yasl_public_surface(s, true))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!instance || !out) {
        yasl_public_fail(EINVAL);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!get_proc) {
        yasl_public_fail(ENODEV);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (atomic_load(&s->display->offline)) {
        yasl_public_fail(ENOTCONN);
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    if (s->vulkan || s->presenter != YASL_PRIVATE_PRESENTER_NONE) {
        yasl_public_fail(EBUSY);
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    PFN_vkDestroySurfaceKHR destroy =
        (PFN_vkDestroySurfaceKHR)get_proc(instance, "vkDestroySurfaceKHR");
    if (!destroy) {
        yasl_public_fail(ENOTSUP);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    struct yasl_vulkan_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        yasl_public_fail(ENOMEM);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    switch (s->display->ops->type) {
#if YASL_HAS_WAYLAND
    case YASL_PRIVATE_BACKEND_WAYLAND: {
        struct yasl_native native;
        if (yasl_surface_get_native(s, &native) < 0)
            break;
        PFN_vkCreateWaylandSurfaceKHR create =
            (PFN_vkCreateWaylandSurfaceKHR)get_proc(instance, "vkCreateWaylandSurfaceKHR");
        if (create) {
            VkWaylandSurfaceCreateInfoKHR info = {
                .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                .display = native.wl_display,
                .surface = native.wl_surface};
            result = create(instance, &info, allocator, out);
        }
        break;
    }
#endif
#if YASL_HAS_X11
    case YASL_PRIVATE_BACKEND_X11: {
        struct yasl_native native;
        if (yasl_surface_get_native(s, &native) < 0)
            break;
        PFN_vkCreateXlibSurfaceKHR create =
            (PFN_vkCreateXlibSurfaceKHR)get_proc(instance, "vkCreateXlibSurfaceKHR");
        if (create) {
            VkXlibSurfaceCreateInfoKHR info = {.sType =
                                                   VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                                               .dpy = native.x11_display,
                                               .window = (Window)native.x11_window};
            result = create(instance, &info, allocator, out);
        }
        break;
    }
#endif
#if YASL_HAS_COCOA
    case YASL_PRIVATE_BACKEND_COCOA: {
        PFN_vkCreateMetalSurfaceEXT create =
            (PFN_vkCreateMetalSurfaceEXT)get_proc(instance, "vkCreateMetalSurfaceEXT");
        if (create && s->display->ops->surface_get_metal_layer) {
            VkMetalSurfaceCreateInfoEXT info = {
                .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                .pLayer = s->display->ops->surface_get_metal_layer(s)};
            result = create(instance, &info, allocator, out);
        }
        break;
    }
#endif
    default:
        break;
    }
    if (result == VK_SUCCESS) {
        binding->instance = instance;
        binding->surface = *out;
        binding->destroy = destroy;
        binding->has_allocator = allocator != NULL;
        if (allocator)
            binding->allocator = *allocator;
        s->vulkan = binding;
        s->presenter = YASL_PRIVATE_PRESENTER_VULKAN;
    } else {
        free(binding);
        yasl_public_fail(result == VK_ERROR_EXTENSION_NOT_PRESENT ? ENOTSUP : EIO);
    }
    return result;
}
