#define _GNU_SOURCE
#include "render/cpu/pool.h"

#include "font/font.h"
#include "font/glyph_cache.h"
#include "render/frame/walk.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * POOL_FORCE_CONDVAR builds the portable path on Linux too. A scratch
 * build uses it to check the path that macOS takes.
 */
#if defined(__linux__) && !defined(POOL_FORCE_CONDVAR)
#define POOL_FUTEX 1
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

/* Polls of the done counter before the main thread sleeps in the join. */
#define POOL_JOIN_SPINS 256

/*
 * Claims. `left` counts the items that no thread has claimed. It counts
 * down from n. A claim subtracts a grain. A result above 0 is a valid
 * claim of the chunk [n - left, n - left + min(grain, left)).
 *
 * A valid claim proves that the job is still open: the job cannot end
 * before the claimer reports its items in `done`. So the claimer can read
 * the job fields until it reports. A result of 0 or less claims nothing,
 * and that thread reads no job field. A late worker that subtracts after
 * the next job stored `left` makes a valid claim in that next job. So it
 * never runs an item of a finished job, and no item is lost.
 *
 * `grain` is atomic because a late worker reads it before its claim. A
 * stale grain changes only the size of one claim. The claimer splits a
 * claim that is too large into chunks of the job grain.
 */
struct pool_thread {
    struct pool *pool;
    pthread_t tid;
    int id;
};

struct pool {
    _Alignas(64) _Atomic int64_t left;
    _Alignas(64) _Atomic int done;
    _Alignas(64) _Atomic uint32_t wake;
    _Atomic bool stop;
    _Atomic int grain;
    /* The job. The main thread writes it before the release store of `left`. */
    pool_fn fn;
    void *ctx;
    int n;
    int job_grain;
    uint64_t job;
    int workers;
#ifndef POOL_FUTEX
    pthread_mutex_t mu;
    pthread_cond_t wake_cv;
    pthread_cond_t done_cv;
#endif
    struct pool_thread threads[POOL_WORKERS_MAX];
};

/* 1 + the worker number of the thread while it runs items, else 0. */
static _Thread_local int tls_worker;

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

#ifdef POOL_FUTEX
static void futex_wait(void *addr, uint32_t val)
{
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
}

static void futex_wake(void *addr, int count)
{
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}
#endif

static void wake_workers(struct pool *p, int count)
{
#ifdef POOL_FUTEX
    atomic_fetch_add_explicit(&p->wake, 1, memory_order_release);
    futex_wake(&p->wake, count);
#else
    (void)count;
    pthread_mutex_lock(&p->mu);
    atomic_fetch_add_explicit(&p->wake, 1, memory_order_release);
    pthread_cond_broadcast(&p->wake_cv);
    pthread_mutex_unlock(&p->mu);
#endif
}

