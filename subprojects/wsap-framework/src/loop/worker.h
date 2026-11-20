#ifndef LOOP_WORKER_H
#define LOOP_WORKER_H

#include <stdbool.h>

struct yasl_loop;

/*
 * One worker thread for slow work that must not block the loop.
 * A submit queues the work and returns. The worker runs it, and the loop
 * thread then calls the done callback with the result.
 * The worker wakes the loop through a descriptor, so a late result starts the
 * next wait.
 */
struct loop_worker;

/* Runs on the worker thread and returns one result pointer. */
typedef void *(*loop_work_fn)(void *arg);

/* Runs on the loop thread after the work is done. */
typedef void (*loop_work_done_fn)(void *arg, void *result);

/*
 * Runs on the loop thread for work whose result is never delivered.
 * `result` is the pointer that `run` returned, or NULL when `run` did not
 * start. Free a heap result here.
 * A destroy drops queued work and calls this for each item that it drops.
 * It runs once for each such item.
 */
typedef void (*loop_work_cancel_fn)(void *arg, void *result);

struct loop_work {
    loop_work_fn run;
    loop_work_done_fn done;
    loop_work_cancel_fn cancel;
    void *arg;
};

/*
 * Creates a worker for `loop`. It returns NULL on failure, with errno set when
 * a system call failed.
 * The loop must outlive the worker. Destroy the worker before the loop.
 */
struct loop_worker *loop_worker_create(struct yasl_loop *loop);

/*
 * Stops the worker and frees it. Call it on the loop's thread, and not from a
 * callback of this worker. Queued work is cancelled. Running work finishes
 * first, and its result is cancelled. All cancel callbacks run on the loop
 * thread before destroy returns. A NULL worker does nothing.
 */
void loop_worker_destroy(struct loop_worker *worker);

/*
 * Queues work and wakes the worker. It does not wait for the work.
 * Any thread may call it. Returns 0, or -1 with EINVAL for a NULL worker or an
 * item whose `run`, `done`, or `cancel` is NULL, or ESHUTDOWN when the worker
 * stops.
 */
int loop_worker_submit(struct loop_worker *worker, const struct loop_work *work);

#endif
