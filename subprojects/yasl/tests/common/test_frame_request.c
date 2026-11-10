#include "common/backend.h"

#include <assert.h>
#include <stdio.h>

/*
 * Tests of a frame request (common/backend.c).
 *
 * A request must wake the gate of a backend that answers it at its next display
 * callback. It must not schedule a raw redraw: a raw redraw answers the request
 * at once and bypasses the pacing of the surface. A backend without such a gate
 * leaves the request to the interval timer, and the CPU presenter keeps its
 * redraw, which the display flush paces.
 *
 * The test drives the operations of a stand-in backend, so it runs on every
 * platform and depends on no display.
 */

struct gate {
    int wakes;
    int raw_redraws;
};

static void count_wake(struct yasl_surface *s)
{
    struct gate *g = s->ud;
    g->wakes++;
}

static void count_redraw(struct yasl_surface *s)
{
    struct gate *g = s->ud;
    g->raw_redraws++;
}

static const struct yasl_private_backend_ops gated = {
    .surface_wake_frame_request = count_wake,
    .surface_schedule_redraw = count_redraw,
};

static const struct yasl_private_backend_ops ungated = {
    .surface_schedule_redraw = count_redraw,
};

struct fixture {
    struct yasl_private_display display;
    struct yasl_surface surface;
    struct gate gate;
};

/* A surface of `presenter` on a display of `ops`. The loop is absent, so a
 * wake is a no-op. */
static void fixture_init(struct fixture *f, const struct yasl_private_backend_ops *ops,
                         enum yasl_private_presenter presenter)
{
    *f = (struct fixture){
        .display = { .ops = ops, .loop = NULL },
        .surface = { .presenter = presenter },
        .gate = {0},
    };
    f->surface.display = &f->display;
    f->surface.ud = &f->gate;
}

int main(void)
{
    /* A gated backend that draws itself: the request wakes the gate once and
     * schedules no redraw. */
    struct fixture f;
    fixture_init(&f, &gated, YASL_PRIVATE_PRESENTER_METAL);
    yasl_private_surface_request_frame(&f.surface);
    assert(f.gate.wakes == 1);
    assert(f.gate.raw_redraws == 0);
    assert(f.surface.refresh_owed == false);

    /* The same request on a backend without a gate: the interval timer owns the
     * answer, so the request is neither answered at once nor turned into a raw
     * redraw. */
    fixture_init(&f, &ungated, YASL_PRIVATE_PRESENTER_VULKAN);
    yasl_private_surface_request_frame(&f.surface);
    assert(f.gate.wakes == 0);
    assert(f.gate.raw_redraws == 0);
    assert(f.surface.refresh_owed == false);

    /* The CPU presenter draws in the display flush, which the display paces, so
     * it keeps its redraw. */
    fixture_init(&f, &ungated, YASL_PRIVATE_PRESENTER_CPU);
    yasl_private_surface_request_frame(&f.surface);
    assert(f.gate.raw_redraws == 1);
    assert(f.gate.wakes == 0);

    /* A gate serves both presenters: the backend owns the answer. */
    fixture_init(&f, &gated, YASL_PRIVATE_PRESENTER_CPU);
    yasl_private_surface_request_frame(&f.surface);
    assert(f.gate.wakes == 1);
    assert(f.gate.raw_redraws == 0);

    /* No surface is not an error. */
    yasl_private_surface_request_frame(NULL);

    puts("frame request: gate wake only, no raw redraw");
    return 0;
}
