#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "loop/latency_probe.h"

/* The tests wait for threads, and the suite runs many tests at once. A count of
 * spins is not a time then, so every wait below has a deadline. */
static uint64_t test_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One request of the benchmark: the loop reads the response of a key, starts a
 * frame for it, and completes that frame. */
static void start_sample(struct latency_probe_shm *p, unsigned long long id)
{
    atomic_store_explicit(&p->request, id, memory_order_release);
    latency_probe_stage(LATENCY_STAGE_KEY_IN);
    latency_probe_stage(LATENCY_STAGE_PTY_WRITTEN);
    char marker[64];
    int n = snprintf(marker, sizeof(marker), "\033]777;latency=%llu\007a", id);
    assert(n > 0 && (size_t)n < sizeof(marker));
    latency_probe_scan_output(marker, (size_t)n);
    latency_probe_stage(LATENCY_STAGE_RENDER_START);
}

/* One display thread that reports one sample: the values of a single reporter.
 * They belong together, so a record of two reporters is visibly mixed. */
struct reporter {
    /* Both reporters leave the gate together. */
    _Atomic int *ready;
    uint64_t sample;
    uint64_t presented_ns;
    uint32_t refresh_ns;
    uint32_t flags;
};

#define REPORTERS 8

/* The reporters report the same sample at the same time, so a record that is
 * not serialized interleaves. The gate only starts them; it measures no time. */
static void *report(void *ud)
{
    struct reporter *r = ud;
    atomic_fetch_add_explicit(r->ready, 1, memory_order_acq_rel);
    while (atomic_load_explicit(r->ready, memory_order_acquire) < REPORTERS)
        ;
    latency_probe_presented(r->sample, r->presented_ns, r->presented_ns + 1000,
                            r->refresh_ns, r->flags);
    return NULL;
}

/* The published record is the record of exactly one reporter: the times, the
 * refresh, and the flags belong together. */
static bool record_of(const struct latency_probe_shm *p, const struct reporter *reporters)
{
    for (int i = 0; i < REPORTERS; i++) {
        if (p->presentation_ns == reporters[i].presented_ns &&
            p->presentation_received_ns == reporters[i].presented_ns + 1000 &&
            p->presentation_refresh_ns == reporters[i].refresh_ns &&
            p->presentation_flags == reporters[i].flags)
            return true;
    }
    return false;
}

/*
 * A display thread that reports an older sample in a loop, as a display reports
 * a late frame while the benchmark starts the next sample. It pauses on no gate,
 * so some of its reports are between the check of the request and the lock that
 * publishes a record when the next request begins.
 */
struct late_report {
    _Atomic int *go;
    _Atomic bool *stop;
    _Atomic unsigned long *calls;
    uint64_t sample;
    uint64_t presented_ns;
};

static void *report_late(void *ud)
{
    struct late_report *r = ud;
    while (!atomic_load_explicit(r->go, memory_order_acquire))
        ;
    while (!atomic_load_explicit(r->stop, memory_order_acquire)) {
        latency_probe_presented(r->sample, r->presented_ns, r->presented_ns + 1000,
                                8333333, 0);
        atomic_fetch_add_explicit(r->calls, 1, memory_order_relaxed);
    }
    return NULL;
}

/* A reader that takes the record of the newer sample while the late reports
 * run, as the benchmark reads a published sample. The record of a sample never
 * changes after its publication, and it never goes back to an older sample. */
struct record_reader {
    struct latency_probe_shm *p;
    _Atomic bool *stop;
    uint64_t sample;
    uint64_t presented_ns;
    uint32_t refresh_ns;
    uint32_t flags;
    _Atomic unsigned long reads;
    /* The reader took the newer record while it held other values. */
    bool changed;
    /* The reader took an older record after it took the newer one. */
    bool regressed;
};

static void *read_record(void *ud)
{
    struct record_reader *r = ud;
    bool seen = false;
    while (!atomic_load_explicit(r->stop, memory_order_acquire)) {
        uint64_t id = atomic_load_explicit(&r->p->presented, memory_order_acquire);
        if (id != r->sample) {
            if (seen && id) r->regressed = true;
            continue;
        }
        seen = true;
        atomic_fetch_add_explicit(&r->reads, 1, memory_order_relaxed);
        if (r->p->presentation_ns != r->presented_ns ||
            r->p->presentation_received_ns != r->presented_ns + 1000 ||
            r->p->presentation_refresh_ns != r->refresh_ns ||
            r->p->presentation_flags != r->flags)
            r->changed = true;
    }
    return NULL;
}

