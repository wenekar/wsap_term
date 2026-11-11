/* The app owns every terminal. A tab keeps its session when it moves to
 * another window, and its reports reach the window that holds it. Closing a
 * window ends the sessions of that window only. */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "app/tabs/tabs.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"

#define WAIT_MS 5000

static bool step_until(struct app *app, bool (*done)(void *), void *ud)
{
    uint64_t end = yasl_time_ns() + (uint64_t)WAIT_MS * 1000000ull;
    while (!done(ud)) {
        if (yasl_time_ns() >= end)
            return false;
        assert(yasl_loop_step(app->loop, 10) >= 0);
    }
    return true;
}

struct title_wait {
    struct window *window;
    const char *title;
};

static bool has_title(void *ud)
{
    struct title_wait *t = ud;
    const char *title = window_title(t->window);
    return title && !strcmp(title, t->title);
}

static bool process_gone(void *ud)
{
    pid_t pid = *(pid_t *)ud;
    return kill(pid, 0) < 0 && errno == ESRCH;
}

struct count_wait {
    struct window *window;
    int count;
};

static bool has_count(void *ud)
{
    struct count_wait *c = ud;
    return app_tabs_count(c->window) == c->count;
}

static pid_t shell_pid(struct terminal *content)
{
    pid_t pid = terminal_session(content)->pty.pid;
    assert(pid > 0);
    return pid;
}

static void write_to(struct terminal *content, const char *text)
{
    assert(session_write_bytes(terminal_session(content), text, strlen(text)));
}

static void moved_tab_keeps_its_session_and_reports_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", (struct vector2i){ .x = 800, .y = 600 });
    assert(app_watch_signals(a.app));
    struct window *source = test_terminal_window(a.app);
    struct window *target = test_terminal_window(a.app);
    assert(source && target);
    assert(app_tabs_new(source) && app_tabs_new(source));
    assert(app_tabs_new(target));

    struct terminal *moved = app_tabs_active(source);
    assert(app_tabs_active_index(source) == 1);
    app_tabs_select(source, 0);
    struct terminal *stays = app_tabs_active(source);
    pid_t moved_pid = shell_pid(moved);
    pid_t stays_pid = shell_pid(stays);

    assert(app_tabs_move(source, 1, target, -1));
    assert(app_tabs_count(source) == 1);
    assert(app_tabs_count(target) == 2);
    assert(app_tabs_active(target) == moved);

    /* The title report of the moved tab reaches its new window. */
    write_to(moved, "\033]2;moved-tab\007\n");
    assert(step_until(a.app, has_title,
                      &(struct title_wait){ target, "moved-tab" }));
    const char *source_title = window_title(source);
    assert(!source_title || strcmp(source_title, "moved-tab"));

    /* Closing the source window ends its own session, and only that one. */
    window_close(source);
    window_host_reap(&a.app->host);
    assert(step_until(a.app, process_gone, &stays_pid));
    assert(kill(moved_pid, 0) == 0);
    assert(app_tabs_count(target) == 2);
    assert(app_tabs_active(target) == moved);

    /* The exit report of the moved tab reaches its new window too. */
    write_to(moved, "\004");
    assert(step_until(a.app, has_count, &(struct count_wait){ target, 1 }));
    assert(app_tabs_active(target) != moved);

    window_close(target);
    test_app_fini(&a);
}

int main(void)
{
    moved_tab_keeps_its_session_and_reports_test();
    puts("PASS: a moved tab keeps its session and reports to its new window");
    return 0;
}
