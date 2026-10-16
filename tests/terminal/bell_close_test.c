#include <assert.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "terminal/terminal.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"

static void step_until_closed(struct window_host *host)
{
    uint64_t deadline = yasl_time_ns() + 5000000000ull;
    while (host->count > 0 && yasl_time_ns() < deadline)
        assert(yasl_loop_step(host->loop, 50) >= 0);
    assert(host->count == 0);
}

int main(void)
{
    struct test_app app;
    test_app_init(&app, "printf '\\a'", (struct vector2i){ .x = 800, .y = 480 });
    app.app->config.terminal.visual_bell = true;
    assert(app_watch_signals(app.app));
    struct window_host *host = &app.app->host;

    struct window *w = test_terminal_window(host, &app.app->config.terminal);
    assert(w);
    assert(terminal_new_tab(w));
    step_until_closed(host);
    window_host_reap(host);

    uint64_t end = yasl_time_ns() + 300000000ull;
    while (yasl_time_ns() < end)
        assert(yasl_loop_step(host->loop, 20) >= 0);

    test_app_fini(&app);
    return 0;
}
