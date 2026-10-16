#include <yasl/yasl.h>
#include <assert.h>
#include <string.h>
static void on_event(void *userdata, const yasl_event *event)
{
    (void)userdata;
    (void)event;
}
int main(void)
{
    assert(yasl_shutdown() == 0);
    yasl_wake();
    yasl_event event = {.type = YASL_EVENT_QUIT};
    assert(yasl_wait_event(&event, 0) == -ENODEV && event.type == YASL_EVENT_NONE);
    assert(yasl_wait_event(NULL, 0) == -EINVAL);
    assert(yasl_init(&(struct yasl_options){.backend = (enum yasl_backend)999}) == -EINVAL);
    /* The loop and its event callback come together or not at all. */
    yasl_loop *loop = yasl_loop_create();
    assert(loop);
    assert(yasl_init(&(struct yasl_options){.loop = loop}) == -EINVAL);
    assert(yasl_init(&(struct yasl_options){.on_event = on_event}) == -EINVAL);
    assert(yasl_loop_destroy(loop) == 0);
    assert(yasl_surface_request_frame(NULL) == -ENODEV);
    assert(yasl_get_backend() == YASL_BACKEND_AUTO && yasl_get_features() == 0);
    assert(!yasl_surface_create(NULL) && errno == EINVAL);
    struct yasl_surface_options options = {.width = 0, .height = 64};
    assert(!yasl_surface_create(&options) && errno == EINVAL);
    options.width = 64;
    options.type = (enum yasl_surface_type)99;
    assert(!yasl_surface_create(&options) && errno == EINVAL);
    options.type = YASL_SURFACE_GL;
    options.gl.major = 9;
    assert(!yasl_surface_create(&options) && errno == EINVAL);
    int w = 9, h = 9;
    assert(!yasl_cpu_pixels(NULL, &w, &h) && errno == ENODEV && w == 0 && h == 0);
    assert(yasl_cpu_present(NULL) == -ENODEV);
    assert(!yasl_metal_get_layer(NULL) && errno == ENODEV);
    struct yasl_native native = {.x11_window = 7};
    assert(yasl_surface_get_native(NULL, &native) == -ENODEV && native.x11_window == 0);
    assert(yasl_surface_destroy(NULL) == 0);
    assert(yasl_time_ns() > 0);
    struct yasl_surface_info info = yasl_surface_get_info(NULL);
    assert(info.width == 0 && info.pixel_height == 0 && info.scale == 1.0);
    assert(yasl_gl_make_current(NULL) == 0);
    assert(yasl_get_double_click_ms() == 400);
    assert(yasl_get_system_string(YASL_EXECUTABLE_PATH, NULL, 0) > 0);
    char locale[64];
    size_t n = yasl_get_system_string(YASL_LOCALE, locale, sizeof(locale));
    assert(n > 0 && n < sizeof(locale) && !strpbrk(locale, ".@"));
    assert(yasl_get_system_string((enum yasl_system_string)9, locale, sizeof(locale)) == 0 && errno == EINVAL);
    assert(yasl_shutdown() == 0);
    return 0;
}
