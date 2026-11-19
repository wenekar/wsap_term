#define _POSIX_C_SOURCE 200809L
/*
 * The window composes a retained canvas.
 *
 * A native surface keeps its canvas and its history. The window marks a
 * frame fresh only when the pixels are not what the last frame drew, so
 * the renderer repaints only what changed. A frame that marks every frame
 * fresh makes the renderer paint the whole canvas at each display tick.
 */
#include <assert.h>
#include <stdio.h>

#include "font/font.h"
#include "helpers/render_draw.h"
#include "ui/yasl/internal.h"

struct compose_run {
    struct renderer *r;
    struct render_frame frame;
    struct render_result result;
    int frames;
};

static void compose(struct window *w, struct window_frame *frame, void *ud)
{
    struct compose_run *c = ud;
    (void)w;
    c->frames++;
    c->result = render_draw(c->r, &frame->target, &c->frame);
}

int main(void)
{
    assert(render_init());
    assert(font_instance_default());
    struct renderer *r = renderer_create(NULL);
    assert(r);

    struct tterm term;
    tterm_open(&term, 40, 10);
    tterm_feed(&term, "hello");
    const struct vector2i size = { .x = 400, .y = 160 };
    struct tcanvas canvas;
    tcanvas_init(&canvas, size.x, size.y);

    struct compose_run run = {
        .r = r,
        .frame = tframe(tterm_frame(&term, true), font_instance_default(), size),
    };
    struct window w = { .compose = compose, .compose_ud = &run };
    struct window_native n = {0};
    struct framebuffer target = { .pixels = canvas.px, .size = size };

    /* The first frame has no history, so it paints every pixel. */
    window_native_compose(&w, &n, target);
    assert(run.frames == 1);
    assert(run.result.damage.all);

    /* The same frame again keeps the canvas, so nothing changed. */
    window_native_compose(&w, &n, target);
    assert(run.frames == 2);
    if (run.result.damage.all || run.result.damage.any || run.result.damage.n != 0) {
        fprintf(stderr, "compose_test: frame 2 reported %d rects (all %d); want none\n",
                run.result.damage.n, run.result.damage.all);
        return 1;
    }

    /* A frame of unknown pixels paints every pixel again. */
    target.fresh = true;
    window_native_compose(&w, &n, target);
    assert(run.frames == 3);
    assert(run.result.damage.all);

    /* After a failed present the next frame paints every pixel again. */
    target.fresh = false;
    n.repaint = true;
    window_native_compose(&w, &n, target);
    assert(run.frames == 4);
    assert(run.result.damage.all);

    framebuffer_history_release(&n.history);
    tcanvas_free(&canvas);
    tterm_close(&term);
    renderer_destroy(r);
    render_shutdown();
    printf("compose_test passed\n");
    return 0;
}
