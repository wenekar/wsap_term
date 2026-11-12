#include "ui/yasl/stats.h"
#include "yasl/yasl.h"

#include <stdio.h>

static double percentile_from_hist(const uint32_t *hist, uint64_t total,
                                    double bucket_width, double min_val,
                                    double max_val, double pct)
{
    if (!total)
        return 0.0;
    uint64_t target = (uint64_t)((double)total * pct + 0.5);
    if (target == 0)
        target = 1;
    uint64_t accumulated = 0;
    double val = max_val;
    for (size_t i = 0; i < FRAME_STATS_HIST_BUCKETS; i++) {
        accumulated += hist[i];
        if (accumulated >= target) {
            val = (double)i * bucket_width;
            break;
        }
    }
    if (val < min_val)
        val = min_val;
    if (val > max_val)
        val = max_val;
    return val;
}

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
        unsigned b = (unsigned)(dt_ms * 10.0);
        if (b >= FRAME_STATS_HIST_BUCKETS)
            b = FRAME_STATS_HIST_BUCKETS - 1;
        st->frame_dt_hist[b]++;
    }
    st->last_frame_ns = now;
}

void frame_stats_snapshot_wait(struct frame_stats *st, double us)
{
    if (!st)
        return;
    st->snap_wait_sum_us += us;
    st->snap_wait_n++;
    if (st->snap_wait_n == 1 || us < st->snap_wait_min_us)
        st->snap_wait_min_us = us;
    if (us > st->snap_wait_max_us)
        st->snap_wait_max_us = us;
    unsigned b = (unsigned)us;
    if (b >= FRAME_STATS_HIST_BUCKETS)
        b = FRAME_STATS_HIST_BUCKETS - 1;
    st->snap_wait_hist[b]++;
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
    double min_frame = st->frame_n ? st->frame_min_ms : 0.0;
    double avg_frame = st->frame_n ? st->frame_sum_ms / (double)st->frame_n : 0.0;
    double p50_frame = percentile_from_hist(st->frame_dt_hist, st->frame_n, 0.1,
                                            min_frame, st->frame_max_ms, 0.50);
    double p95_frame = percentile_from_hist(st->frame_dt_hist, st->frame_n, 0.1,
                                            min_frame, st->frame_max_ms, 0.95);
    double p99_frame = percentile_from_hist(st->frame_dt_hist, st->frame_n, 0.1,
                                            min_frame, st->frame_max_ms, 0.99);

    double avg_render = st->render_n ? st->render_sum_us / (double)st->render_n : 0.0;

    double min_snap = st->snap_wait_n ? st->snap_wait_min_us : 0.0;
    double avg_snap = st->snap_wait_n ? st->snap_wait_sum_us / (double)st->snap_wait_n : 0.0;
    double p50_snap = percentile_from_hist(st->snap_wait_hist, st->snap_wait_n, 1.0,
                                           min_snap, st->snap_wait_max_us, 0.50);
    double p95_snap = percentile_from_hist(st->snap_wait_hist, st->snap_wait_n, 1.0,
                                           min_snap, st->snap_wait_max_us, 0.95);
    double p99_snap = percentile_from_hist(st->snap_wait_hist, st->snap_wait_n, 1.0,
                                           min_snap, st->snap_wait_max_us, 0.99);

    printf("[stats %7.2fs] frames=%llu skip=%llu fps=%.1f "
           "dt(ms) avg=%.2f min=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f | "
           "snap_wait(us) avg=%.1f min=%.1f p50=%.1f p95=%.1f p99=%.1f max=%.1f | "
           "render(us) avg=%.1f max=%.1f\n",
           secs,
           (unsigned long long)st->frames_drawn,
           (unsigned long long)skipped,
           secs > 0.0 ? (double)st->frame_n / secs : 0.0,
           avg_frame, min_frame, p50_frame, p95_frame, p99_frame, st->frame_max_ms,
           avg_snap, min_snap, p50_snap, p95_snap, p99_snap, st->snap_wait_max_us,
           avg_render, st->render_max_us);
    st->render_max_us = 0.0;
    fflush(stdout);
}
