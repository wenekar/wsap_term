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
 * library check does not prove that the loader and a driver exist on the
 * machine that runs the program. */

#if defined(__linux__)
typedef void *(*egl_get_display_fn)(void *display_id);
typedef void *(*egl_get_platform_display_fn)(unsigned platform, void *display_id,
                                             const intptr_t *attribs);

static pthread_once_t gl_once = PTHREAD_ONCE_INIT;
static bool gl_available;

static void probe_gl(void)
{
    static const char *const names[] = {"libEGL.so.1", "libEGL.so"};
    void *handle = NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !handle; i++)
        handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return;
    egl_get_display_fn get_display = (egl_get_display_fn)dlsym(handle, "eglGetDisplay");
    if (!get_display) return;
    /* EGL_DEFAULT_DISPLAY is the native display value zero. */
    if (get_display(NULL)) {
        gl_available = true;
        return;
    }
    egl_get_platform_display_fn get_platform =
        (egl_get_platform_display_fn)dlsym(handle, "eglGetPlatformDisplay");
    if (!get_platform)
        get_platform = (egl_get_platform_display_fn)dlsym(handle, "eglGetPlatformDisplayEXT");
    /* EGL_PLATFORM_SURFACELESS_MESA works without a window system. */
    if (get_platform && get_platform(0x31DDu, NULL, NULL))
        gl_available = true;
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
static PFN_vkEnumerateInstanceExtensionProperties vulkan_extensions;

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
    if (!get_proc) return;
    vulkan_extensions = (PFN_vkEnumerateInstanceExtensionProperties)
        get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
}

/* The loader lists the extensions of every installed ICD, so a surface
 * extension proves that a driver for this window system exists. */
bool yasl_private_runtime_has_vulkan(const char *surface_extension)
{
    if (!surface_extension) return false;
    pthread_once(&vulkan_once, probe_vulkan);
    if (!vulkan_extensions) return false;
    uint32_t count = 0;
    if (vulkan_extensions(NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (!properties) return false;
    bool ok = vulkan_extensions(NULL, &count, properties) == VK_SUCCESS;
    bool surface = false, platform = false;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (!strcmp(properties[i].extensionName, "VK_KHR_surface")) surface = true;
        if (!strcmp(properties[i].extensionName, surface_extension)) platform = true;
    }
    free(properties);
    return ok && surface && platform;
}
#else
bool yasl_private_runtime_has_vulkan(const char *surface_extension)
{
    (void)surface_extension;
    return false;
}
#endif
