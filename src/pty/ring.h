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

#define PTY_RING_SWITCHES 64

struct pty_ring_cbs {
    void (*data)(void *ud, int fd, const char *buf, size_t len,
                 size_t backlog);
    void (*drained)(void *ud);
    void (*ended)(void *ud);
};

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
    size_t switch_at[PTY_RING_SWITCHES];
    alignas(64) _Atomic size_t head;
    _Atomic size_t switch_head;
    _Atomic int need_space;
    alignas(64) _Atomic size_t tail;
    _Atomic size_t switch_tail;
    _Atomic int awake;
    _Atomic int end_requested;
    _Atomic int worker_done;
    _Atomic size_t end_at;
    bool ended;
    alignas(64) unsigned tail_src;
};

_Static_assert(offsetof(struct pty_ring, head) % 64 == 0,
               "pty_ring.head must be cacheline-aligned");
_Static_assert(offsetof(struct pty_ring, tail) % 64 == 0 &&
               offsetof(struct pty_ring, tail) !=
                   offsetof(struct pty_ring, head),
               "pty_ring.tail must live on a different cacheline than head");

int pty_ring_start(struct pty_ring *r, struct yasl_loop *loop, const struct pty *p,
                   const struct pty_ring_cbs *cbs, void *ud);
void pty_ring_stop(struct pty_ring *r);
void pty_ring_end(struct pty_ring *r);
bool pty_ring_owns_fd(const struct pty_ring *r, int fd);

#endif
