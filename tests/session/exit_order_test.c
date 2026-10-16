#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session/session.h"
#include "util/time.h"
#include "yasl/yasl.h"

struct watch {
    struct session *session;
    struct yasl_loop *loop;
    bool exited;
    char title_at_exit[16];
};

static void on_exited(void *ud)
{
    struct watch *w = ud;
    const char *title = session_title(w->session);
    snprintf(w->title_at_exit, sizeof(w->title_at_exit), "%s", title ? title : "");
    w->exited = true;
    yasl_loop_quit(w->loop);
}

static void on_title(void *ud)
{
    (void)ud;
    usleep(100);
}

static const struct session_host HOST = { .title = on_title, .exited = on_exited };

static void on_signal(void *ud, int signo)
{
    (void)ud;
    if (signo == SIGCHLD)
        session_reap_exited();
}

static uint64_t on_timeout(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    yasl_loop_quit(ud);
    return 0;
}

int main(void)
{
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    assert(yasl_loop_watch_signals(loop, (const int[]){SIGCHLD}, 1, on_signal, NULL) == 0);

    char script[] = "/tmp/wsap-exit-order-XXXXXX";
    int fd = mkstemp(script);
    assert(fd >= 0);
    static const char body[] =
        "yes \"$(printf '\\033]0;x\\007')\" | head -n 10000\n"
        "printf '\\033]0;END\\007'\n";
    assert(write(fd, body, sizeof(body) - 1) == (ssize_t)(sizeof(body) - 1));
    close(fd);
    char command[64];
    snprintf(command, sizeof(command), "/bin/sh %s", script);

    static alignas(64) struct session s;
    struct watch w = { .session = &s, .loop = loop };
    assert(session_init(&s, loop, &(struct session_spec){
        .command = command,
        .grid = { .x = 80, .y = 24 },
    }));
    session_set_host(&s, &HOST, &w);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(20000), on_timeout, loop));
    assert(yasl_loop_run(loop) == 0);
    unlink(script);

    if (!w.exited) {
        fprintf(stderr, "FAIL: the child never exited\n");
        return 1;
    }
    if (strcmp(w.title_at_exit, "END")) {
        fprintf(stderr, "FAIL: the exit arrived with title '%s'; the last output "
                        "must reach the terminal first\n", w.title_at_exit);
        return 1;
    }
    session_fini(&s);
    yasl_loop_unwatch_signals(loop);
    assert(yasl_loop_destroy(loop) == 0);
    puts("PASS: the last output reached the terminal before the exit");
    return 0;
}
