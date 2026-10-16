#include "render/backend.h"

struct framebuffer_gpu { int unused; };
static struct framebuffer_gpu test_gpu;
static bool redraw_failure;

struct framebuffer_gpu *render_test_gpu_instance(void) { return &test_gpu; }
void render_test_gpu_redraw_failure(bool fail) { redraw_failure = fail; }

struct framebuffer_gpu *render_gpu_create(void) { return &test_gpu; }
void render_gpu_destroy(struct framebuffer_gpu *g) { (void)g; }
void *render_gpu_device(struct framebuffer_gpu *g) { return g; }
bool render_gpu_begin(struct framebuffer_gpu *g, struct framebuffer *fb, uint32_t base)
{
    (void)fb; (void)base;
    return g == &test_gpu;
}
bool render_gpu_redraw(struct framebuffer_gpu *g)
{
    if (g != &test_gpu)
        return false;
    bool fail = redraw_failure;
    redraw_failure = false;
    return fail;
}
bool render_gpu_submit(struct framebuffer_gpu *g, struct framebuffer *fb)
{
    (void)fb;
    return g == &test_gpu;
}

bool render_gpu_ui_scene(struct framebuffer_gpu *g, const struct ui_scene *scene,
                         struct font_instance *font)
{
    (void)g;
    (void)scene;
    (void)font;
    return false;
}
