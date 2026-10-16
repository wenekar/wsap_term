#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "common/types.h"
#include "common/private_loop.h"
#include "common/private_timing.h"
#include "common/input.h"
#if YASL_HAS_WAYLAND
#include "common/private_native_wayland.h"
#endif

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void format_duration(char *buf, size_t buf_sz, double ns) {
    if (ns < 1000.0) {
        snprintf(buf, buf_sz, "%6.2f ns", ns);
    } else if (ns < 1000000.0) {
        snprintf(buf, buf_sz, "%6.2f us", ns / 1e3);
    } else if (ns < 1000000000.0) {
        snprintf(buf, buf_sz, "%6.2f ms", ns / 1e6);
    } else {
        snprintf(buf, buf_sz, "%6.2f s ", ns / 1e9);
    }
}

static void print_bench_row(const char *name, double per_cycle_ns, double total_wall_ms, const char *details) {
    char per_cycle_str[32];
    format_duration(per_cycle_str, sizeof(per_cycle_str), per_cycle_ns);
    printf("%-29s | %10s | %9.2f ms | %s\n", name, per_cycle_str, total_wall_ms, details);
}

static void print_bench_skipped(const char *name, const char *reason) {
    printf("%-29s |    skipped |         - | %s\n", name, reason);
}

#if YASL_HAS_WAYLAND
/* Benchmark 1: Public Wayland Surface Handle Lookup */
static void bench_surface_lookup(void) {
    const int num_surfaces = 16;
    const int lookups = 10000000;
    struct yasl_private_display_spec spec = {
        .app_id = "org.yasl.benchmark",
        .backend = YASL_PRIVATE_BACKEND_WAYLAND,
    };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &spec);
    struct yasl_surface *surfaces[num_surfaces];
    memset(surfaces, 0, sizeof(surfaces));

    if (d) {
        for (int i = 0; i < num_surfaces; i++) {
            struct yasl_private_window_spec sspec = {
                .title = "Benchmark",
                .size = { .x = 320, .y = 240 },
            };
            surfaces[i] = yasl_private_surface_create_toplevel(d, &sspec);
        }
    }

    uint64_t t0 = get_time_ns();
    uintptr_t sum = 0;
    for (int i = 0; i < lookups; i++) {
        struct wl_surface *found = yasl_private_wayland_surface_get_wl_surface(surfaces[i % num_surfaces]);
        sum += (uintptr_t)found;
    }
    uint64_t t1 = get_time_ns();
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = (double)(t1 - t0) / lookups;
    char details[128];
    snprintf(details, sizeof(details), "%d lookups over %d surfaces (checksum=%lx)",
             lookups, num_surfaces, (unsigned long)sum);
    print_bench_row("[1] wayland_surface_lookup", per_cycle, ms, details);

    if (d) {
        for (int i = 0; i < num_surfaces; i++) {
            if (surfaces[i]) yasl_private_surface_destroy(surfaces[i]);
        }
        yasl_private_display_close(d);
    }
}

/* Benchmark 2: Public Surface Geometry & Refresh Timing */
static void bench_output_follow(void) {
    const int iters = 10000000;
    struct yasl_private_display_spec spec = {
        .app_id = "org.yasl.benchmark",
        .backend = YASL_PRIVATE_BACKEND_WAYLAND,
    };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &spec);
    struct yasl_surface *s = NULL;
    if (d) {
        struct yasl_private_window_spec sspec = {
            .title = "Benchmark",
            .size = { .x = 320, .y = 240 },
        };
        s = yasl_private_surface_create_toplevel(d, &sspec);
    }

    uint64_t t0 = get_time_ns();
    uint64_t sum = 0;
    for (int i = 0; i < iters; i++) {
        struct yasl_private_geometry geom = yasl_private_surface_geometry(s);
        struct yasl_private_timing timing = yasl_private_surface_timing(s);
        int dev = yasl_private_scale_to_device(320.0, geom.scale_120);
        sum += (uint64_t)dev + timing.refresh_ns;
    }
    uint64_t t1 = get_time_ns();
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = (double)(t1 - t0) / iters;
    char details[128];
    snprintf(details, sizeof(details), "%d iterations (checksum=%llu)",
             iters, (unsigned long long)sum);
    print_bench_row("[2] surface_geometry_timing", per_cycle, ms, details);

    if (s) yasl_private_surface_destroy(s);
    if (d) yasl_private_display_close(d);
}
#endif

