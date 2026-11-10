#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "common/backend.h"
#include "common/private_loop.h"
#include "common/private_native.h"
#include "common/private_native_cocoa.h"
#include "common/types.h"

#include <assert.h>

/*
 * Tests of the Cocoa frame pacing and of the presentation policy.
 *
 * A surface that the application draws waits for a display tick, and the tick
 * answers its frame request. No interval timer gates it. A CPU surface shows
 * one frame for a burst of redraw requests, and an unpaced one draws the frame
 * that it is asked for at once. The tearing option decides the display sync of
 * a Metal layer.
 */

@interface NSView (YaslTestDisplayLink)
- (void)displayLinkFired:(id)link;
@end

struct state {
    struct yasl_private_display *display;
    unsigned draws;
};

/* Frames that the system loop forced through the refresh callback. */
static unsigned refresh_calls;

static void draw(struct yasl_surface *surface, struct yasl_private_frame *frame, void *ud)
{
    struct state *state = ud;
    struct yasl_private_native_frame native;
    assert(yasl_private_surface_get_native_frame(surface, &frame->target, &native));
    assert(native.drawable);
    struct yasl_private_damage_list damage = {.n = -1};
    assert(yasl_private_surface_present(surface, &frame->target, &damage));
    state->draws++;
}

static void refresh(yasl_surface *surface)
{
    (void)surface;
    refresh_calls++;
}

static void pump(struct state *state)
{
    yasl_loop_step(yasl_private_display_loop(state->display), 0);
    yasl_private_display_flush(state->display);
}

/* Answers the pending frame intent the way the screen does. */
static void tick_once(struct yasl_surface *surface)
{
    NSWindow *window = (NSWindow *)yasl_private_cocoa_surface_get_window(surface);
    assert(window && window.contentView);
    [window.contentView displayLinkFired:nil];
}

/* Steps the loop until the surface stops drawing, so a later frame comes from
 * a redraw request and not from the frames before it. */
static void settle(struct state *state)
{
    unsigned last = state->draws;
    for (int quiet = 0; quiet < 25;) {
        yasl_loop_step(yasl_private_display_loop(state->display), 5);
        yasl_private_display_flush(state->display);
        if (state->draws == last) {
            quiet++;
        } else {
            quiet = 0;
            last = state->draws;
        }
    }
}

static void wait_for_draw(struct state *state, unsigned want)
{
    uint64_t deadline = yasl_private_loop_now_ns() + 2000000000ull;
    while (state->draws < want && yasl_private_loop_now_ns() < deadline)
        yasl_loop_step(yasl_private_display_loop(state->display), 5);
    assert(state->draws >= want);
}

static struct yasl_surface *make_window(struct state *state, const char *title, bool tearing)
{
    struct yasl_private_window_cbs callbacks = {.draw = draw};
    struct yasl_private_window_spec spec = {
        .title = title, .size = {96, 64}, .tearing = tearing,
        .cbs = &callbacks, .ud = state,
    };
    struct yasl_surface *surface =
        yasl_private_surface_create_toplevel(state->display, &spec);
    assert(surface);
    return surface;
}

static void test_paced(struct state *state)
{
    struct yasl_surface *surface = make_window(state, "paced", false);
    settle(state);
    unsigned before = state->draws;
    /* Redraws asked for between two display frames make one frame. */
    yasl_private_surface_schedule_redraw(surface);
    yasl_private_surface_schedule_redraw(surface);
    yasl_private_surface_schedule_redraw(surface);
    wait_for_draw(state, before + 1);
    settle(state);
    assert(state->draws == before + 1);
    yasl_private_surface_destroy(surface);
}

static void test_unpaced(struct state *state)
{
    struct yasl_surface *surface = make_window(state, "unpaced", true);
    settle(state);
    unsigned before = state->draws;
    /* An unpaced surface draws the frame it is asked for at once. */
    yasl_private_surface_schedule_redraw(surface);
    pump(state);
    assert(state->draws == before + 1);
    /* The frame before it was shown, so nothing more is owed. */
    settle(state);
    assert(state->draws == before + 1);
    yasl_private_surface_destroy(surface);
}

/* A window that the application draws itself: no CPU canvas, and a Metal
 * layer that the bridge claims. */
static struct yasl_surface *make_native_window(struct state *state, const char *title,
                                               bool tearing)
{
    struct yasl_private_window_spec spec = {
        .title = title, .size = {96, 64}, .external_gpu = true, .tearing = tearing,
    };
    struct yasl_surface *surface =
        yasl_private_surface_create_toplevel(state->display, &spec);
    assert(surface);
    return surface;
}

/* The refresh callback reports a frame that the system loop forces, as during a
 * live resize. A redraw request outside that loop must not use it: the REFRESH
 * event paces the draw of a surface that the application draws itself. */
static void test_native_redraw(struct state *state)
{
    struct yasl_surface *surface = make_native_window(state, "native", false);
    assert(surface->presenter != YASL_PRIVATE_PRESENTER_CPU);
    surface->refresh = refresh;
    state->display->modal = true;
    refresh_calls = 0;
    yasl_private_surface_schedule_redraw(surface);
    assert(refresh_calls == 0);
    state->display->modal = false;
    surface->refresh = NULL;
    yasl_private_surface_destroy(surface);
}

/* A display tick answers the frame intent of an application-drawn surface, in
 * the tick itself. No interval timer gates it. */
static void test_tick_delivery(struct state *state, bool tearing)
{
    struct yasl_surface *surface = make_native_window(state, "tick", tearing);
    /* No loop step runs between the request and the tick, so only the tick can
     * answer the request. */
    yasl_private_surface_request_shown_frame(surface);
    yasl_private_surface_request_frame(surface);
    assert(!surface->refresh_owed);
    tick_once(surface);
    assert(surface->refresh_owed);
    /* The next request waits for the next tick, and the interval fallback is
     * not armed for it. A real tick may answer the request first, so read the
     * timer only. */
    surface->refresh_owed = false;
    yasl_private_surface_request_shown_frame(surface);
    yasl_private_surface_request_frame(surface);
    assert(!surface->refresh_owed);
    yasl_loop_step(yasl_private_display_loop(state->display), 0);
    assert(surface->frame_timer == 0);
    if (!surface->refresh_owed) {
        tick_once(surface);
        assert(surface->refresh_owed);
    }
    yasl_private_surface_destroy(surface);
}

/* The tearing option is the presentation policy of the layer. */
static void test_display_sync(struct state *state)
{
    const bool tearing[] = {false, true};
    for (size_t i = 0; i < sizeof tearing / sizeof tearing[0]; i++) {
        struct yasl_surface *surface = make_native_window(state, "display sync", tearing[i]);
        CAMetalLayer *layer =
            (CAMetalLayer *)surface->display->ops->surface_get_metal_layer(surface);
        assert(layer);
        assert(layer.displaySyncEnabled == (tearing[i] ? NO : YES));
        yasl_private_surface_destroy(surface);
    }
}

int main(void)
{
    struct state state = {
        .display = yasl_private_display_open(NULL, &(struct yasl_private_display_spec){
            .backend = YASL_PRIVATE_BACKEND_COCOA,
        }),
    };
    assert(state.display);
    test_display_sync(&state);
    test_tick_delivery(&state, false);
    test_tick_delivery(&state, true);
    test_native_redraw(&state);
    test_paced(&state);
    test_unpaced(&state);
    yasl_private_display_close(state.display);
    return 0;
}
