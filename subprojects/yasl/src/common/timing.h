#ifndef YASL_PRIVATE_COMMON_TIMING_H
#define YASL_PRIVATE_COMMON_TIMING_H

#include "common/private_timing.h"
#include <stdbool.h>

#define YASL_PRIVATE_REFRESH_DEFAULT_NS 16666666.0

struct yasl_private_frame_clock {
    uint64_t last_present_ns;
    uint64_t resize_count;
    uint64_t resize_sum_ns;
    uint64_t resize_max_ns;
    long resize_minflt;
    uint64_t frame_time_ns;
    double refresh_ns;
    enum yasl_private_timing_source source;
};

uint64_t yasl_private_clock_extend_ms(uint32_t ms, uint64_t now_ns);
void yasl_private_clock_presented(struct yasl_private_frame_clock *c, uint64_t now, bool sample);
void yasl_private_clock_note_resize(struct yasl_private_frame_clock *c, uint64_t dt_ns);
void yasl_private_clock_resize_stats(const struct yasl_private_frame_clock *c,
                             struct yasl_private_resize_stats *out);
uint64_t yasl_private_clock_next_present_ns(const struct yasl_private_frame_clock *c);
uint64_t yasl_private_clock_frame_timestamp_ns(struct yasl_private_frame_clock *c, uint64_t now);

#endif /* YASL_PRIVATE_COMMON_TIMING_H */
