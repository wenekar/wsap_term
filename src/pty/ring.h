#ifndef PTY_RING_H
#define PTY_RING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "yasl/yasl.h"
#include "pty/pty.h"

/*
 * The worker thread reads the pty and hands each chunk to the data callback,
 * which parses it. The loop thread wakes when content is ready and draws it.
 * The ring keeps no copy of the output.
 */
struct pty_ring_cbs {
    /* Processes a chunk of reader data on the worker thread. The buffer is
     * valid only until the callback returns. */
    void (*data)(void *ud, int fd, const char *buf, size_t len);
    /* Notifies the event loop thread of pending updates. */
    void (*wake)(void *ud);
    /* Notifies the event loop thread that the reader stream ended. */
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
    /* Signals the loop thread: new content, a host event, or the end of the
     * output. */
    int event_fd;
    int event_wr;
    /* The loop thread sends an end request or a stop to the worker. */
    int request_fd;
    int request_wr;
    int event_watch;
    _Atomic int stop;
    /* Set by the signaling thread before it signals and cleared by the loop
     * thread when it drains the signal. A zero value permits the next
     * signal. */
    _Atomic int wake_pending;
    /* The loop thread asks for the end of the output. The worker drains the
     * sources and then publishes the end. */
    _Atomic int end_requested;
    _Atomic int worker_done;
    _Atomic int end_published;
    bool active;
    bool ended_delivered;
};

/*
 * Starts the reader worker thread and registers the ring's event-loop watch.
 * The ring copies `cbs`; `ud` must stay valid until pty_ring_stop returns.
 * Returns 0 on success, or -1 on failure.
 */
int pty_ring_start(struct pty_ring *r, struct yasl_loop *loop, const struct pty *p,
                   const struct pty_ring_cbs *cbs, void *ud);

/* Stops and joins the worker thread and releases ring resources. */
void pty_ring_stop(struct pty_ring *r);

/* Requests that the worker drain the remaining output and publish the end. */
void pty_ring_end(struct pty_ring *r);

/*
 * Signals the event loop to process pending updates.
 * Safe to call from any thread.
 */
void pty_ring_wake(struct pty_ring *r);

#endif