/* Benchmark 3: Input Queue Resizing Throughput */
static void bench_input_queue_resize(void) {
    const int batches = 50000;
    const int size = 2048;

    uint64_t t0 = get_time_ns();
    for (int b = 0; b < batches; b++) {
        struct input_queue q = {0};
        for (int i = 0; i < size; i++) {
            struct yasl_private_event ev = {
                .type = YASL_PRIVATE_EVENT_POINTER_MOTION,
                .pointer = { .pos = { .x = i, .y = b } },
            };
            input_queue_push(&q, &ev);
        }
        input_queue_clear(&q);
    }
    uint64_t t1 = get_time_ns();
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = (double)(t1 - t0) / ((double)batches * size);
    char details[128];
    snprintf(details, sizeof(details), "%d batches x %d events (102.4M total)", batches, size);
    print_bench_row("[3] input_queue_resize", per_cycle, ms, details);
}

/* Benchmark 4: Input Queue Push & Pop Throughput */
static void bench_input_queue_push_pop(void) {
    const int rounds = 20000;
    const int events_per_round = 500;
    struct input_queue q = {0};

    uint64_t t0 = get_time_ns();
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < events_per_round; i++) {
            struct yasl_private_event ev = {
                .type = YASL_PRIVATE_EVENT_POINTER_DOWN,
                .pointer = { .pos = { .x = i, .y = r }, .button = YASL_PRIVATE_BUTTON_MIDDLE },
            };
            input_queue_push(&q, &ev);
        }
        struct yasl_private_event out;
        while (input_queue_pop(&q, &out)) {
            /* consume */
        }
    }
    uint64_t t1 = get_time_ns();
    input_queue_clear(&q);
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = (double)(t1 - t0) / ((double)rounds * events_per_round);
    char details[128];
    snprintf(details, sizeof(details), "%d rounds x %d events (10.0M total)", rounds, events_per_round);
    print_bench_row("[4] input_queue_push_pop", per_cycle, ms, details);
}

/* Benchmark 5: Event Loop Timers Add & Delete */
static uint64_t timer_dummy_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns) {
    (void)ud; (void)id; (void)interval_ns;
    return 0;
}

static void bench_timers(void) {
    const int iters = 2000000;
    struct yasl_loop *loop = yasl_loop_create();

    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iters; i++) {
        yasl_private_timer_id id = yasl_loop_add_timer(loop, 1000000, timer_dummy_cb, NULL);
        yasl_loop_del_timer(loop, id);
    }
    uint64_t t1 = get_time_ns();
    yasl_loop_destroy(loop);
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = (double)(t1 - t0) / iters;
    char details[128];
    snprintf(details, sizeof(details), "%d arm/disarm cycles", iters);
    print_bench_row("[5] timers_add_del", per_cycle, ms, details);
}

/* Benchmark 6: Window Cold Create & Teardown */
static void bench_window_cold_create(void) {
    const int iters = 200;
    struct yasl_private_display_spec dspec = { .app_id = "org.yasl.bench.cold" };
    struct yasl_private_window_spec sspec = {
        .title = "Benchmark Cold",
        .size = { .x = 320, .y = 240 },
    };

    struct yasl_private_display *probe = yasl_private_display_open(NULL, &dspec);
    if (!probe) {
        print_bench_skipped("[6] window_cold_create", "no display server");
        return;
    }
    yasl_private_display_close(probe);

    uint64_t t0 = get_time_ns();
    int count = 0;
    for (int i = 0; i < iters; i++) {
        struct yasl_private_display *d = yasl_private_display_open(NULL, &dspec);
        if (d) {
            struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &sspec);
            if (s) {
                yasl_private_surface_destroy(s);
                count++;
            }
            yasl_private_display_close(d);
        }
    }
    uint64_t t1 = get_time_ns();
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = count ? (double)(t1 - t0) / count : 0.0;
    char details[128];
    snprintf(details, sizeof(details), "%d open/create/destroy/close cycles", count);
    print_bench_row("[6] window_cold_create", per_cycle, ms, details);
}

