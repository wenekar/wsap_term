#ifndef PTY_RING_H
#define PTY_RING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "yasl/yasl.h"
#include "pty/pty.h"

#define PTY_DRAIN_BUDGET_NS 16000000ull

struct pty_ring_cbs {
    /* The reader worker calls data and drained. */
    void (*data)(void *ud, int fd, const char *buf, size_t len,
                 size_t backlog);
    void (*drained)(void *ud);
    void (*ended)(void *ud);
};

struct pty_ring {
    /* Only the worker reads or writes this buffer. */
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
    _Atomic int end_requested;
    _Atomic int worker_done;
    bool ended;
};

int pty_ring_start(struct pty_ring *r, struct yasl_loop *loop, const struct pty *p,
                   const struct pty_ring_cbs *cbs, void *ud);
void pty_ring_stop(struct pty_ring *r);
void pty_ring_end(struct pty_ring *r);
bool pty_ring_owns_fd(const struct pty_ring *r, int fd);

#endif
