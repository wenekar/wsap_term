#define _POSIX_C_SOURCE 200809L

#include <yasl/yasl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef const unsigned char *(*get_string_fn)(unsigned);
typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned);
typedef void (*finish_fn)(void);
typedef void (*read_pixels_fn)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*gen_textures_fn)(int, unsigned *);
typedef void (*bind_texture_fn)(unsigned, unsigned);
typedef unsigned char (*is_texture_fn)(unsigned);
typedef void (*delete_textures_fn)(int, const unsigned *);

enum { GL_VERSION_ = 0x1f02, GL_COLOR_BUFFER_BIT_ = 0x00004000,
       GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401, GL_TEXTURE_2D_ = 0x0de1 };

static yasl_surface *primary;
static yasl_surface *shared;
static yasl_surface *cpu;

static void fail(const char *message, int line)
{
    const char *description = NULL;
    int error = errno;
    description = strerror(error);
    fprintf(stderr, "GL integration failure at line %d: %s (YASL error %d: %s, native error %d)\n",
            line, message, error, description ? description : "none", errno);
    (void)yasl_gl_make_current(NULL);
    if (cpu) yasl_surface_destroy(cpu);
    if (shared) yasl_surface_destroy(shared);
    if (primary) yasl_surface_destroy(primary);
    yasl_shutdown();
    exit(1);
}

#define REQUIRE(condition, message) do { if (!(condition)) fail((message), __LINE__); } while (0)

#define LOAD_GL(function, type, symbol) do { \
    union { yasl_gl_proc generic; type typed; } conversion; \
    conversion.generic = yasl_gl_get_proc_address(symbol); \
    REQUIRE(conversion.generic != NULL, "missing OpenGL function " symbol); \
    function = conversion.typed; \
} while (0)

