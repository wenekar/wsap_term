#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "loop/latency_probe.h"
#include "yasl/yasl.h"

static struct latency_probe_shm *probe;

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

static _Atomic uint64_t active, rendered;
static _Atomic uint64_t stamps[LATENCY_STAGE_COUNT];

static uint64_t sync_request(void)
{
    if (!probe)
        return 0;
    uint64_t id = atomic_load_explicit(&probe->request, memory_order_acquire);
    uint64_t cur = atomic_load_explicit(&active, memory_order_acquire);
    if (id != cur &&
        atomic_compare_exchange_strong_explicit(&active, &cur, id,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_store_explicit(&rendered, 0, memory_order_relaxed);
        for (int i = 0; i < LATENCY_STAGE_COUNT; i++)
            atomic_store_explicit(&stamps[i], 0, memory_order_relaxed);
    }
    return id;
}

void latency_probe_stage(enum latency_stage stage)
{
    if (!probe)
        return;
    uint64_t id = sync_request();
    if (!id || atomic_load_explicit(&probe->completed, memory_order_relaxed) == id)
        return;
    if (stage == LATENCY_STAGE_RENDER_START) {
        if (!atomic_load_explicit(&stamps[LATENCY_STAGE_PTY_READ], memory_order_acquire))
            return;
        atomic_store_explicit(&rendered, id, memory_order_release);
    }
    if ((unsigned)stage < LATENCY_STAGE_COUNT) {
        uint64_t zero = 0;
        uint64_t now = yasl_time_ns();
        atomic_compare_exchange_strong_explicit(&stamps[stage], &zero, now,
                                                memory_order_release,
                                                memory_order_relaxed);
    }
}

void latency_probe_scan_output(const char *data, size_t len)
{
    if (!probe)
        return;
    static const char prefix[] = "\033]777;latency=";
    const size_t prefix_len = sizeof(prefix) - 1;
    static _Thread_local char marker[64];
    static _Thread_local size_t used;
    static _Thread_local uint64_t pending;

    uint64_t want = sync_request();
    if (!want) {
        used = 0;
        pending = 0;
        return;
    }

    size_t i = 0;
    while (i < len) {
        if (pending) {
            if (data[i] == 'a' && pending == want &&
                atomic_load_explicit(&stamps[LATENCY_STAGE_KEY_IN], memory_order_acquire))
                latency_probe_stage(LATENCY_STAGE_PTY_READ);
            pending = 0;
        }
        if (used == 0) {
            const char *esc = memchr(data + i, '\033', len - i);
            if (!esc)
                return;
            i = (size_t)(esc - data);
        }
        char ch = data[i++];
        if (ch == '\033') {
            marker[0] = '\033';
            used = 1;
            continue;
        }
        if (used < prefix_len) {
            if (ch == prefix[used])
                marker[used++] = ch;
            else
                used = 0;
            continue;
        }
        if (ch >= '0' && ch <= '9' && used < sizeof(marker) - 1) {
            marker[used++] = ch;
            continue;
        }
        if (ch == '\007' && used > prefix_len) {
            uint64_t id = 0;
            for (size_t k = prefix_len; k < used; k++)
                id = id * 10 + (uint64_t)(marker[k] - '0');
            if (id == want)
                pending = id;
        }
        used = 0;
    }
}

static uint64_t pending_frame_id(void)
{
    if (!probe)
        return 0;
    sync_request();
    uint64_t id = atomic_load_explicit(&rendered, memory_order_acquire);
    return id && atomic_load_explicit(&probe->completed, memory_order_relaxed) != id
        ? id : 0;
}

void latency_probe_complete(void)
{
    uint64_t id = pending_frame_id();
    if (!id)
        return;
    uint64_t zero = 0;
    uint64_t now = yasl_time_ns();
    atomic_compare_exchange_strong_explicit(&stamps[LATENCY_STAGE_COMMIT], &zero, now,
                                            memory_order_release,
                                            memory_order_relaxed);
    for (int i = 0; i < LATENCY_STAGE_COUNT; i++)
        probe->stage_ns[i] = atomic_load_explicit(&stamps[i], memory_order_acquire);
    atomic_store_explicit(&probe->completed, id, memory_order_release);
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