/*
 * The display can report an older sample after the benchmark started the next
 * one. That report changes nothing, also when it passed the check of the current
 * request before the next request began: the check repeats under the lock that
 * publishes a record. A reader takes the record of the newer sample while the
 * late reports run.
 */
static void late_reports_across_requests(struct latency_probe_shm *p)
{
    enum { LATE_REPORTERS = 8, ROUNDS = 200 };

    for (int round = 0; round < ROUNDS; round++) {
        uint64_t old = 20000 + 2 * (uint64_t)round, cur = old + 1;
        start_sample(p, old);
        latency_probe_complete();
        latency_probe_presented(old, 2000000, 2001000, 8333333, 0);
        assert(atomic_load_explicit(&p->presented, memory_order_acquire) == old);

        _Atomic int go = 0;
        _Atomic bool stop = false;
        _Atomic unsigned long calls = 0;
        struct late_report reports[LATE_REPORTERS];
        pthread_t threads[LATE_REPORTERS];
        for (int i = 0; i < LATE_REPORTERS; i++) {
            reports[i] = (struct late_report){
                .go = &go, .stop = &stop, .calls = &calls, .sample = old,
                .presented_ns = 3000000ull + (unsigned long long)i * 1000,
            };
            assert(pthread_create(&threads[i], NULL, report_late, &reports[i]) == 0);
        }
        _Atomic bool stop_reader = false;
        struct record_reader reader = {
            .p = p, .stop = &stop_reader, .sample = cur,
            .presented_ns = 4000000, .refresh_ns = 4166667,
            .flags = LATENCY_PRESENTATION_VSYNC,
        };
        pthread_t reader_thread;
        assert(pthread_create(&reader_thread, NULL, read_record, &reader) == 0);

        atomic_store_explicit(&go, 1, memory_order_release);
        /* Let the late reports run against the older sample. */
        for (uint64_t deadline = test_now_ns() + 2000000000ull;
             test_now_ns() < deadline &&
             atomic_load_explicit(&calls, memory_order_relaxed) <
                 (unsigned long)LATE_REPORTERS * 64;)
            sched_yield();
        /* The next request begins, and its frame publishes at once, so a late
         * report can still carry its decision of the older request. */
        atomic_store_explicit(&p->request, cur, memory_order_release);
        latency_probe_presented(cur, 4000000, 4001000, 4166667, LATENCY_PRESENTATION_VSYNC);
        assert(atomic_load_explicit(&p->presented, memory_order_acquire) == cur);
        /* The reader takes the record of the newer sample. */
        for (uint64_t deadline = test_now_ns() + 2000000000ull;
             test_now_ns() < deadline &&
             !atomic_load_explicit(&reader.reads, memory_order_acquire);)
            sched_yield();

        atomic_store_explicit(&stop, true, memory_order_release);
        for (int i = 0; i < LATE_REPORTERS; i++)
            assert(pthread_join(threads[i], NULL) == 0);
        atomic_store_explicit(&stop_reader, true, memory_order_release);
        assert(pthread_join(reader_thread, NULL) == 0);

        /* Every late report of the older sample changed nothing. */
        assert(!reader.changed);
        assert(!reader.regressed);
        assert(atomic_load_explicit(&reader.reads, memory_order_acquire) > 0);
        assert(atomic_load_explicit(&p->presented, memory_order_acquire) == cur);
        assert(p->presentation_ns == 4000000);
        assert(p->presentation_received_ns == 4001000);
        assert(p->presentation_refresh_ns == 4166667);
        assert(p->presentation_flags == LATENCY_PRESENTATION_VSYNC);
    }
}

