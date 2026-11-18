/* Built as strict C11 and C17 with -pedantic-errors: the public headers must not need newer C. */
#include <yasl/yasl.h>
#if YASL_HAS_VULKAN
#include <yasl/vulkan.h>
#endif
#include <assert.h>

int main(void)
{
    enum yasl_backend backend = YASL_BACKEND_AUTO;
    struct yasl_surface_options options = {.title = "portable", .width = 64, .height = 64};
    assert(backend == 0 && options.type == YASL_SURFACE_CPU && YASL_VERSION_MAJOR == 0);
#if YASL_HAS_VULKAN
    struct yasl_vulkan_requirements requirements = {0};
    PFN_vkGetInstanceProcAddr loader = NULL;
    _Static_assert(sizeof(VkInstanceCreateFlags) >= sizeof(uint32_t), "Vulkan flags type");
    assert(requirements.extensions == NULL && requirements.extension_count == 0);
    assert(requirements.flags == 0 && loader == NULL);
#endif
    return 0;
}
