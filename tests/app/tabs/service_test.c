/* The service step of the host pass retires closed tabs. A closed tab keeps
 * its session and its bar entry until its close time. The first host pass
 * after that time ends the session, also when the window draws no frame. */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>

#include "app/tabs/tabs.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"

#define WAIT_MS 5000

static uint64_t now_ms(void)
{
    return yasl_time_ns() / 1000000;
}

static bool process_gone(pid_t pid)
{
    return kill(pid, 0) < 0 && errno == ESRCH;
}

static pid_t shell_pid(struct terminal *content)
{
    pid_t pid = terminal_session(content)->pty.pid;
    assert(pid > 0);
    return pid;
}

static void closed_tab_ends_in_the_host_pass_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", (struct vector2i){ .x = 800, .y = 600 });
    assert(app_watch_signals(a.app));
    struct yasl_loop *loop = a.app->loop;
    /* The window has no native surface, so it never draws a frame. */
    struct window *w = test_terminal_window(a.app);
    assert(w);
    assert(app_tabs_new(w));
    pid_t stays_pid = shell_pid(app_tabs_active(w));
    assert(app_tabs_new(w));
    pid_t closed_pid = shell_pid(app_tabs_active(w));

    app_tabs_close(w, 1);
    uint64_t closed_at = now_ms();
    assert(app_tabs_count(w) == 1);
    int bar = app_tabs_bar_height(w);
    assert(bar > 0);

    /* Before the close time, a host pass keeps the closing tab. */
    window_host_schedule(&a.app->host);
    while (now_ms() < closed_at + APP_TABS_CLOSE_MS / 2)
        assert(yasl_loop_step(loop, 10) >= 0);
    assert(app_tabs_bar_height(w) == bar);
    assert(kill(closed_pid, 0) == 0);

    /* No pass runs by itself in this window. Ask for one after the close
     * time. */
    while (now_ms() <= closed_at + APP_TABS_CLOSE_MS)
        assert(yasl_loop_step(loop, 10) >= 0);
    window_host_schedule(&a.app->host);
    uint64_t end = now_ms() + WAIT_MS;
    while (!process_gone(closed_pid) && now_ms() < end)
        assert(yasl_loop_step(loop, 10) >= 0);
    assert(process_gone(closed_pid));
    assert(app_tabs_bar_height(w) == 0);
    assert(app_tabs_count(w) == 1);
    assert(kill(stays_pid, 0) == 0);

    window_close(w);
    test_app_fini(&a);
}

int main(void)
{
    closed_tab_ends_in_the_host_pass_test();
    puts("PASS: the host pass retires a closed tab after its close time");
    return 0;
}
