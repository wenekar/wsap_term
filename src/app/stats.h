#ifndef APP_STATS_H
#define APP_STATS_H

#include <stdbool.h>

#include "yasl/yasl.h"

struct yasl_loop;
struct window;
struct window_host;

struct app_stats {
    struct yasl_loop *loop;
    const struct window_host *host;
    uint64_t timer_id;
    unsigned interval_ms;
};

bool app_stats_start(struct app_stats *s, struct yasl_loop *loop,
                     const struct window_host *host, unsigned interval_ms);
void app_stats_stop(struct app_stats *s);
void app_stats_report_final(const struct app_stats *s, struct window *last);

#endif
