#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session/session.h"
#include "util/time.h"
#include "yasl/yasl.h"

enum event_type {
    EV_NONE = 0,
    EV_TITLE,
    EV_CLIPBOARD,
    EV_BELL,
    EV_OUTPUT,
    EV_EXITED,
};

struct test_recorder {
    struct session *session;
    struct yasl_loop *loop;
    char title[64];
    char clipboard[64];
    bool got_bell;
    bool got_output;
    bool got_exited;
    enum event_type order[16];
    int order_count;
};

static void record_event(struct test_recorder *r, enum event_type ev)
{
    if (r->order_count < (int)(sizeof(r->order) / sizeof(r->order[0])))
        r->order[r->order_count++] = ev;
}

static void on_title(void *ud)
{
    struct test_recorder *r = ud;
    const char *title = session_title(r->session);
    snprintf(r->title, sizeof(r->title), "%s", title ? title : "");
    record_event(r, EV_TITLE);
}

static void on_clipboard(void *ud, const char *text)
{
    struct test_recorder *r = ud;
    snprintf(r->clipboard, sizeof(r->clipboard), "%s", text ? text : "");
    record_event(r, EV_CLIPBOARD);
}

static void on_bell(void *ud)
{
    struct test_recorder *r = ud;
    r->got_bell = true;
    record_event(r, EV_BELL);
}

static void on_output(void *ud)
{
    struct test_recorder *r = ud;
    r->got_output = true;
    record_event(r, EV_OUTPUT);
}

static void on_exited(void *ud)
{
    struct test_recorder *r = ud;
    r->got_exited = true;
    record_event(r, EV_EXITED);
    yasl_loop_quit(r->loop);
}

static const struct session_host HOST = {
    .output = on_output,
    .title = on_title,
    .clipboard = on_clipboard,
    .bell = on_bell,
    .exited = on_exited,
};

static void on_signal(void *ud, int signo)
{
    (void)ud;
    if (signo == SIGCHLD)
        session_reap_exited();
}

static uint64_t on_timeout(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    yasl_loop_quit(ud);
    return 0;
}

int main(void)
{
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    assert(yasl_loop_watch_signals(loop, (const int[]){SIGCHLD}, 1, on_signal, NULL) == 0);

    char da1_path[] = "/tmp/wsap-da1-reply-XXXXXX";
    int da1_fd = mkstemp(da1_path);
    assert(da1_fd >= 0);
    close(da1_fd);

    char script[] = "/tmp/wsap-cb-order-XXXXXX";
    int fd = mkstemp(script);
    assert(fd >= 0);

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "stty -icanon min 1 time 0 2>/dev/null || stty raw -echo 2>/dev/null\n"
        "printf '\\033]0;TestTitle\\007\\033]52;c;SGVsbG8=\\007\\007\\033[c'\n"
        "dd count=1 bs=16 2>/dev/null > %s\n"
        "exit 0\n", da1_path);
    assert(write(fd, body, (size_t)body_len) == body_len);
    close(fd);

    char command[128];
    snprintf(command, sizeof(command), "/bin/sh %s", script);

    static alignas(64) struct session s;
    struct test_recorder r = { .session = &s, .loop = loop };
    assert(session_init(&s, loop, &(struct session_spec){
        .command = command,
        .grid = { .x = 80, .y = 24 },
    }));
    session_set_host(&s, &HOST, &r);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(10000), on_timeout, loop));
    assert(yasl_loop_run(loop) == 0);

    unlink(script);

    assert(r.got_exited);
    assert(strcmp(r.title, "TestTitle") == 0);
    assert(strcmp(r.clipboard, "Hello") == 0);
    assert(r.got_bell);
    assert(r.got_output);

    FILE *f = fopen(da1_path, "rb");
    assert(f);
    char reply[64] = {0};
    size_t nr = fread(reply, 1, sizeof(reply) - 1, f);
    fclose(f);
    unlink(da1_path);

    assert(nr > 0);
    assert(strstr(reply, "\033[?") != NULL);
    assert(strchr(reply, 'c') != NULL);

    int exit_idx = -1;
    for (int i = 0; i < r.order_count; i++) {
        if (r.order[i] == EV_EXITED) {
            exit_idx = i;
            break;
        }
    }
    assert(exit_idx >= 0);

    bool title_before_exit = false;
    bool clip_before_exit = false;
    bool bell_before_exit = false;
    for (int i = 0; i < exit_idx; i++) {
        if (r.order[i] == EV_TITLE) title_before_exit = true;
        if (r.order[i] == EV_CLIPBOARD) clip_before_exit = true;
        if (r.order[i] == EV_BELL) bell_before_exit = true;
    }
    assert(title_before_exit);
    assert(clip_before_exit);
    assert(bell_before_exit);

    session_fini(&s);
    yasl_loop_unwatch_signals(loop);
    assert(yasl_loop_destroy(loop) == 0);
    puts("PASS: compound write delivered title, clipboard, bell, DA1 reply, and exited in order");
    return 0;
}
