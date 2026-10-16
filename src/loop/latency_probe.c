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

static uint64_t active, rendered;
static uint64_t stamps[LATENCY_STAGE_COUNT];
/* The marker ended; its trailing byte is still due. */
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

/* The sample whose reply the last render drew, while it is not complete. */
static uint64_t pending_frame_id(void)
{
    if (!probe) return 0;
    sync_request();
    return rendered && atomic_load_explicit(&probe->completed, memory_order_relaxed) != rendered
        ? rendered : 0;
}

void latency_probe_complete(void)
{
    uint64_t id = pending_frame_id();
    if (!id) return;
    if (!stamps[LATENCY_STAGE_COMMIT])
        stamps[LATENCY_STAGE_COMMIT] = yasl_time_ns();
    memcpy(probe->stage_ns, stamps, sizeof(probe->stage_ns));
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