static double seconds_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void wait_for_size(yasl_surface *surface, int width, int height)
{
    double deadline = seconds_now() + 8.0;
    int actual_width = 0, actual_height = 0;
    while (seconds_now() < deadline) {
        struct yasl_surface_info info = yasl_surface_get_info(surface);
        actual_width = info.width;
        actual_height = info.height;
        if (actual_width == width && actual_height == height) return;
        yasl_event event = {0};
        int result = yasl_wait_event(&event, 50);
        REQUIRE(result >= 0, "waiting for configure event");
    }
    char message[128];
    snprintf(message, sizeof(message), "resize did not settle at %dx%d (got %dx%d)",
             width, height, actual_width, actual_height);
    fail(message, __LINE__);
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2, "expected backend argument: wayland or x11");
    if (!strcmp(argv[1], "wayland")) REQUIRE(yasl_init(&(struct yasl_options){.backend = YASL_BACKEND_WAYLAND}) == 0, "select Wayland backend");
    else if (!strcmp(argv[1], "x11")) REQUIRE(yasl_init(&(struct yasl_options){.backend = YASL_BACKEND_X11}) == 0, "select X11 backend");
    else REQUIRE(false, "backend must be wayland or x11");

    primary = yasl_surface_create(&(struct yasl_surface_options){.title="YASL GL test", .width=320, .height=240, .type=YASL_SURFACE_GL});
    REQUIRE(yasl_get_features() & YASL_FEATURE_OPENGL, "report OpenGL support");
    REQUIRE(primary != NULL, "create default OpenGL 3.2 surface");
    REQUIRE((yasl_gl_make_current(primary) == 0), "make first GL context current");

    get_string_fn get_string = NULL;
    clear_color_fn clear_color = NULL;
    clear_fn clear = NULL;
    finish_fn finish = NULL;
    read_pixels_fn read_pixels = NULL;
    gen_textures_fn gen_textures = NULL;
    bind_texture_fn bind_texture = NULL;
    is_texture_fn is_texture = NULL;
    delete_textures_fn delete_textures = NULL;
    LOAD_GL(get_string, get_string_fn, "glGetString");
    LOAD_GL(clear_color, clear_color_fn, "glClearColor");
    LOAD_GL(clear, clear_fn, "glClear");
    LOAD_GL(finish, finish_fn, "glFinish");
    LOAD_GL(read_pixels, read_pixels_fn, "glReadPixels");
    LOAD_GL(gen_textures, gen_textures_fn, "glGenTextures");
    LOAD_GL(bind_texture, bind_texture_fn, "glBindTexture");
    LOAD_GL(is_texture, is_texture_fn, "glIsTexture");
    LOAD_GL(delete_textures, delete_textures_fn, "glDeleteTextures");
    const unsigned char *version = get_string(GL_VERSION_);
    REQUIRE(version && version[0], "query GL version string");

    clear_color(0.2f, 0.4f, 0.8f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT_);
    finish();
    unsigned char pixel[4] = {0};
    int pixel_width = 0, pixel_height = 0;
    pixel_width = yasl_surface_get_info(primary).pixel_width;
    pixel_height = yasl_surface_get_info(primary).pixel_height;
    REQUIRE(pixel_width > 0 && pixel_height > 0, "query initial framebuffer size");
    read_pixels(pixel_width / 2, pixel_height / 2, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, pixel);
    REQUIRE(abs((int)pixel[0] - 51) <= 3 && abs((int)pixel[1] - 102) <= 3 &&
            abs((int)pixel[2] - 204) <= 3 && pixel[3] >= 250,
            "read rendered pixel from the native GL framebuffer");
    REQUIRE((yasl_gl_set_swap_interval(primary, 0) == 0), "disable swap interval");
    REQUIRE((yasl_gl_present(primary) == 0), "swap first GL surface");

    unsigned texture = 0;
    gen_textures(1, &texture);
    bind_texture(GL_TEXTURE_2D_, texture);
    REQUIRE(is_texture(texture), "create a shareable GL texture");

    shared = yasl_surface_create(&(struct yasl_surface_options){.title="YASL GL shared context", .width=160, .height=120, .type=YASL_SURFACE_GL, .gl = {.share = primary}});
    REQUIRE(shared != NULL, "create shared GL context");
    REQUIRE((yasl_gl_make_current(shared) == 0), "make shared context current");
    REQUIRE(is_texture(texture), "verify context shares GL objects");
    delete_textures(1, &texture);
    REQUIRE((yasl_gl_make_current(primary) == 0), "restore primary GL context");

    REQUIRE((yasl_surface_request_size(primary, 400, 300) == 0), "request surface resize");
    wait_for_size(primary, 400, 300);
    pixel_width = yasl_surface_get_info(primary).pixel_width;
    pixel_height = yasl_surface_get_info(primary).pixel_height;
    double scale = yasl_surface_get_info(primary).scale;
    REQUIRE(abs(pixel_width - (int)lround(400 * scale)) <= 1 &&
            abs(pixel_height - (int)lround(300 * scale)) <= 1,
            "query resized framebuffer dimensions");
    clear_color(0.7f, 0.1f, 0.3f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT_);
    REQUIRE((yasl_gl_present(primary) == 0), "swap resized GL surface");

    /* An animation that asks for each next frame gets one REFRESH per frame
     * shown, even though GL surfaces also get a REFRESH when a frame is shown. */
    REQUIRE(yasl_surface_request_frame(primary) == 0, "request a frame");
    int frames = 0;
    double animation_end = seconds_now() + 1.0;
    while (seconds_now() < animation_end) {
        yasl_event event = {0};
        int result = yasl_wait_event(&event, 50);
        REQUIRE(result >= 0, "wait during animation");
        if (result == 0 || event.type != YASL_EVENT_REFRESH || event.surface != primary)
            continue;
        frames++;
        REQUIRE(yasl_surface_request_frame(primary) == 0, "request the next frame");
        clear(GL_COLOR_BUFFER_BIT_);
        REQUIRE(yasl_gl_present(primary) == 0, "present an animation frame");
    }
    fprintf(stderr, "%d REFRESH events in 1 s of GL animation\n", frames);
    REQUIRE(frames >= 20 && frames <= 75, "one REFRESH per frame shown");

    REQUIRE(yasl_surface_create(&(struct yasl_surface_options){.title="invalid attrs", .width=100, .height=100, .type=YASL_SURFACE_GL, .gl = {.major = -1}}) == NULL && errno == EINVAL,
            "reject invalid GL context options");
    cpu = yasl_surface_create(&(struct yasl_surface_options){.title="CPU surface", .width=100, .height=100});
    REQUIRE(yasl_surface_create(&(struct yasl_surface_options){.title="no CPU sharing", .width=100, .height=100, .type=YASL_SURFACE_GL, .gl = {.share = cpu}}) == NULL && errno == EINVAL,
            "reject a non-GL share surface");
    REQUIRE(cpu != NULL, "create CPU surface for incompatibility check");
    REQUIRE((yasl_gl_make_current(cpu) < 0), "reject CPU surface as a GL context");

    REQUIRE((yasl_gl_make_current(NULL) == 0), "detach GL context before destruction");
    yasl_surface_destroy(cpu); cpu = NULL;
    yasl_surface_destroy(shared); shared = NULL;
    yasl_surface_destroy(primary); primary = NULL;
    yasl_shutdown();
    return 0;
}
