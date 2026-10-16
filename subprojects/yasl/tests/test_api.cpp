#include <yasl/yasl.h>
#if YASL_HAS_VULKAN
#include <yasl/vulkan.h>
#endif
#include <cassert>
int main()
{
    yasl_event e{};
    e.x = 1.5;
    e.pen.pressure = 0.25;
    assert(e.pen.pressure == 0.25 && e.text == nullptr);
    yasl_surface_options options{};
    options.width = -1;
    options.height = 10;
    assert(!yasl_surface_create(&options) && errno == EINVAL);
    yasl_gl_options gl{};
    assert(gl.major == 0 && gl.share == nullptr);
    int w = 0, h = 0;
    assert(!yasl_cpu_pixels(nullptr, &w, &h) && errno == ENODEV);
    yasl_native native{};
    assert(yasl_surface_get_native(nullptr, &native) == -ENODEV);
    yasl_shutdown();
    return 0;
}
