#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The private probes answer the graphics features of yasl_get_features and
 * yasl_has_feature. This test checks their cost, not their internals: a probe
 * must not keep a driver that the caller did not ask for. */
bool yasl_private_runtime_has_gl(void);
bool yasl_private_runtime_has_vulkan(const char *surface_extension);

/* Reads the mapped objects of this process. The test is Linux only. */
static int mapped(const char *name)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    assert(maps);
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof(line), maps))
        found = strstr(line, name) != NULL;
    fclose(maps);
    return found;
}

static void require_unmapped(const char *name)
{
    if (!mapped(name))
        return;
    fprintf(stderr, "runtime: %s is still mapped\n", name);
    abort();
}

int main(void)
{
    /* Neither the test binary nor the static library links a graphics loader. */
    require_unmapped("libEGL");
    require_unmapped("libvulkan");

    /* "Is GL there" must not make an EGL display. The Mesa driver of an EGL
     * display holds about 17 MB resident. The probe unloads the loader again. */
    bool gl = yasl_private_runtime_has_gl();
    require_unmapped("libgallium");
    require_unmapped("libEGL_mesa");
    require_unmapped("libEGL");

    /* The answer is cached: a second call adds no mapping. */
    assert(yasl_private_runtime_has_gl() == gl);

    /* The extension list is read once and kept. The loader does not stay
     * mapped, so a Vulkan query costs no resident memory. */
    assert(!yasl_private_runtime_has_vulkan(NULL));
    bool wayland = yasl_private_runtime_has_vulkan("VK_KHR_wayland_surface");
    assert(yasl_private_runtime_has_vulkan("VK_KHR_wayland_surface") == wayland);
    /* A second extension is answered from the same cached list. */
    bool xlib = yasl_private_runtime_has_vulkan("VK_KHR_xlib_surface");
    assert(yasl_private_runtime_has_vulkan("VK_KHR_xlib_surface") == xlib);
    require_unmapped("libvulkan");
    require_unmapped("libgallium");
    return 0;
}
