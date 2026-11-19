/**
 * @file
 * Timing provenance, refresh estimates, and buffer-resize statistics.
 * All timestamps are monotonic nanoseconds, not wall-clock dates.
 */
#ifndef YASL_PRIVATE_TIMING_H
#define YASL_PRIVATE_TIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Cumulative backend buffer-resize measurements; unavailable counters are zero. */
struct yasl_private_resize_stats {
    /** Number of measured buffer resizes. */
    uint64_t count;
    /** Mean measured resize duration in microseconds. */
    double avg_us;
    /** Maximum measured resize duration in microseconds. */
    double max_us;
    /** Backend minor page-fault counter, if collected. */
    long minflt;
};

/* Describes last_present_ns, not merely availability of an extension. */
/** Provenance of last_present_ns; only PRESENTATION confirms compositor presentation. */
enum yasl_private_timing_source {
    YASL_PRIVATE_TIMING_UNKNOWN = 0, /**< No timing sample yet. */
    YASL_PRIVATE_TIMING_SUBMISSION, /**< CPU submission time, not scanout confirmation. */
    YASL_PRIVATE_TIMING_FRAME_CALLBACK, /**< Compositor pacing callback, not scanout confirmation. */
    YASL_PRIVATE_TIMING_PRESENTATION, /**< Compositor-confirmed presentation. */
};

/** Snapshot of estimates and timing provenance. Zero timestamps mean no sample yet. */
struct yasl_private_timing {
    /** Estimated refresh interval in nanoseconds; zero if unavailable. */
    double refresh_ns;
    /** Last timestamp, whose meaning is specified by source. */
    uint64_t last_present_ns;
    /** Estimated next presentation time, or zero without a prior sample. */
    uint64_t next_present_ns;
    /** Backend skipped-frame counter; not comparable across backends. */
    uint64_t frames_skipped;
    /** Provenance of last_present_ns. */
    enum yasl_private_timing_source source;
    /** Cumulative backend resize statistics. */
    struct yasl_private_resize_stats resize;
};

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_TIMING_H */
