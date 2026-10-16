#include "font/font.h"
#include "render/scene_paint.h"
#include "ui/internal/tree.h"
#include "ui/css.h"
#include "ui/damage.h"
#include "ui/paint.h"
#include "paint/scene.h"
#include "ui/style_context.h"
#include "ui/surface.h"
#include "paint/text_run.h"
#include "ui/box.h"
#include "ui/widgets/button.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/list.h"
#include "ui/widgets/tabbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void *a, const void *b)
{
    double diff = *(const double *)a - *(const double *)b;
    return (diff > 0) - (diff < 0);
}

struct stats {
    double min;
    double max;
    double med;
    double avg;
};

static struct stats calc_stats(double *samples, int n)
{
    qsort(samples, (size_t)n, sizeof(double), cmp_double);
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        sum += samples[i];
    return (struct stats){
        .min = samples[0],
        .max = samples[n - 1],
        .med = (n % 2 == 1) ? samples[n / 2] : (samples[n / 2 - 1] + samples[n / 2]) * 0.5,
        .avg = sum / n,
    };
}

#define WARMUP_RUNS 3
#define SAMPLE_RUNS 9

static void bench_text_run(void)
{
    const char *ascii = "File  Edit  View  Terminal  Tabs  Help  Configuration  Options  Preferences";
    const char *mixed = "Türkçe: İstanbul — αβγ 日本語 🚀 text run testing mixed utf-8 sequence";
    size_t ascii_len = strlen(ascii);
    size_t mixed_len = strlen(mixed);

    const int iters = 100000;
    volatile int sink = 0;

    for (int w = 0; w < WARMUP_RUNS; w++) {
        for (int i = 0; i < iters; i++) {
            sink += ui_text_run_cells(ascii, ascii_len);
            sink += ui_text_run_cells(mixed, mixed_len);
        }
    }

    double ascii_samples[SAMPLE_RUNS];
    for (int s = 0; s < SAMPLE_RUNS; s++) {
        double t0 = now_sec();
        for (int i = 0; i < iters; i++)
            sink += ui_text_run_cells(ascii, ascii_len);
        ascii_samples[s] = now_sec() - t0;
    }

    double mixed_samples[SAMPLE_RUNS];
    for (int s = 0; s < SAMPLE_RUNS; s++) {
        double t0 = now_sec();
        for (int i = 0; i < iters; i++)
            sink += ui_text_run_cells(mixed, mixed_len);
        mixed_samples[s] = now_sec() - t0;
    }
    (void)sink;

    struct stats st_ascii = calc_stats(ascii_samples, SAMPLE_RUNS);
    struct stats st_mixed = calc_stats(mixed_samples, SAMPLE_RUNS);

    printf("[ui-bench] text_run_cells (ascii x %d):\n", iters);
    printf("           min: %.2f ms | med: %.2f ms | avg: %.2f ms | max: %.2f ms (%.1f Mchars/s med)\n",
           st_ascii.min * 1000.0, st_ascii.med * 1000.0, st_ascii.avg * 1000.0, st_ascii.max * 1000.0,
           ((double)ascii_len * iters / st_ascii.med) / 1e6);
    printf("[ui-bench] text_run_cells (mixed x %d):\n", iters);
    printf("           min: %.2f ms | med: %.2f ms | avg: %.2f ms | max: %.2f ms (%.1f Mchars/s med)\n",
           st_mixed.min * 1000.0, st_mixed.med * 1000.0, st_mixed.avg * 1000.0, st_mixed.max * 1000.0,
           ((double)mixed_len * iters / st_mixed.med) / 1e6);
}

