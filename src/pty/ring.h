#ifndef PTY_RING_H
#define PTY_RING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "yasl/yasl.h"
#include "pty/pty.h"

#define PTY_RING_SIZE (4 * 1024 * 1024)
#define PTY_RING_MASK (PTY_RING_SIZE - 1)

#define PTY_DRAIN_BUDGET_NS 16000000ull

/* Source switches the ring can hold before the consumer passes them. */
#define PTY_RING_SWITCHES 64

struct pty_ring_cbs {
    /* `fd` is the transport the bytes came from: the fast pipe or the
     * pty master. One call never mixes the two. */
    void (*data)(void *ud, int fd, const char *buf, size_t len,
                 size_t backlog);
    void (*drained)(void *ud);
    /* After pty_ring_end(): what the transports held then has gone to
     * data. Runs on the loop thread, last in its callback, so it may stop
     * the ring. */
    void (*ended)(void *ud);
};

/* One worker thread reads every output transport of a pty into one
 * ring, so the ring order is the order the worker read them in. The
 * sync socket lets a writer that changes transport wait until the worker
 * has drained the transport it leaves. */
struct pty_ring {
    char *buf;
    struct yasl_loop *loop;
    pthread_t thread;
    struct pty_ring_cbs cbs;
    void *ud;
    int src_fd[2];
    int sync_fd;
    int event_fd;
    int event_wr;
    int space_fd;
    int space_wr;
    int event_watch;
    _Atomic int stop;
    bool active;
    /* switch_at[i] is the ring offset where the source changes. Written
     * by the worker before it publishes the bytes at that offset. */
    size_t switch_at[PTY_RING_SWITCHES];
    alignas(64) _Atomic size_t head;
    _Atomic size_t switch_head;
    _Atomic int need_space;
    alignas(64) _Atomic size_t tail;
    _Atomic size_t switch_tail;
    _Atomic int awake;
    /* pty_ring_end() asked for the end. Whoever reads last publishes it in
     * end_at as a ring offset plus one: the worker after its final reads,
     * or pty_ring_end() when the worker is done. */
    _Atomic int end_requested;
    _Atomic int worker_done;
    _Atomic size_t end_at;
    bool ended;
    /* Own line so the consumer's writes do not bounce a line the worker
     * reads. */
    alignas(64) unsigned tail_src;
};

_Static_assert(offsetof(struct pty_ring, head) % 64 == 0,
               "pty_ring.head must be cacheline-aligned");
_Static_assert(offsetof(struct pty_ring, tail) % 64 == 0 &&
               offsetof(struct pty_ring, tail) !=
                   offsetof(struct pty_ring, head),
               "pty_ring.tail must live on a different cacheline than head");

/* Read every output transport of `p`: the fast pipe and the master when
 * a pipe exists, else the master alone. */
int pty_ring_start(struct pty_ring *r, struct yasl_loop *loop, const struct pty *p,
                   const struct pty_ring_cbs *cbs, void *ud);
void pty_ring_stop(struct pty_ring *r);
/* For a child that exited: read what its transports hold now, then call
 * cbs.ended once those bytes have gone to cbs.data. So the exit reaches the
 * consumer after the child's last output. */
void pty_ring_end(struct pty_ring *r);
bool pty_ring_owns_fd(const struct pty_ring *r, int fd);

#endif
