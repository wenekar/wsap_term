
#include <assert.h>

#include "render/backend.h"

struct framebuffer_gpu *render_gpu_create(void) { return NULL; }
void render_gpu_destroy(struct framebuffer_gpu *g) { (void)g; }
void *render_gpu_device(struct framebuffer_gpu *g) { (void)g; return NULL; }
bool render_gpu_begin(struct framebuffer_gpu *g, struct framebuffer *fb, uint32_t base)
{
    (void)g; (void)fb; (void)base;
    return false;
}
bool render_gpu_redraw(struct framebuffer_gpu *g) { (void)g; return false; }
bool render_gpu_submit(struct framebuffer_gpu *g, struct framebuffer *fb)
{
    (void)g; (void)fb;
    return false;
}

enum render_backend_result render_gpu_terminal(struct framebuffer_gpu *g,
                                               const struct terminal_frame *frame,
                                               const struct render_backend_view *view)
{
    (void)g; (void)frame; (void)view;
    assert(!"a CPU-only build has no GPU frame");
    return RENDER_BACKEND_SKIP;
}

bool render_gpu_ui_scene(struct framebuffer_gpu *g, const struct ui_scene *scene,
                         struct font_instance *font)
{
    (void)g; (void)scene; (void)font;
    assert(!"a CPU-only build has no GPU frame");
    return false;
}
