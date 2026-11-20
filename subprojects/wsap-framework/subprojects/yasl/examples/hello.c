/* A CPU-rendered window. */
#include <yasl/yasl.h>

int main(void)
{
    yasl_surface *surface = yasl_surface_create(
        &(struct yasl_surface_options){.title = "Hello YASL", .width = 640, .height = 480});
    if (!surface)
        return 1;
    yasl_event event;
    while (yasl_wait_event(&event, -1) >= 0) {
        if (event.type == YASL_EVENT_CLOSE || event.type == YASL_EVENT_QUIT)
            break;
        if (event.type != YASL_EVENT_REFRESH)
            continue;
        int w, h;
        uint32_t *pixels = yasl_cpu_pixels(surface, &w, &h);
        if (!pixels)
            break;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                /* Opaque premultiplied ARGB: red grows rightward, green downward. */
                uint32_t r = (uint32_t)(x * 255 / (w > 1 ? w - 1 : 1));
                uint32_t g = (uint32_t)(y * 255 / (h > 1 ? h - 1 : 1));
                pixels[(size_t)y * w + x] = 0xff000000 | r << 16 | g << 8 | (255 - r);
            }
        yasl_cpu_present(surface);
    }
    return yasl_shutdown() < 0;
}
