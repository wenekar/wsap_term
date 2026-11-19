#define _GNU_SOURCE

#include "loop/worker.h"

#include "yasl/yasl.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_t loop_thread;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

struct gate {
    _Atomic int started;
    _Atomic int release;
    _Atomic int done;
    _Atomic int cancelled;
    _Atomic int released;
    _Atomic int ran;
    bool release_result;
    void *result;
};

struct release_arg {
    struct gate *gate;
    unsigned delay_us;
};

static void *release_thread(void *userdata)
{
    struct release_arg *arg = userdata;
    usleep(arg->delay_us);
    atomic_store(&arg->gate->release, 1);
    return NULL;
}

static void *blocking_run(void *userdata)
{
    struct gate *gate = userdata;
    atomic_store(&gate->started, 1);
    while (!atomic_load(&gate->release))
        usleep(1000);
    return gate->result;
}

static void *quick_run(void *userdata)
{
    struct gate *gate = userdata;
    atomic_store(&gate->ran, 1);
    return gate->result;
}

static void on_done(void *userdata, void *result)
{
    struct gate *gate = userdata;
    assert(pthread_equal(pthread_self(), loop_thread));
    assert(result == gate->result);
    atomic_store(&gate->done, 1);
}

static void on_cancel(void *userdata, void *result)
{
    struct gate *gate = userdata;
    assert(pthread_equal(pthread_self(), loop_thread));
    if (gate->release_result) {
        assert(result == gate->result);
        free(result);
        atomic_store(&gate->released, 1);
    } else {
        assert(result == NULL);
    }
    atomic_store(&gate->cancelled, 1);
}

static struct loop_work work_for(struct gate *gate, loop_work_fn run)
{
    return (struct loop_work){
        .run = run, .done = on_done, .cancel = on_cancel, .arg = gate};
}

/* A long submit must not hold the loop. The job blocks until the helper
 * releases it, so a submit that waits for the job exceeds the bound. */
static void test_submit_does_not_block(void)
{
    yasl_loop *loop = yasl_loop_create();
    assert(loop);
    struct loop_worker *worker = loop_worker_create(loop);
    assert(worker);
    struct gate gate = {0};
    gate.result = (void *)(uintptr_t)0x1234;
    struct loop_work work = work_for(&gate, blocking_run);

    uint64_t start = now_ms();
    assert(loop_worker_submit(worker, &work) == 0);
    uint64_t elapsed = now_ms() - start;
    assert(elapsed < 300);

    struct release_arg release = {.gate = &gate, .delay_us = 400000};
    pthread_t helper;
    assert(pthread_create(&helper, NULL, release_thread, &release) == 0);
    uint64_t deadline = now_ms() + 5000;
    while (!atomic_load(&gate.done) && now_ms() < deadline)
        yasl_loop_step(loop, 5);
    assert(atomic_load(&gate.done));
    assert(atomic_load(&gate.started));
    pthread_join(helper, NULL);
    loop_worker_destroy(worker);
    yasl_loop_destroy(loop);
}

/* Queued work is cancelled at shutdown, and a completed result that the loop
 * did not deliver is cancelled too. The done callback never runs. The cancel
 * hook receives the result, so it can free a heap result. */
static void test_cancel_at_shutdown(void)
{
    yasl_loop *loop = yasl_loop_create();
    assert(loop);
    struct loop_worker *worker = loop_worker_create(loop);
    assert(worker);
    struct gate running = {0};
    struct gate queued = {0};
    running.result = malloc(1);
    assert(running.result);
    running.release_result = true;
    struct loop_work first = work_for(&running, blocking_run);
    struct loop_work second = work_for(&queued, quick_run);
    assert(loop_worker_submit(worker, &first) == 0);
    uint64_t deadline = now_ms() + 2000;
    while (!atomic_load(&running.started) && now_ms() < deadline)
        usleep(1000);
    assert(atomic_load(&running.started));
    assert(loop_worker_submit(worker, &second) == 0);

    struct release_arg release = {.gate = &running, .delay_us = 50000};
    pthread_t helper;
    assert(pthread_create(&helper, NULL, release_thread, &release) == 0);
    loop_worker_destroy(worker);
    pthread_join(helper, NULL);

    assert(atomic_load(&running.cancelled));
    assert(atomic_load(&queued.cancelled));
    assert(atomic_load(&running.released));
    assert(!atomic_load(&running.done));
    assert(!atomic_load(&queued.done));
    assert(!atomic_load(&queued.ran));
    yasl_loop_destroy(loop);
}

int main(void)
{
    loop_thread = pthread_self();
    test_submit_does_not_block();
    test_cancel_at_shutdown();
    puts("worker-test: ok");
    return 0;
}
