/* Compositor restart: run by helpers/run_reconnect.py, which kills the compositor
 * after "ready" and starts a new one after "lost". Windows must survive, keep
 * their pixels, and accept new GL and Vulkan objects after DISPLAY_RESTORED. */
#include <yasl/yasl.h>
#if TEST_VULKAN
#include <yasl/vulkan.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, what) do { if (!(cond)) { const char *why; why = strerror(errno); \
    fprintf(stderr, "reconnect: %s failed at line %d (%s)\n", what, __LINE__, why); exit(1); } } while (0)

typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned);

static yasl_surface *cpu, *gl;

/* Waits up to timeout_ms for an event of the given type, failing on any CLOSE. */
static yasl_event expect(enum yasl_event_type type, yasl_surface *surface, int timeout_ms)
{
    yasl_event e;
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        CHECK(yasl_wait_event(&e, 50) >= 0, "wait");
        CHECK(e.type != YASL_EVENT_CLOSE, "keeping windows open");
        if (e.type == type && (!surface || e.surface == surface)) return e;
    }
    fprintf(stderr, "reconnect: timed out waiting for event %d\n", type);
    exit(1);
}

static void draw_gl(float red)
{
    CHECK((yasl_gl_make_current(gl) == 0), "binding GL");
    clear_color_fn clear_color = (clear_color_fn)yasl_gl_get_proc_address("glClearColor");
    clear_fn clear = (clear_fn)yasl_gl_get_proc_address("glClear");
    CHECK(clear_color && clear, "loading GL procedures");
    clear_color(red, 0.2f, 0.3f, 1);
    clear(0x00004000u);
    CHECK((yasl_gl_present(gl) == 0), "swapping GL buffers");
}

#if TEST_VULKAN
static VkInstance instance;
static yasl_surface *vk;

static VkSurfaceKHR vulkan_surface(void)
{
    VkSurfaceKHR out = VK_NULL_HANDLE;
    CHECK(yasl_vulkan_create_surface(vk, instance, NULL, &out) == VK_SUCCESS, "creating Vulkan surface");
    return out;
}
#endif

int main(void)
{
    CHECK(yasl_init(&(struct yasl_options){.backend = YASL_BACKEND_WAYLAND}) == 0, "selecting Wayland");
    cpu = yasl_surface_create(&(struct yasl_surface_options){.title="reconnect CPU", .width=160, .height=120});
    CHECK(cpu, "creating CPU window");
    gl = yasl_surface_create(&(struct yasl_surface_options){.title="reconnect GL", .width=160, .height=120, .type=YASL_SURFACE_GL});
    CHECK(gl, "creating GL window");

    expect(YASL_EVENT_REFRESH, cpu, 5000);
    int w, h;
    uint32_t *pixels = yasl_cpu_pixels(cpu, &w, &h);
    CHECK(pixels, "getting pixels");
    for (size_t i = 0; i < (size_t)w * h; i++) pixels[i] = 0xff336699u;
    CHECK(yasl_cpu_present(cpu) == 0, "presenting");
    draw_gl(0.8f);

#if TEST_VULKAN
    struct yasl_vulkan_requirements requirements;
    CHECK(yasl_vulkan_init(vkGetInstanceProcAddr, &requirements) == 0, "loading Vulkan");
    vk = yasl_surface_create(&(struct yasl_surface_options){.title="reconnect Vulkan", .width=160, .height=120, .type=YASL_SURFACE_NATIVE});
    CHECK(vk, "creating Vulkan window");
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = requirements.flags, .enabledExtensionCount = requirements.extension_count,
        .ppEnabledExtensionNames = requirements.extensions};
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_SUCCESS, "creating Vulkan instance");
    CHECK(vulkan_surface() != VK_NULL_HANDLE, "Vulkan handle");
#endif

    puts("ready");
    fflush(stdout);

    yasl_event lost = expect(YASL_EVENT_DISPLAY_LOST, NULL, 10000);
    CHECK(lost.action == YASL_LOST_RECONNECTING, "reporting a loss that reconnects");
    struct yasl_native native;
    CHECK(yasl_surface_get_native(cpu, &native) == -ENOTCONN && !native.wl_display && !native.wl_surface,
        "rejecting native handles while offline");
    CHECK(yasl_cpu_present(cpu) == -ENOTCONN, "rejecting CPU presentation while offline");
    CHECK(yasl_gl_make_current(gl) == -ENOTCONN, "rejecting GL binding while offline");
    CHECK(yasl_gl_make_current(NULL) == 0, "detaching GL while offline");
#if TEST_VULKAN
    CHECK(yasl_vulkan_destroy_surface(vk) == 0, "destroying Vulkan surface");
#endif
    CHECK(!yasl_surface_create(&(struct yasl_surface_options){.title="offline", .width=64, .height=64}), "refusing windows while offline");
    CHECK(errno == ENOTCONN, "reporting the disconnect");
    puts("lost");
    fflush(stdout);

    expect(YASL_EVENT_DISPLAY_RESTORED, NULL, 15000);
    CHECK(yasl_surface_get_native(cpu, &native) == 0 && native.wl_display && native.wl_surface,
        "querying restored native handles");
    expect(YASL_EVENT_REFRESH, cpu, 5000);
    int rw, rh;
    pixels = yasl_cpu_pixels(cpu, &rw, &rh);
    CHECK(pixels && rw == w && rh == h, "keeping the window size");
    CHECK(pixels[0] == 0xff336699u && pixels[(size_t)w * h - 1] == 0xff336699u, "keeping the pixels");
    CHECK(yasl_cpu_present(cpu) == 0, "presenting after reconnect");
    draw_gl(0.1f);
#if TEST_VULKAN
    CHECK(vulkan_surface() != VK_NULL_HANDLE, "restored Vulkan handle");
    CHECK(yasl_vulkan_destroy_surface(vk) == 0, "destroying Vulkan surface");
    vkDestroyInstance(instance, NULL);
#endif

    /* A second window proves the new connection accepts creation. */
    CHECK(yasl_surface_create(&(struct yasl_surface_options){.title="after reconnect", .width=64, .height=64}), "creating a window after reconnect");
    yasl_gl_make_current(NULL);
    yasl_shutdown();
    puts("restored");
    return 0;
}
