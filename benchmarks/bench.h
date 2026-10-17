#ifndef WSAP_BENCH_H
#define WSAP_BENCH_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double bench_now(void)
{
    return (double)bench_now_ns() * 1e-9;
}

static inline int bench_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

struct bench_stats {
    double min, median, p99, max;
};

static inline struct bench_stats bench_stats(double *samples, int n)
{
    assert(n > 0);
    qsort(samples, (size_t)n, sizeof(*samples), bench_cmp_double);
    return (struct bench_stats){
        .min = samples[0], .median = samples[n / 2],
        .p99 = samples[((size_t)n * 99 + 99) / 100 - 1], .max = samples[n - 1],
    };
}

/* Run one warmup batch and seven timed batches. Keep the inner loop inline. */
#define BENCH_RUN(name, operations, ...) do { \
    double bench_samples_[7]; \
    for (int bench_sample_ = -1; bench_sample_ < 7; bench_sample_++) { \
        uint64_t bench_start_ = bench_now_ns(); \
        __VA_ARGS__ \
        if (bench_sample_ >= 0) \
            bench_samples_[bench_sample_] = (double)(bench_now_ns() - bench_start_) / (operations); \
    } \
    struct bench_stats bench_st_ = bench_stats(bench_samples_, 7); \
    printf("%-24s %10.2f ns/op (min %.2f, max %.2f)\n", \
           name, bench_st_.median, bench_st_.min, bench_st_.max); \
} while (0)

#endif
