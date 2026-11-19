#ifndef WSAP_LATENCY_PROBE_H
#define WSAP_LATENCY_PROBE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Presentation flags of the display. Only a paced presentation, one that waits
 * for the display refresh, sets VSYNC. A tearing frame never does. */
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
    /* 1 once the display reported a presentation. Zero means that the window
     * layer of this platform gives no feedback. */
    _Atomic uint32_t presentation_supported;
    _Atomic uint32_t fullscreen;
    _Atomic uint64_t request;
    /* The sample whose first frame the loop submitted. A later frame of the same
     * sample keeps this record. */
    _Atomic uint64_t completed;
    /* The sample whose frame the display reported. */
    _Atomic uint64_t presented;
    uint64_t stage_ns[LATENCY_STAGE_COUNT];
    uint64_t presentation_ns;
    uint64_t presentation_received_ns;
    uint32_t presentation_refresh_ns;
    uint32_t presentation_flags;
    /* The sample of the last frame that the display discarded. */
    _Atomic uint64_t discarded;
};
void latency_probe_init(void);
/* Records the submission of the current frame. It publishes the stages of its
 * sample once, also when a display presented the frame before this call: the
 * readiness to submit a frame does not depend on its presentation. A second
 * submission of the same sample keeps the record that a reader already took. */
void latency_probe_complete(void);
void latency_probe_set_focus(bool focused);
void latency_probe_stage(enum latency_stage stage);
void latency_probe_scan_output(const char *data, size_t len);
void latency_probe_fullscreen(bool fullscreen);

/*
 * Presentation feedback. The display reports a presentation on its own thread,
 * after the loop submitted the frame. Capture the identity of the frame before
 * that callback runs, so late or out-of-order feedback cannot complete another
 * sample.
 */

/* Returns the active probe sample ID pending presentation, or 0 if none is pending. */
uint64_t latency_probe_frame_target(void);

/*
 * Records that the display presented, or failed to present, the frame of
 * `sample`. It is safe on any thread: it writes the shared probe fields only,
 * and reads no window, no surface, and no renderer state.
 *
 * - A real presentation sets `presentation_supported`, also for a frame with no
 *   sample, so the frames before the first request report the capability.
 * - The times and the flags are published only for the frame of the current
 *   request, and only once. A duplicate report for a published sample changes
 *   nothing, and a report of an older sample cannot complete a newer one, also
 *   when that report was delayed across the start of the newer request.
 * - `presented_ns` is zero when the display reports no presentation; the sample
 *   then counts as discarded and can still present with a later frame.
 * - `received_ns` is the time of the callback, and `refresh_ns` is the display
 *   period at submission. `flags` holds the LATENCY_PRESENTATION_* bits.
 */
void latency_probe_presented(uint64_t sample, uint64_t presented_ns, uint64_t received_ns,
                             uint32_t refresh_ns, uint32_t flags);
#endif