/* Benchmark 7: Window Warm Create */
static void bench_window_warm_create(void) {
    const int iters = 2000;
    struct yasl_private_display_spec dspec = { .app_id = "org.yasl.bench.warm" };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &dspec);
    if (!d) {
        print_bench_skipped("[7] window_warm_create", "no display server");
        return;
    }

    struct yasl_private_window_spec sspec = {
        .title = "Benchmark Warm",
        .size = { .x = 320, .y = 240 },
    };

    uint64_t t0 = get_time_ns();
    int count = 0;
    for (int i = 0; i < iters; i++) {
        struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &sspec);
        if (s) {
            yasl_private_surface_destroy(s);
            count++;
        }
    }
    uint64_t t1 = get_time_ns();
    yasl_private_display_close(d);
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = count ? (double)(t1 - t0) / count : 0.0;
    char details[128];
    snprintf(details, sizeof(details), "%d surface create/destroy cycles", count);
    print_bench_row("[7] window_warm_create", per_cycle, ms, details);
}

/* Benchmark 8: Window Shutdown (Single) */
static void bench_window_shutdown_single(void) {
    const int iters = 2000;
    struct yasl_private_display_spec dspec = { .app_id = "org.yasl.bench.shutdown_single" };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &dspec);
    if (!d) {
        print_bench_skipped("[8] window_shutdown_single", "no display server");
        return;
    }

    struct yasl_private_window_spec sspec = {
        .title = "Benchmark Shutdown Single",
        .size = { .x = 320, .y = 240 },
    };

    uint64_t destroy_time_ns = 0;
    int count = 0;
    for (int i = 0; i < iters; i++) {
        struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &sspec);
        if (s) {
            uint64_t t0 = get_time_ns();
            yasl_private_surface_destroy(s);
            uint64_t t1 = get_time_ns();
            destroy_time_ns += (t1 - t0);
            count++;
        }
    }
    yasl_private_display_close(d);
    double ms = (double)destroy_time_ns / 1e6;
    double per_cycle = count ? (double)destroy_time_ns / count : 0.0;
    char details[128];
    snprintf(details, sizeof(details), "%d isolated surface destructions", count);
    print_bench_row("[8] window_shutdown_single", per_cycle, ms, details);
}

/* Benchmark 9: Window Shutdown (Bulk / All) */
static void bench_window_shutdown_all(void) {
    const int batches = 50;
    const int batch_size = 64;
    struct yasl_private_display_spec dspec = { .app_id = "org.yasl.bench.shutdown_all" };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &dspec);
    if (!d) {
        print_bench_skipped("[9] window_shutdown_all", "no display server");
        return;
    }

    struct yasl_private_window_spec sspec = {
        .title = "Benchmark Bulk Window",
        .size = { .x = 320, .y = 240 },
    };

    struct yasl_surface *surfaces[64];
    uint64_t destroy_time_ns = 0;
    int total_destroyed = 0;

    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < batch_size; i++) {
            surfaces[i] = yasl_private_surface_create_toplevel(d, &sspec);
        }
        uint64_t t0 = get_time_ns();
        for (int i = 0; i < batch_size; i++) {
            if (surfaces[i]) {
                yasl_private_surface_destroy(surfaces[i]);
                surfaces[i] = NULL;
                total_destroyed++;
            }
        }
        uint64_t t1 = get_time_ns();
        destroy_time_ns += (t1 - t0);
    }
    yasl_private_display_close(d);
    double ms = (double)destroy_time_ns / 1e6;
    double per_cycle = total_destroyed ? (double)destroy_time_ns / total_destroyed : 0.0;
    double per_batch_ns = batches ? (double)destroy_time_ns / batches : 0.0;
    char per_batch_str[32];
    format_duration(per_batch_str, sizeof(per_batch_str), per_batch_ns);
    char details[128];
    snprintf(details, sizeof(details), "%d windows (%d batches x %d, %s/batch)",
             total_destroyed, batches, batch_size, per_batch_str);
    print_bench_row("[9] window_shutdown_all", per_cycle, ms, details);
}

