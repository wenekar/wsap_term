/* Mouse and pen input on a CPU surface. */
#include <yasl/yasl.h>
#include <string.h>

int main(int argc, char **argv)
{
    bool smoke = argc == 2 && !strcmp(argv[1], "--smoke-test");
    yasl_surface *s = yasl_surface_create(
        &(struct yasl_surface_options){.title = "YASL input", .width = 640, .height = 480});
    if (!s)
        return 1;
    double px = -100, py = -100, pressure = 0;
    bool dirty = false;
    int frames = 0, result = 1;
    yasl_event e;
    for (;;) {
        int n = yasl_wait_event(&e, smoke ? 20 : -1);
        if (n < 0)
            break;
        if (e.type == YASL_EVENT_CLOSE || e.type == YASL_EVENT_QUIT ||
            (e.type == YASL_EVENT_KEY && e.key.key == YASL_KEY_ESCAPE)) {
            result = 0;
            break;
        }
        if (e.type == YASL_EVENT_POINTER || e.type == YASL_EVENT_PEN) {
            px = e.x;
            py = e.y;
            pressure = e.type == YASL_EVENT_PEN ? ((e.pen.axes & YASL_PEN_PRESSURE) ? e.pen.pressure : 0)
                       : (e.pointer.buttons & YASL_BUTTON_BIT(YASL_BUTTON_PRIMARY)) ? 0.5
                                                                                      : 0;
            dirty = true;
        }
        if (e.type == YASL_EVENT_REFRESH)
            dirty = true;
        if (dirty) {
            int w, h;
            uint32_t *pixels = yasl_cpu_pixels(s, &w, &h);
            if (!pixels)
                break;
            double scale = yasl_surface_get_info(s).scale, radius = (8 + pressure * 48) * scale;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    double dx = x - px * scale, dy = y - py * scale;
                    pixels[(size_t)y * w + x] = dx * dx + dy * dy < radius * radius ? 0xffffc060u : 0xff203040u;
                }
            if (yasl_cpu_present(s) < 0)
                break;
            dirty = false;
            frames++;
        }
        if (smoke && frames && n == 0) {
            result = 0;
            break;
        }
    }
    yasl_shutdown();
    return result;
}
