#include "runtime/runtime.h"

#include <stdlib.h>
#ifdef __GLIBC__
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#include "render/render.h"
#include "yasl/yasl.h"

#define GPU_IDLE_RELEASE_MS 500u

struct app_runtime {
    struct renderer *renderer;
    struct yasl_loop *loop;
    uint64_t gpu_idle_timer;
    uint64_t last_gpu_draw_ms;
    bool trimmed_first_frame;
};

void app_runtime_trim_heap(void)
{
#ifdef __GLIBC__
    malloc_trim(0);
#elif defined(__APPLE__)
    malloc_zone_pressure_relief(NULL, 0);
#endif
}

static uint64_t gpu_idle_fire(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct app_runtime *rt = ud;
    uint64_t now_ms = yasl_time_ns() / 1000000;
    uint64_t elapsed = now_ms >= rt->last_gpu_draw_ms ? now_ms - rt->last_gpu_draw_ms : 0;
    if (elapsed < GPU_IDLE_RELEASE_MS)
        return (uint64_t)(GPU_IDLE_RELEASE_MS - elapsed) * 1000000ull;
    if (!renderer_idle_gpu(rt->renderer))
        return 100ull * 1000000ull;
    rt->gpu_idle_timer = 0;
    app_runtime_trim_heap();
    return 0;
}

void app_runtime_note_gpu_draw(struct app_runtime *rt, uint64_t now_ms)
{
    rt->last_gpu_draw_ms = now_ms;
    if (!rt->loop || rt->gpu_idle_timer)
        return;
    rt->gpu_idle_timer = yasl_loop_add_timer(rt->loop,
                                             (uint64_t)GPU_IDLE_RELEASE_MS * 1000000ull,
                                             gpu_idle_fire, rt);
}

void app_runtime_note_first_frame(struct app_runtime *rt)
{
    if (rt->trimmed_first_frame)
        return;
    rt->trimmed_first_frame = true;
    app_runtime_trim_heap();
}

struct app_runtime *app_runtime_create(struct renderer *renderer,
                                       struct yasl_loop *loop)
{
    if (!renderer)
        return NULL;
    struct app_runtime *rt = calloc(1, sizeof(*rt));
    if (!rt)
        return NULL;
    rt->renderer = renderer;
    rt->loop = loop;
    return rt;
}

struct renderer *app_runtime_renderer(struct app_runtime *rt)
{
    return rt ? rt->renderer : NULL;
}

void app_runtime_destroy(struct app_runtime *rt)
{
    if (!rt)
        return;
    if (rt->loop && rt->gpu_idle_timer)
        yasl_loop_del_timer(rt->loop, rt->gpu_idle_timer);
    free(rt);
}

bool app_runtime_paint_scene(void *ud, struct font_instance *font,
                             uint32_t *pixels, struct vector2i size,
                             const struct ui_scene *scene)
{
    struct app_runtime *rt = ud;
    if (!rt)
        return false;
    struct framebuffer target = {
        .pixels = pixels,
        .size = size,
        .fresh = true,
    };
    struct renderer_frame frame = {
        .ui = scene,
        .ui_font = font,
        .repaint = true,
        .cpu_only = true,
    };
    (void)renderer_draw(rt->renderer, &target, &frame);
    return true;
}