/* Benchmark 10: Window Titlebar Update */
static void bench_window_title_update(void) {
    const int iters = 200000;
    struct yasl_private_display_spec dspec = { .app_id = "org.yasl.bench.title" };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &dspec);
    if (!d) {
        print_bench_skipped("[10] window_titlebar_update", "no display server");
        return;
    }

    struct yasl_private_window_spec sspec = {
        .title = "Initial Title",
        .size = { .x = 320, .y = 240 },
    };
    struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &sspec);
    if (!s) {
        yasl_private_display_close(d);
        print_bench_skipped("[10] window_titlebar_update", "failed to create surface");
        return;
    }

    const char *titles[4] = {
        "YASL Benchmark - State 1",
        "YASL Benchmark - State 2",
        "Document Editor - Untitled",
        "Terminal - Session Active"
    };

    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iters; i++) {
        yasl_private_surface_set_title(s, titles[i & 3]);
    }
    uint64_t t1 = get_time_ns();
    yasl_private_surface_destroy(s);
    yasl_private_display_close(d);
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = (double)(t1 - t0) / iters;
    char details[128];
    snprintf(details, sizeof(details), "%d title changes", iters);
    print_bench_row("[10] window_titlebar_update", per_cycle, ms, details);
}

/* Benchmark 11: Window Resize / Dimension Reallocation */
static void bench_window_resize(void) {
    const int iters = 2000;
    struct yasl_private_display_spec dspec = { .app_id = "org.yasl.bench.resize" };
    struct yasl_private_display *d = yasl_private_display_open(NULL, &dspec);
    if (!d) {
        print_bench_skipped("[11] window_resize_cycle", "no display server");
        return;
    }

    static const struct yasl_private_vector2i sizes[] = {
        { 320, 240 },
        { 640, 480 },
        { 800, 600 },
        { 1024, 768 },
        { 1280, 720 },
        { 1920, 1080 }
    };
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    uint64_t t0 = get_time_ns();
    int count = 0;
    for (int i = 0; i < iters; i++) {
        struct yasl_private_window_spec sspec = {
            .title = "Resize Bench",
            .size = sizes[i % num_sizes],
        };
        struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &sspec);
        if (s) {
            struct yasl_private_geometry geom = yasl_private_surface_geometry(s);
            (void)geom;
            yasl_private_surface_destroy(s);
            count++;
        }
    }
    uint64_t t1 = get_time_ns();
    yasl_private_display_close(d);
    double ms = (double)(t1 - t0) / 1e6;
    double per_cycle = count ? (double)(t1 - t0) / count : 0.0;
    char details[128];
    snprintf(details, sizeof(details), "%d dynamic reallocations across 6 resolutions", count);
    print_bench_row("[11] window_resize_cycle", per_cycle, ms, details);
}

int main(void) {
    printf("=================================== YASL Performance Benchmark ===================================\n");
    printf("%-29s | %10s | %12s | %s\n", "Benchmark", "Per Cycle", "Total Wall", "Operations / Details");
    printf("------------------------------+------------+---------------+--------------------------------------\n");
#if YASL_HAS_WAYLAND
    bench_surface_lookup();
    bench_output_follow();
#endif
    bench_input_queue_resize();
    bench_input_queue_push_pop();
    bench_timers();
    bench_window_cold_create();
    bench_window_warm_create();
    bench_window_shutdown_single();
    bench_window_shutdown_all();
    bench_window_title_update();
    bench_window_resize();
    printf("==================================================================================================\n");
    return 0;
}
