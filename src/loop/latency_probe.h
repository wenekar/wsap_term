#ifndef WSAP_LATENCY_PROBE_H
#define WSAP_LATENCY_PROBE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LATENCY_PRESENTATION_VSYNC (1u << 0)
#define LATENCY_PRESENTATION_ZERO_COPY (1u << 3)

enum latency_stage {
    LATENCY_STAGE_KEY_IN, LATENCY_STAGE_PTY_WRITTEN,
    LATENCY_STAGE_PTY_READ, LATENCY_STAGE_RENDER_START,
    LATENCY_STAGE_COMMIT, LATENCY_STAGE_COUNT
};
struct latency_probe_shm {
    _Atomic uint32_t focused;
    _Atomic uint32_t ready;
    _Atomic uint64_t focus_losses;
    _Atomic uint32_t presentation_supported;
    _Atomic uint32_t fullscreen;
    _Atomic uint64_t request;
    _Atomic uint64_t completed;
    _Atomic uint64_t presented;
    uint64_t stage_ns[LATENCY_STAGE_COUNT];
    uint64_t presentation_ns;
    uint64_t presentation_received_ns;
    uint32_t presentation_refresh_ns;
    uint32_t presentation_flags;
    bool discarded;
};
void latency_probe_init(void);
void latency_probe_complete(void);
void latency_probe_set_focus(bool focused);
void latency_probe_stage(enum latency_stage stage);
void latency_probe_scan_output(const char *data, size_t len);
void latency_probe_fullscreen(bool fullscreen);
#endif
