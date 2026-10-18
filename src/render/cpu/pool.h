#ifndef RENDER_CPU_POOL_H
#define RENDER_CPU_POOL_H

/*
 * The worker pool of one renderer.
 *
 * - The renderer creates the pool in renderer_create() and destroys it in
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
 *
 * Font protocol (docs/font-api.md, "Thread affinity"). Each worker:
 * - calls font_thread_attach() when it starts;
 * - calls glyph_cache_flush_deferred() and font_thread_attach() at the start
 *   of each job that it takes part in;
 * - calls render_walk_thread_release(), glyph_cache_thread_detach(), and
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
 * Runs fn(ctx, worker, begin, end) for chunks of the items [0, n).
 * - `worker` is 0 on the main thread and 1..pool_workers(p) on the workers.
 *   Use it to pick a per-worker context.
 * - Chunks do not overlap. Together they cover [0, n) once.
 * - `grain` < 1 means 1. With n <= grain, with no workers, or with p NULL,
 *   the main thread runs everything and wakes no worker.
 * - Main thread only. One run at a time. `fn` must not call pool_run().
 *   If it does, that call runs all of its items on the calling thread,
 *   with the worker number of that thread.
 */
typedef void (*pool_fn)(void *ctx, int worker, int begin, int end);
void pool_run(struct pool *p, int n, int grain, pool_fn fn, void *ctx);

/*
 * Starts a pool with `workers` threads, clamped to [0, POOL_WORKERS_MAX].
 * A thread that fails to start is left out. It returns when each worker
 * has called font_thread_attach(). Returns NULL only when the memory is
 * missing. Main thread only.
 */
struct pool *pool_create(int workers);
/* Stops and joins the workers. NULL does nothing. Main thread only. */
void pool_destroy(struct pool *p);
/* The number of workers that run, not counting the main thread. */
int pool_workers(const struct pool *p);

/*
 * The default worker count. It is WSAP_RENDER_THREADS when that is a valid
 * count >= 0. Else it is min(nproc - 1, 3). An invalid value prints one
 * warning on stderr.
 */
int pool_default_workers(void);

#endif
