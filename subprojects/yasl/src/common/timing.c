#include "timing.h"

uint64_t yasl_private_clock_extend_ms(uint32_t ms, uint64_t now_ns)
{
    uint64_t now_ms = now_ns / 1000000;
    int64_t ahead = (int32_t)(ms - (uint32_t)now_ms);
    if (ahead < 0 && (uint64_t)-ahead > now_ms)
        return (uint64_t)ms * 1000000ull;
    return (uint64_t)((int64_t)now_ms + ahead) * 1000000ull;
}

void yasl_private_clock_presented(struct yasl_private_frame_clock *c, uint64_t now, bool sample)
{
    if (c->last_present_ns) {
        double dt = (double)(now - c->last_present_ns);
        if (sample && dt < 250e6)
            c->refresh_ns += 0.125 * (dt - c->refresh_ns);
    }
    c->last_present_ns = now;
    c->source = YASL_PRIVATE_TIMING_SUBMISSION;
}

void yasl_private_clock_note_resize(struct yasl_private_frame_clock *c, uint64_t dt_ns)
{
    c->resize_count++;
    c->resize_sum_ns += dt_ns;
    if (dt_ns > c->resize_max_ns)
        c->resize_max_ns = dt_ns;
}

void yasl_private_clock_resize_stats(const struct yasl_private_frame_clock *c,
                             struct yasl_private_resize_stats *out)
{
    out->count = c->resize_count;
    out->avg_us = c->resize_count
            ? (double)c->resize_sum_ns / 1e3 / (double)c->resize_count
            : 0.0;
    out->max_us = (double)c->resize_max_ns / 1e3;
    out->minflt = c->resize_minflt;
}

uint64_t yasl_private_clock_next_present_ns(const struct yasl_private_frame_clock *c)
{
    uint64_t next = c->last_present_ns + (uint64_t)c->refresh_ns;
    return (uint64_t)-(c->last_present_ns != 0) & next;
}

uint64_t yasl_private_clock_frame_timestamp_ns(struct yasl_private_frame_clock *c, uint64_t now)
{
    if (!c->last_present_ns || c->refresh_ns <= 0.0) {
        c->frame_time_ns = now;
        return now;
    }
    double k = ((double)now - (double)c->last_present_ns) / c->refresh_ns + 0.5;
    uint64_t grid = c->last_present_ns +
        (uint64_t)((uint64_t)(k > 0.0 ? k : 0.0) * c->refresh_ns);
    if (!c->frame_time_ns) {
        c->frame_time_ns = grid;
        return grid;
    }
    uint64_t t = grid > c->frame_time_ns ? grid : c->frame_time_ns;
    c->frame_time_ns = t;
    return t;
}
