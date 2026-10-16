#define _GNU_SOURCE
#include "yasl/yasl.h"
#include "util/time.h"
#include "pty/pty.h"
#include "pty/ring.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROUNDS 200
#define BURST 32768
#define TIMEOUT_MS 20000
#define CONSUME_DELAY_US 100

struct capture {
    struct yasl_loop *loop;
    const struct pty *pty;
    char *bytes;
    char *source;
    size_t len;
    size_t cap;
};

static void on_data(void *ud, int fd, const char *buf, size_t len,
                    size_t backlog)
{
    (void)backlog;
    struct capture *c = ud;
    /* A slow consumer, like a render-bound terminal, keeps the ring full.
     * Both transports then have data pending at the same time, which is
     * when a switch without a barrier is read out of order. */
    usleep(CONSUME_DELAY_US);
    assert(c->len + len <= c->cap);
    char tag = fd == pty_pipe_fd(c->pty) ? 'p' : fd == pty_master_fd(c->pty) ? 't' : '?';
    memcpy(c->bytes + c->len, buf, len);
    memset(c->source + c->len, tag, len);
    c->len += len;
    if (c->len >= 3 && !memcmp(c->bytes + c->len - 3, "END", 3))
        yasl_loop_quit(c->loop);
}

static uint64_t on_timeout(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    yasl_loop_quit(ud);
    return 0;
}

/* Build the stream the helper writes, with the transport each part must
 * use: bursts and A markers on the fast pipe, B markers on the pty. */
static size_t expected_stream(char *bytes, char *source)
{
    size_t n = 0;
    for (int i = 0; i < ROUNDS; i++) {
        memset(bytes + n, 'a', BURST);
        memset(source + n, 'p', BURST);
        n += BURST;
        int w = sprintf(bytes + n, "A%d.", i);
        memset(source + n, 'p', (size_t)w);
        n += (size_t)w;
        w = sprintf(bytes + n, "B%d.", i);
        memset(source + n, 't', (size_t)w);
        n += (size_t)w;
    }
    memcpy(bytes + n, "END", 3);
    memset(source + n, 'p', 3);
    return n + 3;
}

static size_t first_diff(const char *a, const char *b, size_t n)
{
    size_t i = 0;
    while (i < n && a[i] == b[i])
        i++;
    return i;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    setenv("LD_PRELOAD", argv[1], 1);
    setenv("WSAP_FAST_PIPE", "1", 1);

    size_t cap = (size_t)ROUNDS * (BURST + 32) + 16;
    char *want = malloc(cap), *want_src = malloc(cap);
    struct capture c = {
        .bytes = malloc(cap),
        .source = malloc(cap),
        .cap = cap,
        .loop = yasl_loop_create(),
    };
    assert(want && want_src && c.bytes && c.source && c.loop);
    size_t want_len = expected_stream(want, want_src);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s probe-route %d", argv[2], ROUNDS);
    struct pty p = {0};
    assert(pty_spawn(&p, &(struct pty_spec){
        .grid = { .x = 80, .y = 24 },
        .command = cmd,
    }) == 0);
    assert(pty_pipe_fd(&p) >= 0);
    c.pty = &p;

    struct pty_ring ring;
    const struct pty_ring_cbs cbs = { .data = on_data };
    assert(pty_ring_start(&ring, c.loop, &p, &cbs, &c) == 0);
    yasl_loop_add_timer(c.loop, MS_TO_NS(TIMEOUT_MS), on_timeout, c.loop);
    assert(yasl_loop_run(c.loop) == 0);
    pty_ring_stop(&ring);
    pty_destroy(&p);

    if (c.len != want_len || memcmp(c.bytes, want, want_len)) {
        size_t at = first_diff(c.bytes, want, c.len < want_len ? c.len : want_len);
        fprintf(stderr, "FAIL: stream order: got %zu bytes, want %zu, first "
                "difference at %zu: \"%.12s\"\n", c.len, want_len, at,
                c.bytes + at);
        return 1;
    }
    if (memcmp(c.source, want_src, want_len)) {
        size_t at = first_diff(c.source, want_src, want_len);
        fprintf(stderr, "FAIL: transport: byte %zu \"%.8s\" came from %c, "
                "want %c\n", at, c.bytes + at, c.source[at], want_src[at]);
        return 1;
    }
    puts("PASS: raw-mode output uses the pty, cooked output the fast pipe, "
         "and the stream stays in program order across switches");
    yasl_loop_destroy(c.loop);
    free(want);
    free(want_src);
    free(c.bytes);
    free(c.source);
    return 0;
}
