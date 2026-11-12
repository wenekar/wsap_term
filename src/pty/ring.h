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
 * which parses it. The loop thread only wakes on content and then draws it.
 * The ring keeps no copy of the output.
 */
struct pty_ring_cbs {
    /* Processes a chunk of reader data on the worker thread. */
    void (*data)(void *ud, int fd, const char *buf, size_t len);
    /* Notifies the event loop thread of pending updates. */
    void (*wake)(void *ud);
    /* Notifies the event loop thread that reader stream ended. */
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
    /* The worker signals the loop thread: new content, a host event, or the
     * end of the output. */
    int event_fd;
    int event_wr;
    /* The loop thread asks the worker to run: an end request, or a stop. */
    int request_fd;
    int request_wr;
    int event_watch;
    _Atomic int stop;
    /* Set by the worker before it signals, cleared by the loop thread when it
     * drains the signal. A zero value sends the next signal. */
    _Atomic int wake_pending;
    /* The loop thread asks for the end of the output. The worker drains the
     * pty and then publishes the end. */
    _Atomic int end_requested;
    _Atomic int worker_done;
    _Atomic int end_published;
    bool active;
    /* The loop thread delivered the end callback. */
    bool ended_delivered;
};

/*
 * Starts the reader worker thread and registers event loop watches for `p`.
 * Returns 0 on success, or -1 on failure.
 */
int pty_ring_start(struct pty_ring *r, struct yasl_loop *loop, const struct pty *p,
                   const struct pty_ring_cbs *cbs, void *ud);

/* Stops and joins the worker thread and releases ring resources. */
void pty_ring_stop(struct pty_ring *r);

/* Requests the worker thread to drain remaining output and signal completion. */
void pty_ring_end(struct pty_ring *r);

/*
 * Signals the event loop to process available reader data.
 * Safe to call from any thread.
 */
void pty_ring_wake(struct pty_ring *r);

#endif
