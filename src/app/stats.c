#define _GNU_SOURCE

#include <stdio.h>
#include "app/stats.h"
#include "yasl/yasl.h"
#include "util/time.h"
#include "ui/yasl/stats.h"
#include "ui/yasl/window.h"

static uint64_t tick(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    struct app_stats *s = ud;
    struct window *w = s->host->primary;
    if (!w || !window_surface(w))
        return 0;
    frame_stats_print(window_stats(w), 0);
    return interval_ns;
}

bool app_stats_start(struct app_stats *s, struct yasl_loop *loop,
                     const struct window_host *host, unsigned interval_ms)
{
    *s = (struct app_stats){
        .loop = loop, .host = host, .interval_ms = interval_ms,
    };
    if (!interval_ms)
        return true;
    s->timer_id = yasl_loop_add_timer(loop, MS_TO_NS(interval_ms), tick, s);
    return s->timer_id != 0;
}

void app_stats_stop(struct app_stats *s)
{
    if (s->loop && s->timer_id) {
        yasl_loop_del_timer(s->loop, s->timer_id);
        s->timer_id = 0;
    }
}

void app_stats_report_final(const struct app_stats *s, struct window *last)
{
    if (!s->interval_ms || !last || !window_surface(last))
        return;
    struct vector2i size = window_size(last);
    printf("[final] %dx%d\n", size.x, size.y);
    frame_stats_print(window_stats(last), 0);
}
