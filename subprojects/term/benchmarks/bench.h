#ifndef TERM_BENCH_H
#define TERM_BENCH_H

#include <stdlib.h>
#include <time.h>

static inline double bench_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline int bench_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static inline void bench_sort(double *samples, int n)
{
    qsort(samples, (size_t)n, sizeof(*samples), bench_cmp_double);
}

#endif
