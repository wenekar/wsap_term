#ifndef RENDER_CPU_POOL_H
#define RENDER_CPU_POOL_H

/*
 * The worker pool of one renderer.
 *
 * - The renderer creates the pool at the first CPU frame that has
 *   POOL_SERIAL_CELLS dirty cells or more. It destroys the pool in
 *   renderer_destroy(). There is no global pool.
 * - pool_run() splits n items into chunks of `grain`. The main thread works
 *   too. Each thread claims the next chunk with one atomic add on an item
 *   counter.
 * - The join waits until every claimed item is done. It does not wait for
 *   every worker to wake up. A late worker finds no item and sleeps again.
 *   A late worker never runs an item of a finished job.
 * - Workers wait and wake with a futex on Linux and a condition variable on
 *   other systems. After a job, a worker polls for the next job for a short
 *   time before it sleeps. On Linux, a wake makes a system call only when a
 *   worker sleeps. Workers block every signal.
 * - Steady state allocates nothing.
 * - pool_each() runs a function once on every thread of the pool. The
 *   renderer uses it to size the thread-local scratch of every thread
 *   before a pool_run(), because timing decides which thread runs which
 *   item.
 *
 * Font protocol (docs/font-api.md, "Thread affinity"). Each worker:
 * - calls font_thread_attach() when it starts;
 * - calls glyph_cache_flush_deferred() and font_thread_attach() at the start
 *   of each job that it takes part in;
 * - calls render_thread_scratch_release(), glyph_cache_thread_detach(), and
 *   font_thread_detach() before it exits.
 * The main thread turns on glyph_cache_set_parallel(true) before pool_run()
 * and turns it off after. It calls glyph_cache_flush_frame() after the UI
 * pass. The pool does not do those calls.
 */

/* The most workers that a pool has. */
#define POOL_WORKERS_MAX 15

/*
 * render_draw() runs the raster on the main thread alone when the frame
 * has fewer dirty cells than this. It passes grain = n then.
 *
 * The value is measured. A plain cell costs about 15 ns on one core. Most
 * of it is the stores to the canvas. At 1024 cells the frame takes about
 * 15 us, and the workers then save more time than the wake costs. A
 * frame of one or two rows stays serial.
 */
#define POOL_SERIAL_CELLS 1024

struct pool;

/*
 * Runs `fn` over items [0, n) in chunks of `grain`. Items can run on the
 * calling thread and on the pool workers at the same time. In
 * `fn(ctx, worker, begin, end)`, `worker` is the thread index: 0 for the
 * calling thread and one id per worker. Returns after every item ran. A
 * call from a worker thread runs its items on that thread.
 */
typedef void (*pool_fn)(void *ctx, int worker, int begin, int end);
void pool_run(struct pool *p, int n, int grain, pool_fn fn, void *ctx);

/*
 * Runs `fn` once on each worker thread and once on the calling thread.
 * Waits for all executions to complete.
 */
typedef void (*pool_each_fn)(void *ctx, int worker);
void pool_each(struct pool *p, pool_each_fn fn, void *ctx);

/* Creates a pool of up to `workers` worker threads. A count below 0 counts
 * as 0, and POOL_WORKERS_MAX bounds a larger count. Returns the pool, or
 * NULL when it cannot be allocated or initialized. A pool whose threads
 * failed to start is valid and has fewer workers; pool_workers() gives the
 * count. */
struct pool *pool_create(int workers);

/* Stops and joins all worker threads in pool `p` and frees pool memory. */
void pool_destroy(struct pool *p);

/* Returns the number of worker threads in pool `p`, or 0 for NULL. */
int pool_workers(const struct pool *p);

/*
 * Returns the default worker count. WSAP_RENDER_THREADS sets it when it
 * holds a non-negative integer; a larger value is clamped to
 * POOL_WORKERS_MAX. An invalid value prints one warning and is ignored.
 * Else the count is one less than the online processor count, at most 3
 * and at least 0.
 */
int pool_default_workers(void);

#endif
