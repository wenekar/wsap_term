#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pty/pty.h"
#include "pty/ring.h"
#include "yasl/yasl.h"

/*
 * A test of the wake that the ring worker gives the loop thread.
 *
 * The worker parses a burst of short reads before it signals the loop thread,
 * so that a flood does not wake it for each read. It signals the content that
 * it holds when the pty runs dry, so a small burst still wakes the loop thread
 * at once. A signal that was already pending must not swallow the next one.
 *
 * The test reads a pipe as the pty transport of a `struct pty`, so it controls
 * every byte and every boundary without a child process. Each step of the loop
 * waits for the wake of one burst. A step that times out means that the loop
 * thread got no wake for the output that waits.
 */

#define STEP_TIMEOUT_MS 2000
/* A burst that stays below the batch of the worker. */
#define SMALL_BYTES 64u
/* A burst of the size that the kernel pty hands over in one read. */
#define CHUNK_BYTES 8192u
#define CHUNKS 8u
/* The bytes that the test keeps for each wake, by the number of the wake. It
 * reads the last wake only, so a wake beyond this count replaces the first. */
#define MAX_WAKES 32u

struct watch {
    _Atomic size_t bytes;
    _Atomic size_t wake_bytes[MAX_WAKES];
    _Atomic unsigned wakes;
};

static void on_data(void *ud, int fd, const char *buf, size_t len)
{
    (void)fd;
    (void)buf;
    struct watch *w = ud;
    atomic_fetch_add_explicit(&w->bytes, len, memory_order_relaxed);
}

static void on_wake(void *ud)
{
    struct watch *w = ud;
    unsigned n = atomic_fetch_add_explicit(&w->wakes, 1, memory_order_relaxed);
    atomic_store_explicit(&w->wake_bytes[n % MAX_WAKES],
                          atomic_load_explicit(&w->bytes, memory_order_relaxed),
                          memory_order_relaxed);
}

static size_t watch_bytes(const struct watch *w)
{
    return atomic_load_explicit(&w->bytes, memory_order_relaxed);
}

static unsigned watch_wakes(const struct watch *w)
{
    return atomic_load_explicit(&w->wakes, memory_order_relaxed);
}

static size_t wake_bytes(const struct watch *w, unsigned i)
{
    return atomic_load_explicit(&w->wake_bytes[i % MAX_WAKES],
                                memory_order_relaxed);
}

/* Writes one burst and waits for the loop thread to wake for it. */
static int burst(struct yasl_loop *loop, int fd, const char *buf, size_t len,
                 const struct watch *w, size_t expected)
{
    ssize_t n = write(fd, buf, len);
    if (n != (ssize_t)len) {
        fprintf(stderr, "FAIL: wrote %zd of %zu bytes\n", n, len);
        return 1;
    }
    if (yasl_loop_step(loop, STEP_TIMEOUT_MS) <= 0) {
        fprintf(stderr, "FAIL: no wake for %zu bytes at %zu\n", len,
                watch_bytes(w));
        return 1;
    }
    if (watch_bytes(w) != expected) {
        fprintf(stderr, "FAIL: %zu bytes parsed, expected %zu\n", watch_bytes(w),
                expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fds[2];
    assert(pipe(fds) == 0);
    /* The ring reads the pipe and polls it. It must not block in the read. */
    assert(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == 0);

    /* The read end of a pipe stands in for the pty. The handle holds the
     * descriptor plus one, and no master or sync socket exists. */
    struct pty p = { .pipe_read_handle = (unsigned)fds[0] + 1 };
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    struct watch w = { 0 };
    struct pty_ring ring;
    const struct pty_ring_cbs cbs = { .data = on_data, .wake = on_wake };
    assert(pty_ring_start(&ring, loop, &p, &cbs, &w) == 0);

    int status = 0;
    char small[SMALL_BYTES];
    memset(small, 'a', sizeof small);
    static char chunk[CHUNK_BYTES];
    memset(chunk, 'b', sizeof chunk);
    size_t total = 0;

    /* A small burst wakes the loop thread without a flood behind it. */
    if (burst(loop, fds[1], small, sizeof small, &w, total += sizeof small))
        status = 1;
    /* A burst after a wake wakes the loop thread too, so the first wake left
     * the ring able to signal again. */
    if (!status && burst(loop, fds[1], small, sizeof small, &w,
                         total += sizeof small))
        status = 1;

    /* A stream wakes the loop thread for each part of it. */
    for (unsigned i = 0; !status && i < CHUNKS; i++) {
        if (burst(loop, fds[1], chunk, sizeof chunk, &w, total += sizeof chunk))
            status = 1;
    }

    /* A small burst at the end of the stream wakes the loop thread, so the
     * worker did not hold its signal back for the batch that never fills. */
    if (!status && burst(loop, fds[1], small, sizeof small, &w,
                         total += sizeof small))
        status = 1;

    unsigned wakes = watch_wakes(&w);
    if (!status && wakes < CHUNKS + 3) {
        fprintf(stderr, "FAIL: %u wakes for %u bursts\n", wakes, CHUNKS + 3);
        status = 1;
    }
    /* The last wake came after the last byte, so no content waits for a
     * signal. */
    if (!status && wake_bytes(&w, wakes - 1) != total) {
        fprintf(stderr, "FAIL: the last wake saw %zu of %zu bytes\n",
                wake_bytes(&w, wakes - 1), total);
        status = 1;
    }
    if (!status)
        printf("PASS: %u wakes carried %zu bytes, and the last burst woke the "
               "loop thread with all of them\n", wakes, total);

    pty_ring_stop(&ring);
    yasl_loop_destroy(loop);
    close(fds[0]);
    close(fds[1]);
    return status;
}
