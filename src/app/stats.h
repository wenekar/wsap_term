#ifndef APP_STATS_H
#define APP_STATS_H

#include <stdbool.h>

#include "yasl/yasl.h"

struct yasl_loop;
struct window;
struct window_host;

/* Frame stats of the primary window. */
struct app_stats {
    struct yasl_loop *loop;
    const struct window_host *host;
    uint64_t timer_id;
    unsigned interval_ms;
};

/* Start the frame stats reporter. */
bool app_stats_start(struct app_stats *s, struct yasl_loop *loop,
                     const struct window_host *host, unsigned interval_ms);
/* Stop the frame stats reporter. */
void app_stats_stop(struct app_stats *s);
/* Print the final stats report for the closing window, when stats are on. */
void app_stats_report_final(const struct app_stats *s, struct window *last);

#endif
