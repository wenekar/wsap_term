#include "ui/yasl/stats.h"
#include "yasl/yasl.h"

#include <stdio.h>

void frame_stats_tick(struct frame_stats *st)
{
    uint64_t now = yasl_time_ns();
    if (st->last_frame_ns) {
        double dt_ms = (double)(now - st->last_frame_ns) / 1e6;
        st->frame_sum_ms += dt_ms;
        st->frame_n++;
        if (st->frame_n == 1 || dt_ms < st->frame_min_ms)
            st->frame_min_ms = dt_ms;
        if (dt_ms > st->frame_max_ms)
            st->frame_max_ms = dt_ms;
    }
    st->last_frame_ns = now;
}

void render_begin(struct frame_stats *st)
{
    st->render_started_ns = yasl_time_ns();
}

void render_end_and_count_frame(struct frame_stats *st)
{
    double us = (double)(yasl_time_ns() - st->render_started_ns) / 1e3;
    st->render_sum_us += us;
    st->render_n++;
    if (us > st->render_max_us)
        st->render_max_us = us;
    st->frames_drawn++;
}

void frame_stats_print(struct frame_stats *st, uint64_t skipped)
{
    double secs = st->started_ns
            ? (double)(yasl_time_ns() - st->started_ns) / 1e9
            : 0.0;
    double avg_frame = st->frame_n ? st->frame_sum_ms / (double)st->frame_n : 0.0;
    double avg_render = st->render_n ? st->render_sum_us / (double)st->render_n : 0.0;
    printf("[stats %7.2fs] frames=%llu skip=%llu fps=%.1f dt(ms) avg=%.2f min=%.2f max=%.2f | "
           "render(us) avg=%.1f max=%.1f\n",
           secs,
           (unsigned long long)st->frames_drawn,
           (unsigned long long)skipped,
           secs > 0.0 ? (double)st->frame_n / secs : 0.0,
           avg_frame, st->frame_n ? st->frame_min_ms : 0.0, st->frame_max_ms,
           avg_render, st->render_max_us);
    st->render_max_us = 0.0;
    fflush(stdout);
}
