#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "loop/latency_probe.h"
#include "yasl/yasl.h"

static struct latency_probe_shm *probe;

/*
 * The publication of one presentation. Two display threads can report at the
 * same time, and the benchmark reads a published sample at any time, so the
 * fields of one record are written under this lock. It covers the copy of the
 * fields only: no renderer work and no callback runs while it is held.
 */
static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
/* The sample that a presentation was published for. The writer holds the lock. */
static uint64_t published_sample;

void latency_probe_init(void)
{
    const char *path = getenv("WSAP_LATENCY_PROBE");
    if (probe || !path || !*path)
        return;

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(*probe)) < 0) {
        fprintf(stderr, "latency probe: cannot set up %s\n", path);
        if (fd >= 0) close(fd);
        return;
    }
    void *map = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "latency probe: mmap failed\n");
        return;
    }
    probe = map;
}

static uint64_t active, rendered;
static uint64_t stamps[LATENCY_STAGE_COUNT];
static uint64_t pending;

static void sync_request(void)
{
    if (!probe) return;
    uint64_t id = atomic_load_explicit(&probe->request, memory_order_acquire);
    if (id != active) {
        active = id;
        rendered = 0;
        memset(stamps, 0, sizeof(stamps));
    }
}

void latency_probe_stage(enum latency_stage stage)
{
    if (!probe) return;
    sync_request();
    if (!active || atomic_load_explicit(&probe->completed, memory_order_relaxed) == active)
        return;
    if (stage == LATENCY_STAGE_RENDER_START) {
        if (!stamps[LATENCY_STAGE_PTY_READ]) return;
        rendered = active;
    }
    if ((unsigned)stage < LATENCY_STAGE_COUNT && !stamps[stage])
        stamps[stage] = yasl_time_ns();
}

void latency_probe_scan_output(const char *data, size_t len)
{
    if (!probe) return;
    static char marker[64];
    static size_t used;
    sync_request();
    for (size_t i = 0; i < len; i++) {
        if (pending) {
            if (data[i] == 'a' && pending == active && stamps[LATENCY_STAGE_KEY_IN])
                latency_probe_stage(LATENCY_STAGE_PTY_READ);
            pending = 0;
        }
        if (data[i] == '\033') used = 0;
        if (used < sizeof(marker) - 1) marker[used++] = data[i];
        if (data[i] == '\007') {
            marker[used] = 0;
            unsigned long long id = 0;
            int end = 0;
            if (sscanf(marker, "\033]777;latency=%llu\007%n", &id, &end) == 1 &&
                end == (int)used && id == active)
                pending = id;
            used = 0;
        }
    }
}

/* The sample that waits for a presentation: the sample of the current request
 * that a frame rendered and no display presented yet. The submission of a frame
 * is not the end of the wait. A frame that the display discards leaves the
 * sample waiting, so a later frame of the same request carries it again. */
static uint64_t waiting_sample(void)
{
    if (!probe) return 0;
    sync_request();
    if (!rendered) return 0;
    /* The store of `presented` publishes the record, so this load also orders
     * the record reads of the caller. */
    return atomic_load_explicit(&probe->presented, memory_order_acquire) == rendered
        ? 0 : rendered;
}

void latency_probe_complete(void)
{
    if (!probe) return;
    /* The submission of a frame does not wait for its presentation. The display
     * can present the frame before the loop reaches this point, so this record
     * reads the sample that the frame belongs to, and not the sample that waits
     * for a presentation. */
    sync_request();
    uint64_t id = rendered;
    if (!id) return;
    /* The first frame of a sample records its stages. A later frame of the same
     * sample shows the same content, so it keeps the record that a reader can
     * already take. */
    if (atomic_load_explicit(&probe->completed, memory_order_relaxed) == id)
        return;
    if (!stamps[LATENCY_STAGE_COMMIT])
        stamps[LATENCY_STAGE_COMMIT] = yasl_time_ns();
    memcpy(probe->stage_ns, stamps, sizeof(probe->stage_ns));
    /* The reader takes the stages above through this store. */
    atomic_store_explicit(&probe->completed, id, memory_order_release);
}

uint64_t latency_probe_frame_target(void)
{
    /* A frame carries its sample until a display presents that sample, or until
     * the benchmark starts the next one. */
    return waiting_sample();
}

void latency_probe_presented(uint64_t sample, uint64_t presented_ns, uint64_t received_ns,
                             uint32_t refresh_ns, uint32_t flags)
{
    if (!probe) return;
    /* A frame that the display showed reports that this platform has real
     * presentation feedback. The frames before the first request carry no
     * sample, and the benchmark reads the capability before it starts, so the
     * capability does not depend on a sample. */
    if (presented_ns)
        atomic_store_explicit(&probe->presentation_supported, 1, memory_order_release);
    /* Only the frame of the current request can publish its times. A frame of
     * an older sample presents after the next request has begun, so it cannot
     * complete the newer sample. */
    if (!sample || atomic_load_explicit(&probe->request, memory_order_acquire) != sample)
        return;
    pthread_mutex_lock(&present_lock);
    /* Check the request again under the lock. A report can pass the check above,
     * then wait while the benchmark starts the next sample and a frame of it
     * publishes. Without this check the delayed report writes the record of the
     * older sample over the published record of the newer one.
     *
     * The first presentation of a sample is the one that counts. A second
     * report for it cannot change the record that a reader already took. */
    if (atomic_load_explicit(&probe->request, memory_order_acquire) == sample &&
        published_sample != sample) {
        if (!presented_ns) {
            /* No display showed the frame. It is not a presentation, and a
             * later frame of the same sample can still present. */
            atomic_store_explicit(&probe->discarded, sample, memory_order_relaxed);
        } else {
            probe->presentation_ns = presented_ns;
            probe->presentation_received_ns = received_ns;
            probe->presentation_refresh_ns = refresh_ns;
            probe->presentation_flags = flags;
            published_sample = sample;
            /* The reader takes the fields above through this store. */
            atomic_store_explicit(&probe->presented, sample, memory_order_release);
        }
    }
    pthread_mutex_unlock(&present_lock);
}

void latency_probe_set_focus(bool focused)
{
    if (!probe) return;
    if (!focused) atomic_fetch_add_explicit(&probe->focus_losses, 1, memory_order_relaxed);
    atomic_store_explicit(&probe->focused, focused, memory_order_relaxed);
}

void latency_probe_fullscreen(bool fullscreen)
{
    if (probe) atomic_store_explicit(&probe->fullscreen, fullscreen, memory_order_release);
}
