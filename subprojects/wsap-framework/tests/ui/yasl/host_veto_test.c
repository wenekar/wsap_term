#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "ui/yasl/internal.h"

/* The hooks count the requests. They cancel first and allow later, so the
 * test shows that the host asks again. */
struct veto_state {
    int close_asks;
    int quit_asks;
    bool close_allowed;
    bool quit_allowed;
};

static bool on_close(void *ud, struct window *w)
{
    (void)w;
    struct veto_state *state = ud;
    state->close_asks++;
    return state->close_allowed;
}

static bool on_quit_request(void *ud)
{
    struct veto_state *state = ud;
    state->quit_asks++;
    return state->quit_allowed;
}

static void on_quit(void *ud)
{
    struct veto_state *state = ud;
    assert(state->quit_allowed);
}

/* A canceled close keeps the window open. The host asks again on the next
 * request, and the close runs when the handler allows it. */
static void close_veto_test(void)
{
    struct veto_state state = {0};
    struct window_host host = {0};
    struct window w = {0};
    struct yasl_surface *surface = (struct yasl_surface *)&host;
    window_host_init(&host, NULL, NULL, &(struct window_host_hooks){
        .close = on_close,
        .quit_request = on_quit_request,
        .quit = on_quit,
        .ud = &state,
    });
    w.host = &host;
    w.surface = surface;
    w.natives[0].surface = surface;
    w.native_count = 1;
    window_host_add(&host, &w);
    host.destroying = true;

    assert(!window_host_request_close(&w));
    assert(state.close_asks == 1);
    assert(!w.closed);

    struct yasl_event ev = { .type = YASL_EVENT_CLOSE, .surface = surface };
    window_host_dispatch_event(&host, &ev);
    assert(state.close_asks == 2);
    assert(!w.closed);

    state.close_allowed = true;
    assert(window_host_request_close(&w));
    assert(state.close_asks == 3);
    window_host_registry_remove(&host);
    free(host.windows);
}

/* A canceled quit runs no action. A later allowed request runs it once. */
static void quit_veto_test(void)
{
    struct veto_state state = {0};
    struct window_host host = {0};
    window_host_init(&host, NULL, NULL, &(struct window_host_hooks){
        .quit_request = on_quit_request,
        .quit = on_quit,
        .ud = &state,
    });
    host.destroying = true;
    struct yasl_event ev = { .type = YASL_EVENT_QUIT };

    window_host_dispatch_event(&host, &ev);
    assert(state.quit_asks == 1);

    window_host_dispatch_event(&host, &ev);
    assert(state.quit_asks == 2);

    state.quit_allowed = true;
    window_host_dispatch_event(&host, &ev);
    assert(state.quit_asks == 3);
    window_host_registry_remove(&host);
    free(host.windows);
}

/* Without hooks the host allows both requests. */
static void no_hook_test(void)
{
    struct window_host host = {0};
    struct window w = {0};
    window_host_init(&host, NULL, NULL, NULL);
    w.host = &host;
    assert(window_host_request_close(&w));
    assert(window_host_request_quit(&host));
    window_host_registry_remove(&host);
    free(host.windows);
}

int main(void)
{
    close_veto_test();
    quit_veto_test();
    no_hook_test();
    return 0;
}
