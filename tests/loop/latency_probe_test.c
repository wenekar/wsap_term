#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "loop/latency_probe.h"

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
    munmap(p, sizeof(*p));
    return 0;
}
