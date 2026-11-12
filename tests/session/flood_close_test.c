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

static void on_signal(void *ud, int signo)
{
    (void)ud;
    if (signo == SIGCHLD)
        session_reap_exited();
}

int main(void)
{
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    assert(yasl_loop_watch_signals(loop, (const int[]){SIGCHLD}, 1, on_signal, NULL) == 0);

    const char *command = "yes '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'";

    for (int iter = 0; iter < 100; iter++) {
        static alignas(64) struct session s;
        bool ok = session_init(&s, loop, &(struct session_spec){
            .command = command,
            .grid = { .x = 80, .y = 24 },
            .scrollback = 1000,
        });
        assert(ok);

        /* Let the worker thread actively pump and parse data for 2 ms. */
        struct timespec pause_ts = { .tv_sec = 0, .tv_nsec = 2000000 };
        nanosleep(&pause_ts, NULL);
        assert(yasl_loop_step(loop, 0) >= 0);

        /* Close and destroy the session while the background worker is actively flooding. */
        session_fini(&s);
    }

    yasl_loop_unwatch_signals(loop);
    assert(yasl_loop_destroy(loop) == 0);
    puts("PASS: 100 repeated session closes during active flood completed cleanly");
    return 0;
}