static void bench_css_resolve(void)
{
    struct ui_style_context sctx;
    ui_style_context_init(&sctx);

    const char *test_classes[] = {
        "tab", "tab active", "tab-close", "tab-new",
        "scrollbar", "scrollbar-thumb", "menu-item", "menu-label",
        "tooltip", "unknown-class"
    };
    int n_classes = (int)(sizeof(test_classes) / sizeof(test_classes[0]));

    const int iters = 50000;

    for (int w = 0; w < WARMUP_RUNS; w++) {
        for (int i = 0; i < iters; i++) {
            const char *cls = test_classes[i % n_classes];
            unsigned pseudo = (i & 1) ? CSS_PSEUDO_HOVER : CSS_PSEUDO_NONE;
            struct css_style s = ui_style_context_style(&sctx, cls, pseudo);
            (void)s;
        }
    }

    double samples[SAMPLE_RUNS];
    for (int s = 0; s < SAMPLE_RUNS; s++) {
        double t0 = now_sec();
        for (int i = 0; i < iters; i++) {
            const char *cls = test_classes[i % n_classes];
            unsigned pseudo = (i & 1) ? CSS_PSEUDO_HOVER : CSS_PSEUDO_NONE;
            struct css_style st = ui_style_context_style(&sctx, cls, pseudo);
            (void)st;
        }
        samples[s] = now_sec() - t0;
    }

    struct stats st = calc_stats(samples, SAMPLE_RUNS);
    printf("[ui-bench] css_style_resolve (%d lookups):\n", iters);
    printf("           min: %.2f ms | med: %.2f ms | avg: %.2f ms | max: %.2f ms (%.2f ns/lookup med)\n",
           st.min * 1000.0, st.med * 1000.0, st.avg * 1000.0, st.max * 1000.0,
           (st.med / iters) * 1e9);
}

static void bench_scene_pipeline(void)
{
    struct ui_scene scene = {0};
    ui_scene_init(&scene);

    static char owners[16];
    for (int g = 0; g < 16; g++) {
        ui_scene_begin_node(&scene, &owners[g], (struct vector2i){ .x = g * 50 });
        ui_scene_push_clip(&scene, (struct rect2i){ .size = { .x = 50, .y = 30 } });
        ui_scene_rect(&scene, (struct rect2i){ .size = { .x = 50, .y = 30 } }, 0xFF1E1E2E);
        ui_scene_blend_rect(&scene, (struct rect2i){ .pos = { .x = 2, .y = 2 }, .size = { .x = 46, .y = 26 } }, 0xFF89B4FA, 128);
        ui_scene_text_run(&scene, (struct rect2i){ .pos = { .x = 5, .y = 5 }, .size = { .x = 40, .y = 20 } },
                          (struct ui_text_style){ .advance = 8, .cell_h = 16 }, 0xFFCDD6F4, "TabTitle", 8);
        ui_scene_pop_clip(&scene);
        ui_scene_end_scope(&scene);
    }

    const int iters = 25000;
    static struct ui_damage_tracker tracker;
    struct ui_damage dmg;

    ui_damage_track(&tracker, &scene, &dmg);

    for (int w = 0; w < WARMUP_RUNS; w++) {
        for (int i = 0; i < iters; i++)
            ui_damage_track(&tracker, &scene, &dmg);
    }

    double samples[SAMPLE_RUNS];
    for (int s = 0; s < SAMPLE_RUNS; s++) {
        double t0 = now_sec();
        for (int i = 0; i < iters; i++)
            ui_damage_track(&tracker, &scene, &dmg);
        samples[s] = now_sec() - t0;
    }

    struct stats st = calc_stats(samples, SAMPLE_RUNS);
    printf("[ui-bench] damage_track (%d ops, %d passes):\n", scene.n_ops, iters);
    printf("           min: %.2f ms | med: %.2f ms | avg: %.2f ms | max: %.2f ms (%.2f us/pass med)\n",
           st.min * 1000.0, st.med * 1000.0, st.avg * 1000.0, st.max * 1000.0,
           (st.med / iters) * 1e6);
}

