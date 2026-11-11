#define _GNU_SOURCE

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/tabs/tabs.h"
#include "helpers/app_fixture.h"
#include "session/session.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"

struct fixture {
    struct test_app app;
    struct window *opened;
};

static struct window *open_window(void *ud, app_tab_id tab, const char *cwd,
                                  struct vector2i size)
{
    struct fixture *f = ud;
    struct window *w = window_create(&f->app.app->host,
                                     &(struct window_spec){ .size = size });
    assert(w);
    assert(app_tabs_mount(w, &(struct app_tabs_spec){
        .config = &f->app.app->config.terminal,
        .model = &f->app.app->tabs,
        .tab = tab,
        .cwd = cwd,
        .open_window = open_window,
        .ud = f,
    }));
    f->opened = w;
    return w;
}

static void expect_cwd(struct window *w, const char *want)
{
    struct session *session = terminal_session(app_tabs_active(w));
    char got[PATH_MAX];
    uint64_t end = yasl_time_ns() + 2000000000ull;
    do {
        if (session_cwd(session, got, sizeof(got)) && !strcmp(got, want))
            return;
        assert(yasl_loop_step(window_host(w)->loop, 10) >= 0);
    } while (yasl_time_ns() < end);
    assert(!"the terminal did not start in the requested directory");
}

int main(void)
{
    struct fixture f = {0};
    test_app_init(&f.app, "/bin/cat", (struct vector2i){ .x = 800, .y = 600 });
    char path[] = "/tmp/wsap-tabs-cwd-XXXXXX";
    assert(mkdtemp(path));
    char resolved[PATH_MAX];
    assert(realpath(path, resolved));

    struct window *first = open_window(&f, 0, resolved,
                                       (struct vector2i){ .x = 800, .y = 600 });
    expect_cwd(first, resolved);
    assert(app_tabs_new(first));
    expect_cwd(first, resolved);

    assert(app_tabs_new_window(first));
    assert(f.opened != first);
    expect_cwd(f.opened, resolved);

    test_app_fini(&f.app);
    assert(rmdir(path) == 0);
    puts("PASS: new tabs and windows keep the requested directory");
    return 0;
}
