#include <assert.h>

#include "ui/yasl/internal.h"
#include "ui/yasl/window.h"

/*
 * window_present owns the repaint flag. A GPU present that fails must ask for
 * a full repaint, because the canvas lost its pixels. A GPU present that
 * succeeds must not.
 */

static int surface_token;

static struct window_frame gpu_frame(struct yasl_surface *surface)
{
    return (struct window_frame){
        .surface = surface,
        .target = { .drawable = &surface_token },
    };
}

static void failed_gpu_present_sets_repaint_test(void)
{
    struct window w = {0};
    struct yasl_surface *surface = (struct yasl_surface *)(void *)&surface_token;
    assert(window_native_add(&w, surface));
    struct window_native *n = &w.natives[0];
    struct window_frame frame = gpu_frame(surface);

    assert(!window_present(&w, &frame, NULL, false));
    assert(!n->frame_submitted);
    assert(n->repaint);
}

static void submitted_gpu_frame_clears_repaint_test(void)
{
    struct window w = {0};
    struct yasl_surface *surface = (struct yasl_surface *)(void *)&surface_token;
    assert(window_native_add(&w, surface));
    struct window_native *n = &w.natives[0];
    n->repaint = true;
    struct window_frame frame = gpu_frame(surface);

    assert(window_present(&w, &frame, NULL, true));
    assert(n->frame_submitted);
    assert(!n->repaint);
}

int main(void)
{
    failed_gpu_present_sets_repaint_test();
    submitted_gpu_frame_clears_repaint_test();
    return 0;
}