/* Sleeps until `wake` differs from `seen`. Returns the new value. */
static uint32_t wait_wake(struct pool *p, uint32_t seen)
{
    uint32_t now;
#ifdef POOL_FUTEX
    while ((now = atomic_load_explicit(&p->wake, memory_order_acquire)) == seen)
        futex_wait(&p->wake, seen);
#else
    pthread_mutex_lock(&p->mu);
    while ((now = atomic_load_explicit(&p->wake, memory_order_acquire)) == seen)
        pthread_cond_wait(&p->wake_cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
#endif
    return now;
}

static void wake_main(struct pool *p)
{
#ifdef POOL_FUTEX
    futex_wake(&p->done, 1);
#else
    pthread_mutex_lock(&p->mu);
    pthread_cond_signal(&p->done_cv);
    pthread_mutex_unlock(&p->mu);
#endif
}

static void wait_done(struct pool *p, int n)
{
    for (int i = 0; i < POOL_JOIN_SPINS; i++) {
        if (atomic_load_explicit(&p->done, memory_order_acquire) == n)
            return;
        cpu_relax();
    }
#ifdef POOL_FUTEX
    int d;
    while ((d = atomic_load_explicit(&p->done, memory_order_acquire)) != n)
        futex_wait(&p->done, (uint32_t)d);
#else
    pthread_mutex_lock(&p->mu);
    while (atomic_load_explicit(&p->done, memory_order_acquire) != n)
        pthread_cond_wait(&p->done_cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
#endif
}

/*
 * Claims and runs chunks until none is left. Returns the items that it
 * ran. When it ran some, *n gets the item count of the job. A worker
 * passes `job` to do the per-job font calls on its first valid claim.
 */
static int drain(struct pool *p, int id, uint64_t *job, int *n)
{
    int ran = 0;
    tls_worker = id + 1;
    for (;;) {
        int grain = atomic_load_explicit(&p->grain, memory_order_relaxed);
        if (grain < 1)
            grain = 1;
        int64_t left = atomic_fetch_sub_explicit(&p->left, grain, memory_order_acquire);
        if (left <= 0)
            break;
        if (job && *job != p->job) {
            *job = p->job;
            glyph_cache_flush_deferred();
            (void)font_thread_attach();
        }
        int begin = p->n - (int)left;
        int end = begin + (left < grain ? (int)left : grain);
        ran += end - begin;
        *n = p->n;
        while (begin < end) {
            int stop = end - begin > p->job_grain ? begin + p->job_grain : end;
            p->fn(p->ctx, id, begin, stop);
            begin = stop;
        }
    }
    tls_worker = 0;
    return ran;
}

static void *worker_main(void *arg)
{
    struct pool_thread *t = arg;
    struct pool *p = t->pool;
    (void)font_thread_attach();
    uint32_t seen = 0;
    uint64_t job = 0;
    for (;;) {
        seen = wait_wake(p, seen);
        if (atomic_load_explicit(&p->stop, memory_order_acquire))
            break;
        int n = 0;
        int ran = drain(p, t->id, &job, &n);
        if (ran && atomic_fetch_add_explicit(&p->done, ran, memory_order_acq_rel) + ran == n)
            wake_main(p);
    }
    render_walk_thread_release();
    glyph_cache_thread_detach();
    font_thread_detach();
    return NULL;
}

static void run_serial(int worker, int n, int grain, pool_fn fn, void *ctx)
{
    for (int begin = 0; begin < n;) {
        int end = n - begin > grain ? begin + grain : n;
        fn(ctx, worker, begin, end);
        begin = end;
    }
}

void pool_run(struct pool *p, int n, int grain, pool_fn fn, void *ctx)
{
    if (n <= 0 || !fn)
        return;
    if (grain < 1)
        grain = 1;
    if (tls_worker) {
        run_serial(tls_worker - 1, n, grain, fn, ctx);
        return;
    }
    if (!p || !p->workers || n <= grain) {
        tls_worker = 1;
        run_serial(0, n, grain, fn, ctx);
        tls_worker = 0;
        return;
    }

    p->fn = fn;
    p->ctx = ctx;
    p->n = n;
    p->job_grain = grain;
    p->job++;
    atomic_store_explicit(&p->grain, grain, memory_order_relaxed);
    atomic_store_explicit(&p->done, 0, memory_order_relaxed);
    atomic_store_explicit(&p->left, n, memory_order_release);

    int chunks = (n - 1) / grain + 1;
    wake_workers(p, chunks - 1 < p->workers ? chunks - 1 : p->workers);

    int unused = 0;
    int mine = drain(p, 0, NULL, &unused);
    if (atomic_fetch_add_explicit(&p->done, mine, memory_order_acq_rel) + mine != n)
        wait_done(p, n);
}

struct pool *pool_create(int workers)
{
    if (workers < 0)
        workers = 0;
    if (workers > POOL_WORKERS_MAX)
        workers = POOL_WORKERS_MAX;
    size_t size = (sizeof(struct pool) + 63) & ~(size_t)63;
    struct pool *p = aligned_alloc(64, size);
    if (!p)
        return NULL;
    memset(p, 0, size);
#ifndef POOL_FUTEX
    if (pthread_mutex_init(&p->mu, NULL)) {
        free(p);
        return NULL;
    }
    if (pthread_cond_init(&p->wake_cv, NULL)) {
        pthread_mutex_destroy(&p->mu);
        free(p);
        return NULL;
    }
    if (pthread_cond_init(&p->done_cv, NULL)) {
        pthread_cond_destroy(&p->wake_cv);
        pthread_mutex_destroy(&p->mu);
        free(p);
        return NULL;
    }
#endif
    /* A new thread inherits the mask. So the workers block every signal. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    for (int i = 0; i < workers; i++) {
        struct pool_thread *t = &p->threads[p->workers];
        t->pool = p;
        t->id = p->workers + 1;
        if (!pthread_create(&t->tid, NULL, worker_main, t))
            p->workers++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return p;
}

void pool_destroy(struct pool *p)
{
    if (!p)
        return;
    atomic_store_explicit(&p->stop, true, memory_order_release);
    wake_workers(p, INT_MAX);
    for (int i = 0; i < p->workers; i++)
        pthread_join(p->threads[i].tid, NULL);
#ifndef POOL_FUTEX
    pthread_cond_destroy(&p->done_cv);
    pthread_cond_destroy(&p->wake_cv);
    pthread_mutex_destroy(&p->mu);
#endif
    free(p);
}

int pool_workers(const struct pool *p)
{
    return p ? p->workers : 0;
}

int pool_default_workers(void)
{
    static _Atomic bool warned;
    const char *env = getenv("WSAP_RENDER_THREADS");
    if (env) {
        char *end;
        errno = 0;
        long v = strtol(env, &end, 10);
        if (!errno && end != env && !*end && v >= 0)
            return v > POOL_WORKERS_MAX ? POOL_WORKERS_MAX : (int)v;
        if (!atomic_exchange_explicit(&warned, true, memory_order_relaxed))
            fprintf(stderr, "ignoring invalid WSAP_RENDER_THREADS value: %s\n", env);
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN) - 1;
    if (n > 3)
        n = 3;
    return n > 0 ? (int)n : 0;
}
