#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if YASL_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

bool yasl_private_runtime_has_gl(void);
bool yasl_private_runtime_has_vulkan(const char *surface_extension);

/* Runtime probes for the optional graphics APIs. A compile-time header or
 * library check does not prove that the loader and the driver exist on the
 * machine that runs the program. A probe runs on the first query for its own
 * feature, loads only its own loader, and unloads that loader before it
 * returns. A caller that does not ask for a feature maps nothing. */

#if defined(__linux__)
static pthread_once_t gl_once = PTHREAD_ONCE_INIT;
static bool gl_available;

static void probe_gl(void)
{
    static const char *const names[] = {"libEGL.so.1", "libEGL.so"};
    void *handle = NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !handle; i++)
        handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return;
    /* A loader that exports an EGL entry point can provide a display, and the
     * display would in turn load the GL driver, which holds about 17 MB
     * resident. The probe therefore only looks for the entry points. */
    gl_available = dlsym(handle, "eglGetDisplay") != NULL ||
                   dlsym(handle, "eglGetPlatformDisplay") != NULL ||
                   dlsym(handle, "eglGetPlatformDisplayEXT") != NULL;
    dlclose(handle);
}

bool yasl_private_runtime_has_gl(void)
{
    pthread_once(&gl_once, probe_gl);
    return gl_available;
}
#else
bool yasl_private_runtime_has_gl(void)
{
    return false;
}
#endif

#if YASL_HAS_VULKAN
static pthread_once_t vulkan_once = PTHREAD_ONCE_INIT;
/* The instance extension list of every installed driver. probe_vulkan()
 * reads the list once and keeps it because the array is small; the loader
 * itself does not stay mapped. */
static VkExtensionProperties *vulkan_extensions;
static uint32_t vulkan_extension_count;

static void probe_vulkan(void)
{
    static const char *const names[] = {
#if defined(__APPLE__)
        "libvulkan.1.dylib", "libMoltenVK.dylib", "libvulkan.dylib",
#else
        "libvulkan.so.1", "libvulkan.so",
#endif
    };
    void *handle = NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !handle; i++)
        handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return;
    PFN_vkGetInstanceProcAddr get_proc =
        (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    PFN_vkEnumerateInstanceExtensionProperties enumerate = get_proc
        ? (PFN_vkEnumerateInstanceExtensionProperties)
              get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")
        : NULL;
    uint32_t count = 0;
    if (enumerate && enumerate(NULL, &count, NULL) == VK_SUCCESS && count > 0) {
        VkExtensionProperties *properties = calloc(count, sizeof(*properties));
        if (properties && enumerate(NULL, &count, properties) == VK_SUCCESS) {
            vulkan_extensions = properties;
            vulkan_extension_count = count;
        } else {
            free(properties);
        }
    }
    dlclose(handle);
}

/* The list covers every installed driver (ICD), so VK_KHR_surface together
 * with the caller's surface extension proves that a driver for this window
 * system exists. A NULL extension returns false without probing. */
bool yasl_private_runtime_has_vulkan(const char *surface_extension)
{
    if (!surface_extension) return false;
    pthread_once(&vulkan_once, probe_vulkan);
    bool surface = false, platform = false;
    for (uint32_t i = 0; i < vulkan_extension_count; i++) {
        if (!strcmp(vulkan_extensions[i].extensionName, "VK_KHR_surface")) surface = true;
        if (!strcmp(vulkan_extensions[i].extensionName, surface_extension)) platform = true;
    }
    return surface && platform;
}
#else
bool yasl_private_runtime_has_vulkan(const char *surface_extension)
{
    (void)surface_extension;
    return false;
}
#endif
