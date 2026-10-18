#include "render/gpu/gpu.h"

#include <stddef.h>

/* The build without a GPU. There is no device, so no frame has a drawable. */

struct render_gpu *render_gpu_new(void)
{
    return NULL;
}

void render_gpu_free(struct render_gpu *g)
{
    (void)g;
}

void *render_gpu_native_device(struct render_gpu *g)
{
    (void)g;
    return NULL;
}

enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame)
{
    (void)g;
    (void)s;
    (void)fb;
    (void)frame;
    return RENDER_GPU_UNUSED;
}

bool render_gpu_present(struct render_gpu *g, struct render_surface *s, struct framebuffer *fb,
                        const struct damage_list *damage)
{
    (void)g;
    (void)s;
    (void)fb;
    (void)damage;
    return false;
}

void render_gpu_surface_free(void *gpu_surface)
{
    (void)gpu_surface;
}
