#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session/session.h"
#include "util/time.h"
#include "yasl/yasl.h"

struct capture {
    struct session *session;
    struct yasl_loop *loop;
    struct wsap_vtcore_snapshot *snapshot;
    bool found;
    bool ended;
};

static void on_output(void *ud)
{
    struct capture *c = ud;
    assert(wsap_vtcore_snapshot_update(c->snapshot, c->session->term));
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(c->snapshot);
    static const char marker[] = "QUIET";
    size_t matched = 0;
    for (int y = 0; y < view->size.y; y++) {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(c->snapshot, y);
        for (int x = 0; x < view->size.x; x++) {
            if (row[x].text == (unsigned char)marker[matched])
                matched++;
            else
                matched = row[x].text == marker[0] ? 1 : 0;
            if (matched == sizeof(marker) - 1) {
                c->found = true;
                yasl_loop_quit(c->loop);
                return;
            }
        }
    }
}

static void on_ended(void *ud)
{
    struct capture *c = ud;
    c->ended = true;
    yasl_loop_quit(c->loop);
}

static uint64_t on_timeout(void *ud, uint64_t id, uint64_t delay)
{
    (void)id;
    (void)delay;
    yasl_loop_quit(ud);
    return 0;
}

static void write_all(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        assert(n > 0);
        data += n;
        len -= (size_t)n;
    }
}

int main(void)
{
    char path[] = "/tmp/wsap-idle-output-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    char block[65536];
    memset(block, 'x', sizeof(block));
    for (int i = 0; i < 31; i++)
        write_all(fd, block, sizeof(block));
    memcpy(block + sizeof(block) - 5, "QUIET", 5);
    write_all(fd, block, sizeof(block));
    close(fd);

    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    char command[256];
    snprintf(command, sizeof(command), "/bin/sh -c 'cat %s; read value'", path);
    struct session session;
    assert(session_init(&session, loop, &(struct session_spec){
        .command = command, .grid = { .x = 80, .y = 24 }, .scrollback = 1000,
    }));
    struct capture capture = {
        .session = &session, .loop = loop,
        .snapshot = wsap_vtcore_snapshot_new(),
    };
    assert(capture.snapshot);
    const struct session_host host = { .output = on_output, .exited = on_ended };
    session_set_host(&session, &host, &capture);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(5000), on_timeout, loop));
    assert(yasl_loop_run(loop) == 0);
    assert(capture.found && !capture.ended);
    session_fini(&session);
    wsap_vtcore_snapshot_free(capture.snapshot);
    assert(yasl_loop_destroy(loop) == 0);
    unlink(path);
    puts("PASS: a large burst reaches the screen before the child exits");
    return 0;
}
