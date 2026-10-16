#ifndef FRAME_STATS_H
#define FRAME_STATS_H

#include <stdint.h>

struct frame_stats {
    uint64_t frames_drawn;
    double frame_min_ms;
    double frame_max_ms;
    double frame_sum_ms;
    uint64_t frame_n;
    double render_sum_us;
    double render_max_us; /* Max since the last frame_stats_print(). */
    uint64_t render_n;
    uint64_t started_ns;
    uint64_t render_started_ns;
    uint64_t last_frame_ns;
};

/* Print the frame statistics. */
void frame_stats_print(struct frame_stats *st, uint64_t skipped);
void frame_stats_tick(struct frame_stats *st);
void render_begin(struct frame_stats *st);
void render_end_and_count_frame(struct frame_stats *st);

#endif