static void bench_layout_tree(void)
{
    struct ui_box root;
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_COLUMN);
    ui_box_set_gap(&root, 4);
    ui_box_set_padding(&root, insets2i_splat(8));

    struct ui_box rows[16];
    struct ui_widget dummy_children[64];

    for (int r = 0; r < 16; r++) {
        ui_box_init(&rows[r]);
        ui_box_set_direction(&rows[r], UI_BOX_ROW);
        ui_box_set_gap(&rows[r], 2);
        ui_widget_add_child(&root.widget, &rows[r].widget);
        for (int c = 0; c < 4; c++) {
            int idx = r * 4 + c;
            ui_widget_init(&dummy_children[idx], NULL, NULL);
            ui_widget_set_fixed_size(&dummy_children[idx], (struct vector2i){ .x = 80, .y = 24 });
            ui_widget_add_child(&rows[r].widget, &dummy_children[idx]);
        }
    }

    const int iters = 25000;
    struct ui_constraints constraints = {
        .min = { .x = 0, .y = 0 },
        .max = { .x = 1024, .y = 768 }
    };
    struct rect2i bounds = {
        .pos = { .x = 0, .y = 0 },
        .size = { .x = 1024, .y = 768 }
    };

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &root.widget);
    ui_surface_resize(&surface, bounds.size);

    for (int w = 0; w < WARMUP_RUNS; w++) {
        for (int i = 0; i < iters; i++) {
            ui_widget_measure(&root.widget, constraints);
            ui_widget_layout(&root.widget, bounds);
        }
    }

    double samples[SAMPLE_RUNS];
    for (int s = 0; s < SAMPLE_RUNS; s++) {
        double t0 = now_sec();
        for (int i = 0; i < iters; i++) {
            ui_widget_measure(&root.widget, constraints);
            ui_widget_layout(&root.widget, bounds);
        }
        samples[s] = now_sec() - t0;
    }

    struct stats st = calc_stats(samples, SAMPLE_RUNS);
    printf("[ui-bench] box_measure_and_layout (tree 16x4, %d passes):\n", iters);
    printf("           min: %.2f ms | med: %.2f ms | avg: %.2f ms | max: %.2f ms (%.2f us/pass med)\n",
           st.min * 1000.0, st.med * 1000.0, st.avg * 1000.0, st.max * 1000.0,
           (st.med / iters) * 1e6);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
}

static void bench_blend_rect(void)
{
    const int w = 1920, h = 1080;
    uint32_t *buf = malloc((size_t)w * h * sizeof(uint32_t));
    if (!buf) return;
    for (int i = 0; i < w * h; i++)
        buf[i] = 0xFF181825;

    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    for (int i = 0; i < 10; i++) {
        struct rect2i r = {
            .pos = { .x = 100 + i * 50, .y = 50 + i * 30 },
            .size = { .x = 800, .y = 400 },
        };
        ui_scene_blend_rect(&scene, r, 0xFF89B4FA, 128);
    }

    for (int k = 0; k < WARMUP_RUNS; k++)
        ui_scene_paint(font_instance_default(), buf, w, h, &scene);

    double samples[SAMPLE_RUNS];
    for (int s = 0; s < SAMPLE_RUNS; s++) {
        double t0 = now_sec();
        ui_scene_paint(font_instance_default(), buf, w, h, &scene);
        samples[s] = now_sec() - t0;
    }

    struct stats st = calc_stats(samples, SAMPLE_RUNS);
    printf("[ui-bench] blend_rect_paint (10 x 800x400 translucent overlays on 1080p):\n");
    printf("           min: %.2f ms | med: %.2f ms | avg: %.2f ms | max: %.2f ms (%.1f Mpx/s med)\n",
           st.min * 1000.0, st.med * 1000.0, st.avg * 1000.0, st.max * 1000.0,
           ((double)(10 * 800 * 400) / st.med) / 1e6);

    ui_scene_destroy(&scene);
    free(buf);
}

int main(void)
{
    printf("=== UI Subsystem Benchmark ===\n");
    bench_text_run();
    bench_css_resolve();
    bench_scene_pipeline();
    bench_layout_tree();
    bench_blend_rect();
    printf("==============================\n");
    return 0;
}
