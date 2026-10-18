#define _DEFAULT_SOURCE
#include "render/cpu/pool.h"
#include "render/render.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * The worker pool (render/cpu/pool.h).
 * - pool_each() calls its function once on each thread of the pool, each
 *   time on another thread, and returns after the last call.
 * - Any order of pool_each() and pool_run() calls loses no wake: each call
 *   returns, and each pool_run() covers every item once. Some rounds wait
 *   until the workers sleep, so both the poll and the sleep paths run.
 */

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define ITEMS 257
#define ROUNDS 300

struct each_log {
    _Atomic int calls;
    _Atomic int bad;
    int by_worker[POOL_WORKERS_MAX + 1];
    pthread_t thread[POOL_WORKERS_MAX + 1];
};

static void each_record(void *ctx, int worker)
{
    struct each_log *l = ctx;
    atomic_fetch_add(&l->calls, 1);
    if (worker < 0 || worker > POOL_WORKERS_MAX) {
        atomic_store(&l->bad, 1);
        return;
    }
    /* Each worker number has its own entry, so no two calls write one. */
    l->by_worker[worker]++;
    l->thread[worker] = pthread_self();
}

struct run_log {
    _Atomic int hits[ITEMS];
};

static void run_record(void *ctx, int worker, int begin, int end)
{
    struct run_log *l = ctx;
    (void)worker;
    for (int i = begin; i < end; i++)
        atomic_fetch_add(&l->hits[i], 1);
}

static void check_each(struct pool *p, int workers, const char *what)
{
    struct each_log l = {0};
    pool_each(p, each_record, &l);
    CHECK(!atomic_load(&l.bad), "%s: a worker number out of range", what);
    CHECK(atomic_load(&l.calls) == workers + 1, "%s: %d calls, want %d", what,
          atomic_load(&l.calls), workers + 1);
    CHECK(pthread_equal(l.thread[0], pthread_self()), "%s: worker 0 is not the main thread",
          what);
    for (int i = 0; i <= workers; i++) {
        CHECK(l.by_worker[i] == 1, "%s: worker %d ran %d times", what, i, l.by_worker[i]);
        for (int j = 0; j < i; j++)
            CHECK(!pthread_equal(l.thread[i], l.thread[j]), "%s: workers %d and %d share a thread",
                  what, j, i);
    }
}

static void check_run(struct pool *p, int grain, const char *what)
{
    struct run_log l = {0};
    pool_run(p, ITEMS, grain, run_record, &l);
    for (int i = 0; i < ITEMS; i++)
        CHECK(atomic_load(&l.hits[i]) == 1, "%s: item %d ran %d times", what, i,
              atomic_load(&l.hits[i]));
}

/* Long enough for each worker to stop its poll and sleep. */
static void let_workers_sleep(void)
{
    struct timespec t = { .tv_nsec = 200000 };
    nanosleep(&t, NULL);
}

static void pool_case(int want)
{
    struct pool *p = pool_create(want);
    CHECK(p, "pool of %d workers", want);
    int workers = pool_workers(p);
    CHECK(workers == want, "%d workers started, want %d", workers, want);
    char what[64];
    for (int round = 0; round < ROUNDS; round++) {
        snprintf(what, sizeof(what), "%d workers, round %d", workers, round);
        if (round % 7 == 0)
            let_workers_sleep();
        switch (round % 3) {
        case 0:
            check_each(p, workers, what);
            check_each(p, workers, what);
            break;
        case 1:
            check_each(p, workers, what);
            check_run(p, 1, what);
            break;
        case 2:
            check_run(p, 4, what);
            check_each(p, workers, what);
            break;
        }
    }
    pool_destroy(p);
}

int main(void)
{
    CHECK(render_init(), "render_init");
    /* No pool: only the main thread calls fn. */
    check_each(NULL, 0, "no pool");
    static const int counts[] = { 0, 1, 3, POOL_WORKERS_MAX };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
        pool_case(counts[i]);
    render_shutdown();
    printf("render-pool-test: ok\n");
    return 0;
}
