#ifndef WSAP_TEST_SEEDS_H
#define WSAP_TEST_SEEDS_H

/* Run independent test seeds on several threads. A seed function must keep its
 * state in local or thread-local storage. WSAP_VTCORE_TEST_THREADS sets the
 * thread count. The default is one thread for each online CPU. */

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define SEEDS_MAX_THREADS 64

/* Return true when the seed passes. */
typedef bool (*seed_fn)(int seed);

struct seed_pool {
    seed_fn fn;
    int end;
    atomic_int next;
    atomic_int failed;
};

static void *seed_worker(void *arg)
{
    struct seed_pool *pool = arg;
    for (;;) {
        int seed = atomic_fetch_add(&pool->next, 1);
        // Seeds start in order. Seeds below a failed seed always run to the end.
        if (seed >= pool->end || seed > atomic_load(&pool->failed))
            return NULL;
        if (pool->fn(seed))
            continue;
        int low = atomic_load(&pool->failed);
        while (seed < low && !atomic_compare_exchange_weak(&pool->failed, &low, seed)) {
        }
    }
}

static int seed_threads(void)
{
    const char *env = getenv("WSAP_VTCORE_TEST_THREADS");
    long n = env ? atol(env) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    return n > SEEDS_MAX_THREADS ? SEEDS_MAX_THREADS : (int)n;
}

/* Run fn for each seed from first to first + count - 1 on the given number of
 * threads. Zero threads picks the default. Return the lowest seed that fails,
 * or -1. A serial run would stop at the same seed. */
static int run_seeds(seed_fn fn, int first, int count, int threads)
{
    struct seed_pool pool = { .fn = fn, .end = count > INT_MAX - first ? INT_MAX : first + count };
    atomic_init(&pool.next, first);
    atomic_init(&pool.failed, INT_MAX);
    pthread_t tid[SEEDS_MAX_THREADS];
    if (threads <= 0)
        threads = seed_threads();
    if (threads > SEEDS_MAX_THREADS)
        threads = SEEDS_MAX_THREADS;
    int started = 0;
    while (started < threads - 1 && pthread_create(&tid[started], NULL, seed_worker, &pool) == 0)
        started++;
    seed_worker(&pool);
    for (int i = 0; i < started; i++)
        pthread_join(tid[i], NULL);
    int failed = atomic_load(&pool.failed);
    return failed == INT_MAX ? -1 : failed;
}

#endif