int main(void)
{
    char path[] = "/tmp/latency-probe-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(ftruncate(fd, sizeof(struct latency_probe_shm)) == 0);
    struct latency_probe_shm *p = mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(p != MAP_FAILED);
    close(fd);
    setenv("WSAP_LATENCY_PROBE", path, 1);
    latency_probe_init();
    unlink(path);
    /* A frame before the first request carries no sample. A frame that reached
     * no display reports no capability and publishes no sample. */
    latency_probe_presented(0, 0, 1000, 8333333, 0);
    assert(atomic_load_explicit(&p->presentation_supported, memory_order_acquire) == 0);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 0);
    /* A frame that the display showed reports the capability of the platform,
     * with no sample to publish. The benchmark reads the capability before it
     * starts, and the frames of the startup screen carry no sample. */
    latency_probe_presented(0, 50000, 60000, 8333333, LATENCY_PRESENTATION_VSYNC);
    assert(atomic_load_explicit(&p->presentation_supported, memory_order_acquire) == 1);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 0);
    assert(p->presentation_ns == 0);
    assert(p->presentation_received_ns == 0);
    assert(p->presentation_flags == 0);
    atomic_store_explicit(&p->request, 1, memory_order_release);
    latency_probe_stage(LATENCY_STAGE_KEY_IN);
    latency_probe_stage(LATENCY_STAGE_PTY_WRITTEN);
    latency_probe_scan_output("unrelated", 9);
    latency_probe_stage(LATENCY_STAGE_RENDER_START);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 0);
    const char reply[] = "\033]777;latency=1\007a";
    for (size_t i = 0; i < sizeof(reply) - 2; i++) latency_probe_scan_output(reply + i, 1);
    latency_probe_stage(LATENCY_STAGE_RENDER_START);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 0);
    latency_probe_scan_output(reply + sizeof(reply) - 2, 1);
    latency_probe_stage(LATENCY_STAGE_RENDER_START);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 1);
    for (int i = 0; i < LATENCY_STAGE_COUNT; i++) {
        assert(p->stage_ns[i]);
        if (i) assert(p->stage_ns[i] >= p->stage_ns[i - 1]);
    }
    uint64_t commit = p->stage_ns[LATENCY_STAGE_COMMIT];
    latency_probe_complete();
    assert(p->stage_ns[LATENCY_STAGE_COMMIT] == commit);
    atomic_store_explicit(&p->request, 2, memory_order_release);
    latency_probe_stage(LATENCY_STAGE_KEY_IN);
    latency_probe_stage(LATENCY_STAGE_PTY_WRITTEN);
    latency_probe_scan_output(reply, sizeof(reply) - 1);
    latency_probe_stage(LATENCY_STAGE_RENDER_START);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 1);
    const char second[] = "\033]777;latency=2\007a";
    latency_probe_scan_output(second, sizeof(second) - 1);
    latency_probe_stage(LATENCY_STAGE_RENDER_START);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 2);

    /* A frame carries the sample that waits for a presentation. The submission
     * of the frame is separate: it retires nothing, so the sample still waits
     * after the loop submitted a frame of it. */
    assert(latency_probe_frame_target() == 2);
    start_sample(p, 3);
    assert(latency_probe_frame_target() == 3);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 3);
    uint64_t commit3 = p->stage_ns[LATENCY_STAGE_COMMIT];
    assert(commit3);
    assert(latency_probe_frame_target() == 3);

    /* Feedback of an older sample cannot complete the current one. */
    latency_probe_presented(2, 40000, 45000, 8333333, LATENCY_PRESENTATION_VSYNC);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 0);
    assert(p->presentation_flags == 0 && p->presentation_ns == 0);

    /* The display showed no frame of the submitted sample: that is a discard,
     * not a presentation. The sample still waits for a frame. */
    latency_probe_presented(3, 0, 55000, 8333333, LATENCY_PRESENTATION_VSYNC);
    assert(atomic_load_explicit(&p->discarded, memory_order_relaxed) == 3);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 0);

    /* A later frame of the same request carries the sample again. Its
     * presentation publishes the sample, with the stages of the submission. */
    assert(latency_probe_frame_target() == 3);
    latency_probe_complete();
    assert(p->stage_ns[LATENCY_STAGE_COMMIT] == commit3);
    latency_probe_presented(3, 50000, 60000, 8333333, LATENCY_PRESENTATION_VSYNC);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 3);
    assert(atomic_load_explicit(&p->presentation_supported, memory_order_acquire) == 1);
    assert(p->presentation_ns == 50000);
    assert(p->presentation_received_ns == 60000);
    assert(p->presentation_refresh_ns == 8333333);
    assert(p->presentation_flags == LATENCY_PRESENTATION_VSYNC);
    /* The sample is presented, so a later frame of the same request carries
     * none. */
    assert(latency_probe_frame_target() == 0);

    /* The first presentation of a sample is the one that counts. A second
     * report of the same sample cannot change a record that a reader took. */
    latency_probe_presented(3, 90000, 95000, 4166667, 0);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 3);
    assert(p->presentation_ns == 50000);
    assert(p->presentation_received_ns == 60000);
    assert(p->presentation_refresh_ns == 8333333);
    assert(p->presentation_flags == LATENCY_PRESENTATION_VSYNC);

    /* A frame that reached no display is a discard, not a presentation. It
     * keeps the times of the last real presentation, and the sample still
     * waits for a frame. */
    start_sample(p, 4);
    assert(latency_probe_frame_target() == 4);
    latency_probe_complete();
    latency_probe_presented(4, 0, 70000, 8333333, LATENCY_PRESENTATION_VSYNC);
    assert(atomic_load_explicit(&p->discarded, memory_order_relaxed) == 4);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 3);
    assert(p->presentation_ns == 50000);
    assert(p->presentation_received_ns == 60000);
    assert(p->presentation_flags == LATENCY_PRESENTATION_VSYNC);

    /* Out-of-order feedback for the sample before the current one changes
     * nothing, and the current sample still reports. */
    start_sample(p, 5);
    latency_probe_complete();
    latency_probe_presented(4, 80000, 90000, 8333333, 0);
    assert(atomic_load_explicit(&p->discarded, memory_order_relaxed) == 4);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 3);
    latency_probe_presented(5, 80000, 90000, 4166667, 0);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 5);
    assert(p->presentation_ns == 80000);
    assert(p->presentation_refresh_ns == 4166667);
    assert(p->presentation_flags == 0);
    assert(latency_probe_frame_target() == 0);

    /* A report without a sample does nothing. */
    latency_probe_presented(0, 1, 2, 3, 0);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 5);
    assert(p->presentation_ns == 80000);

    /* A display can present a frame before the loop completes its submission.
     * The submission records the stages of its sample anyway: the readiness to
     * submit a frame does not depend on the presentation of the frame. */
    start_sample(p, 6);
    assert(latency_probe_frame_target() == 6);
    latency_probe_presented(6, 110000, 120000, 8333333, LATENCY_PRESENTATION_VSYNC);
    assert(atomic_load_explicit(&p->presented, memory_order_acquire) == 6);
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 5);
    latency_probe_complete();
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 6);
    uint64_t stages6[LATENCY_STAGE_COUNT];
    memcpy(stages6, p->stage_ns, sizeof(stages6));
    for (int i = 0; i < LATENCY_STAGE_COUNT; i++) {
        assert(stages6[i]);
        if (i) assert(stages6[i] >= stages6[i - 1]);
    }
    /* A second completion of the same sample keeps the record that a reader can
     * already take. */
    latency_probe_complete();
    assert(memcmp(p->stage_ns, stages6, sizeof(stages6)) == 0);
    assert(atomic_load_explicit(&p->completed, memory_order_acquire) == 6);

    /* Several display threads report one sample at the same time, and a reader
     * takes the record as the benchmark does. The published record belongs to
     * one reporter, never to a mix of them. */
    for (unsigned long long round = 0; round < 200; round++) {
        unsigned long long id = 1000 + round;
        start_sample(p, id);
        latency_probe_complete();
        atomic_store_explicit(&p->presented, 0, memory_order_release);
        p->presentation_ns = 0;
        p->presentation_received_ns = 0;
        p->presentation_refresh_ns = 0;
        p->presentation_flags = 0;
        _Atomic int ready = 0;
        struct reporter reporters[REPORTERS];
        pthread_t threads[REPORTERS];
        for (int i = 0; i < REPORTERS; i++) {
            reporters[i] = (struct reporter){
                .ready = &ready, .sample = id,
                .presented_ns = 1000000ull + (unsigned long long)i * 1000,
                .refresh_ns = 8333333u - (uint32_t)i,
                .flags = i % 2 ? 0 : LATENCY_PRESENTATION_VSYNC,
            };
            assert(pthread_create(&threads[i], NULL, report, &reporters[i]) == 0);
        }
        /* The reader takes the record while the reports run. */
        for (uint64_t deadline = test_now_ns() + 5000000000ull;
             test_now_ns() < deadline &&
             atomic_load_explicit(&p->presented, memory_order_acquire) != id;)
            sched_yield();
        assert(atomic_load_explicit(&p->presented, memory_order_acquire) == id);
        assert(record_of(p, reporters));
        for (int i = 0; i < REPORTERS; i++)
            assert(pthread_join(threads[i], NULL) == 0);
        /* The record of one reporter survived every report. */
        assert(record_of(p, reporters));
        /* A report that arrives later changes nothing. */
        uint64_t taken = p->presentation_ns;
        latency_probe_presented(id, 7000000, 7001000, 1000000, 0);
        assert(p->presentation_ns == taken);
    }

    /* A display can report an older sample after the next request began. */
    late_reports_across_requests(p);

    munmap(p, sizeof(*p));
    return 0;
}
