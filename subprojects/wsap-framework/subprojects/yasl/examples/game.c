/* GPU example: the application owns context binding, drawing and swap pacing. */
#include <yasl/yasl.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned);
typedef void (*viewport_fn)(int, int, int, int);

int main(int argc, char **argv)
{
    bool smoke = argc == 2 && !strcmp(argv[1], "--smoke-test");
    yasl_surface *s = yasl_surface_create(&(struct yasl_surface_options){
        .title = "YASL OpenGL", .width = 800, .height = 600, .type = YASL_SURFACE_GL});
    if (!s || yasl_gl_make_current(s) < 0) {
        fprintf(stderr, "%s\n", strerror(errno));
        yasl_shutdown();
        return 1;
    }
    clear_color_fn clear_color = (clear_color_fn)yasl_gl_get_proc_address("glClearColor");
    clear_fn clear = (clear_fn)yasl_gl_get_proc_address("glClear");
    viewport_fn viewport = (viewport_fn)yasl_gl_get_proc_address("glViewport");
    if (!clear_color || !clear || !viewport) {
        yasl_shutdown();
        return 1;
    }
    yasl_gl_set_swap_interval(s, 1);
    int result = 0, frames = 0;
    bool online = true;
    for (bool running = true; running;) {
        /* Drain events before each frame; while the display is gone, block until it returns. */
        yasl_event e;
        int n = yasl_wait_event(&e, online ? 0 : -1);
        if (n < 0) {
            result = 1;
            break;
        }
        if (e.type == YASL_EVENT_CLOSE || e.type == YASL_EVENT_QUIT ||
            (e.type == YASL_EVENT_KEY && e.key.key == YASL_KEY_ESCAPE))
            running = false;
        if (e.type == YASL_EVENT_DISPLAY_LOST) {
            yasl_gl_make_current(NULL);
            online = false;
        }
        if (e.type == YASL_EVENT_DISPLAY_RESTORED) {
            /* The context is new: bind it again and restore GL state here. */
            online = yasl_gl_make_current(s) == 0 && yasl_gl_set_swap_interval(s, 1) == 0;
        }
        if (n > 0 || !online || !running)
            continue;
        struct yasl_surface_info info = yasl_surface_get_info(s);
        viewport(0, 0, info.pixel_width, info.pixel_height);
        double t = yasl_time_ns() / 1e9;
        clear_color((float)(0.3 + 0.2 * sin(t)), 0.2f, 0.4f, 1);
        clear(0x00004000u);
        if (yasl_gl_present(s) < 0) {
            result = 1;
            break;
        }
        if (smoke && ++frames == 3)
            break;
    }
    yasl_gl_make_current(NULL);
    yasl_shutdown();
    return result;
}
