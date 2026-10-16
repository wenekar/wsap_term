#define _POSIX_C_SOURCE 200809L
#include "common/private_native.h"
#include "common/private_loop.h"
#include "common/types.h"

#include <assert.h>
#include <time.h>

struct state {
    struct yasl_private_display *display;
    struct yasl_surface *surface;
    unsigned draws;
};

static void draw(struct yasl_surface *surface, struct yasl_private_frame *frame, void *ud)
{
    struct state *state = ud;
    assert(surface == state->surface && frame->surface == surface);
    assert(yasl_private_surface_display(surface) == state->display);
    assert(frame->target.pixels);
    struct yasl_private_native_frame native;
    assert(yasl_private_surface_get_native_frame(surface, &frame->target, &native));
    assert(native.drawable);
    struct yasl_private_vector2i size = yasl_private_surface_geometry(surface).size;
    assert(size.x == frame->target.size.x && size.y == frame->target.size.y);
    for (int i = 0; i < size.x * size.y; i++) frame->target.pixels[i] = 0xff4080c0;
    struct yasl_private_damage_list damage = {.n = YASL_PRIVATE_DAMAGE_MAX + 1};
    assert(!yasl_private_surface_present(surface, &frame->target, &damage));
    damage.n = -1;
    struct yasl_private_framebuffer invalid = frame->target;
    invalid.size.x++;
    assert(!yasl_private_surface_present(surface, &invalid, &damage));
    invalid = frame->target;
    invalid.generation--;
    assert(!yasl_private_surface_present(surface, &invalid, &damage));
    assert(!yasl_private_surface_present(surface, &frame->target, NULL));
    assert(yasl_private_surface_present(surface, &frame->target, &damage));
    assert(!yasl_private_surface_get_native_frame(surface, &frame->target, &native));
    assert(!yasl_private_surface_present(surface, &frame->target, &damage));
    state->draws++;
    yasl_private_surface_destroy(surface);
    state->surface = NULL;
}

static void test_window(enum yasl_private_backend request, bool null_spec)
{
    struct yasl_private_display_spec spec = {.backend = request};
    struct state state = {.display = yasl_private_display_open(NULL, null_spec ? NULL : &spec)};
    assert(state.display);
    assert(yasl_private_display_backend(state.display) == YASL_PRIVATE_BACKEND_COCOA);
    struct yasl_private_window_cbs callbacks = {.draw = draw};
    /* Check real Metal draw/ownership without waiting for a display-link tick. */
    struct yasl_private_window_spec surface = {
        .title = "Native backend test", .size = {160, 100}, .tearing = true,
        .cbs = &callbacks, .ud = &state,
    };
    state.surface = yasl_private_surface_create_toplevel(state.display, &surface);
    assert(state.surface);
    assert(!state.draws);
    callbacks.draw = NULL;
    yasl_private_surface_schedule_redraw(state.surface);
    uint64_t deadline = yasl_private_loop_now_ns() + 3000000000ull;
    while (!state.draws && yasl_private_loop_now_ns() < deadline) {
        yasl_loop_step(yasl_private_display_loop(state.display), 0);
        yasl_private_display_flush(state.display);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    assert(state.draws == 1 && !state.surface);
    yasl_private_display_close(state.display);
}

int main(void)
{
    test_window(YASL_PRIVATE_BACKEND_AUTO, true);
    test_window(YASL_PRIVATE_BACKEND_AUTO, false);
    test_window(YASL_PRIVATE_BACKEND_COCOA, false);
    return 0;
}
