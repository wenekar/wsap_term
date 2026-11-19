/* Pen canvas: pressure, eraser and coherent strokes drawn straight into the CPU surface. */
#include <yasl/yasl.h>
#include <string.h>
#include <math.h>

#define PAPER 0xfff0eee8u
#define INK 0xff202830u

static void stamp(uint32_t *pixels, int w, int h, double x, double y, double radius, bool erase)
{
    int left = (int)floor(x - radius), top = (int)floor(y - radius);
    int right = (int)ceil(x + radius), bottom = (int)ceil(y + radius);
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > w) right = w;
    if (bottom > h) bottom = h;
    for (int row = top; row < bottom; row++) {
        for (int col = left; col < right; col++) {
            double dx = col - x, dy = row - y;
            if (dx * dx + dy * dy <= radius * radius) pixels[(size_t)row * w + col] = erase ? PAPER : INK;
        }
    }
}

/* Paints paper over the area a resize added, or everything when clear is set. */
static uint32_t *canvas(yasl_surface *s, int *w, int *h, int *painted_w, int *painted_h, bool clear)
{
    uint32_t *pixels = yasl_cpu_pixels(s, w, h);
    if (!pixels) return NULL;
    if (clear) *painted_w = *painted_h = 0;
    for (int y = 0; y < *h; y++)
        for (int x = y < *painted_h ? *painted_w : 0; x < *w; x++) pixels[(size_t)y * *w + x] = PAPER;
    *painted_w = *w;
    *painted_h = *h;
    return pixels;
}

int main(int argc, char **argv)
{
    bool smoke = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--smoke-test")) smoke = true;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            const char *b = argv[++i];
            enum yasl_backend backend = !strcmp(b, "wayland") ? YASL_BACKEND_WAYLAND :
                                        !strcmp(b, "x11") ? YASL_BACKEND_X11 : YASL_BACKEND_COCOA;
            if (yasl_init(&(struct yasl_options){.backend = backend}) < 0) return 1;
        } else return 1;
    }
    yasl_surface *s = yasl_surface_create(&(struct yasl_surface_options){.title="YASL pen canvas — C clears, Escape exits", .width=800, .height=600});
    if (!s) return 1;
    int painted_w = 0, painted_h = 0, frames = 0, result = 1;
    bool contact = false;
    double last_x = 0, last_y = 0;
    yasl_event e;
    for (;;) {
        int n = yasl_wait_event(&e, smoke ? 20 : -1);
        if (n < 0) break;
        if (smoke && frames && n == 0) { result = 0; break; }
        bool escape = e.type == YASL_EVENT_KEY && e.action == YASL_KEY_PRESS && e.key.key == YASL_KEY_ESCAPE;
        if (e.type == YASL_EVENT_CLOSE || e.type == YASL_EVENT_QUIT || escape) { result = 0; break; }
        bool clear = false, draw = false, erase = false, down = false, present = e.type == YASL_EVENT_REFRESH;
        double x = 0, y = 0, p = 0.5;
        switch (e.type) {
        case YASL_EVENT_KEY:
            if (e.action == YASL_KEY_PRESS && e.key.key == YASL_KEY_C) clear = present = true;
            break;
        case YASL_EVENT_PEN:
            x = e.x; y = e.y;
            p = (e.pen.axes & YASL_PEN_PRESSURE) ? e.pen.pressure : 0.5;
            erase = e.pen.tool == YASL_PEN_ERASER;
            down = e.pen.in_contact;
            draw = down && (e.action == YASL_PEN_DOWN || e.action == YASL_PEN_MOVE);
            if (e.action == YASL_PEN_LEAVE || e.action == YASL_PEN_CANCEL) contact = false;
            break;
        case YASL_EVENT_POINTER:
            x = e.x; y = e.y;
            down = (e.pointer.buttons & YASL_BUTTON_BIT(YASL_BUTTON_PRIMARY)) != 0;
            draw = down && (e.action == YASL_POINTER_DOWN || e.action == YASL_POINTER_MOVE);
            break;
        case YASL_EVENT_INPUT_RESET:
            contact = false;
            break;
        default:
            break;
        }
        if (draw || present) {
            int w, h;
            int old_w = painted_w, old_h = painted_h;
            uint32_t *pixels = canvas(s, &w, &h, &painted_w, &painted_h, clear);
            if (!pixels) break;
            if (w != old_w || h != old_h) contact = false;
            if (draw) {
                double scale = yasl_surface_get_info(s).scale;
                x *= scale; y *= scale;
                double radius = (1.5 + 12 * p) * scale, dx = contact ? x - last_x : 0, dy = contact ? y - last_y : 0;
                int steps = (int)ceil(hypot(dx, dy) / (radius * 0.4));
                if (steps < 1) steps = 1;
                for (int i = 1; i <= steps; i++) stamp(pixels, w, h, x - dx + dx * i / steps, y - dy + dy * i / steps, radius, erase);
                last_x = x; last_y = y;
            }
            if (yasl_cpu_present(s) < 0) break;
            frames++;
        }
        if (e.type == YASL_EVENT_PEN || e.type == YASL_EVENT_POINTER) contact = down;
    }
    yasl_shutdown();
    return result;
}
