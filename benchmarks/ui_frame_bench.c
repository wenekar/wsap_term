#define _POSIX_C_SOURCE 200809L

#include "font/font.h"
#include "font/glyph_cache.h"
#include "render/scene_paint.h"
#include "ui/css.h"
#include "ui/damage.h"
#include "paint/scene.h"
#include "ui/surface.h"
#include "ui/ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP_FRAMES 50
#define DEFAULT_FRAMES 5000
#define IDLE_TABS 12
#define OPEN_BASE_TABS 50
#define OPEN_SETTLE_MS 1000
#define MAX_TABS 64
#define MENU_ROWS 20
#define TABBAR_VIEWPORT_W 1920
#define TABBAR_VIEWPORT_H 600
#define MENU_VIEWPORT_W 400
#define MENU_VIEWPORT_H 600

static const struct css_scale_context SCALE = {
    .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
};

enum scenario {
    SCENARIO_IDLE,
    SCENARIO_HOVER,
    SCENARIO_TAB_OPEN,
    SCENARIO_MENU_20,
    SCENARIO_COUNT,
};

static const char *const SCENARIO_NAME[SCENARIO_COUNT] = {
    "idle", "hover", "tab-open", "menu-20",
};

static const char *const SCENARIO_DESC[SCENARIO_COUNT] = {
    "12-tab bar, no input",
    "12-tab bar, pointer moves across the tabs each frame",
    "50-tab bar, open animation of tab 51",
    "20-row menu list, repainted each frame",
};

static const char *const SCENARIO_ENV[SCENARIO_COUNT] = {
    "UI_FRAME_BENCH_LIMIT_IDLE_US",
    "UI_FRAME_BENCH_LIMIT_HOVER_US",
    "UI_FRAME_BENCH_LIMIT_TAB_OPEN_US",
    "UI_FRAME_BENCH_LIMIT_MENU_20_US",
};

struct limit {
    bool set;
    double us;
};

struct bench {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_tabbar tabbar;
    struct ui_list list;
    struct ui_scene scene;
    struct ui_damage_tracker damage;
    struct ui_tab_label labels[MAX_TABS];
    char tab_names[MAX_TABS][16];
    int n_tabs;
    struct vector2i viewport;
    uint32_t *pixels;
    uint64_t now_ms;
};

struct run {
    struct bench *b;
    double *samples;
    int n;
    int seen;
};

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

static int cmp_double(const void *a, const void *b)
{
    double diff = *(const double *)a - *(const double *)b;
    return (diff > 0) - (diff < 0);
}

static bool ignore_action(struct ui_widget *source,
                          const struct ui_action *action, void *userdata)
{
    (void)source;
    (void)action;
    (void)userdata;
    return true;
}

static void set_labels(struct bench *b)
{
    for (int i = 0; i < b->n_tabs; i++) {
        snprintf(b->tab_names[i], sizeof(b->tab_names[i]), "tab%02d", i);
        b->labels[i] = (struct ui_tab_label){
            .text = b->tab_names[i], .active = i == 0,
        };
    }
    ui_tabbar_set_tabs(&b->tabbar, b->labels, b->n_tabs);
    ui_surface_prepare(&b->surface);
}

static bool bench_setup(struct bench *b, struct vector2i viewport)
{
    b->viewport = viewport;
    ui_surface_init(&b->surface, NULL, NULL);
    ui_surface_set_sheet(&b->surface, css_default_sheet());
    ui_surface_set_scale(&b->surface, SCALE);
    ui_widget_init(&b->root, NULL, NULL);
    b->pixels = malloc((size_t)viewport.x * (size_t)viewport.y * sizeof(uint32_t));
    return b->pixels != NULL;
}

static void bench_teardown(struct bench *b)
{
    ui_surface_destroy(&b->surface);
    ui_widget_destroy(&b->root);
    ui_scene_destroy(&b->scene);
    ui_damage_tracker_destroy(&b->damage);
    free(b->pixels);
    memset(b, 0, sizeof(*b));
}

static bool setup_tabbar(struct bench *b, int n_tabs)
{
    if (!bench_setup(b, (struct vector2i){ .x = TABBAR_VIEWPORT_W, .y = TABBAR_VIEWPORT_H }))
        return false;
    ui_tabbar_init(&b->tabbar);
    ui_widget_set_action_handler(ui_tabbar_widget(&b->tabbar), ignore_action, NULL);
    if (!ui_widget_add_child(&b->root, ui_tabbar_widget(&b->tabbar)))
        return false;
    if (!ui_surface_set_root(&b->surface, &b->root))
        return false;
    ui_surface_resize(&b->surface, b->viewport);
    b->n_tabs = n_tabs;
    set_labels(b);
    b->now_ms = 500;
    ui_surface_tick(&b->surface, b->now_ms);
    return true;
}

static bool setup_menu(struct bench *b)
{
    static char labels[MENU_ROWS][24];
    struct ui_list_item items[MENU_ROWS];
    if (!bench_setup(b, (struct vector2i){ .x = MENU_VIEWPORT_W, .y = MENU_VIEWPORT_H }))
        return false;
    for (int i = 0; i < MENU_ROWS; i++) {
        snprintf(labels[i], sizeof(labels[i]), "Menu item %02d", i);
        items[i] = (struct ui_list_item){ .label = labels[i], .action = i };
    }
    ui_list_init(&b->list);
    if (!ui_list_set_items(&b->list, items, MENU_ROWS))
        return false;
    if (!ui_widget_add_child(&b->root, ui_list_widget(&b->list)))
        return false;
    if (!ui_surface_set_root(&b->surface, &b->root))
        return false;
    ui_surface_resize(&b->surface, b->viewport);
    ui_surface_prepare(&b->surface);
    b->now_ms = 500;
    return true;
}

static bool timed_frame(struct bench *b, uint64_t now_ms, double *out_us)
{
    (void)ui_surface_take_frame_request(&b->surface);
    double t0 = now_us();
    (void)ui_surface_tick(&b->surface, now_ms);
    ui_scene_init(&b->scene);
    bool built = ui_surface_build_scene(&b->surface, &b->scene);
    struct ui_damage dmg;
    ui_damage_track(&b->damage, &b->scene, &dmg);
    bool painted = ui_scene_paint(font_instance_default(), b->pixels,
                                  b->viewport.x, b->viewport.y, &b->scene);
    double t1 = now_us();
    *out_us = t1 - t0;
    return built && painted && b->scene.n_ops > 0;
}

static bool run_frame(struct run *r)
{
    r->b->now_ms++;
    double us = 0;
    if (!timed_frame(r->b, r->b->now_ms, &us))
        return false;
    if (r->seen >= WARMUP_FRAMES && r->seen - WARMUP_FRAMES < r->n)
        r->samples[r->seen - WARMUP_FRAMES] = us;
    r->seen++;
    return true;
}

static bool run_done(const struct run *r)
{
    return r->seen >= WARMUP_FRAMES + r->n;
}

static bool hover_tab(struct bench *b, int index)
{
    struct rect2i r;
    if (!ui_tabbar_tab_rect(&b->tabbar, index, &r))
        return false;
    struct ui_event e = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION,
        .pos = { .x = r.x + r.width / 2, .y = r.y + r.height / 2 },
    }, b->now_ms);
    return ui_surface_dispatch(&b->surface, &e);
}

static bool run_idle(struct run *r)
{
    while (!run_done(r))
        if (!run_frame(r))
            return false;
    return true;
}

static bool run_hover(struct run *r)
{
    while (!run_done(r)) {
        if (!hover_tab(r->b, r->seen % IDLE_TABS))
            return false;
        if (!run_frame(r))
            return false;
    }
    return true;
}

static bool run_tab_open(struct run *r)
{
    struct bench *b = r->b;
    const int animation_frames = UI_TABBAR_RESIZE_ANIM_MS;
    while (!run_done(r)) {
        if (!ui_tabbar_insert_tab(&b->tabbar, b->n_tabs))
            return false;
        b->n_tabs++;
        set_labels(b);
        if (!ui_tabbar_begin_open(&b->tabbar, b->n_tabs - 1, b->now_ms))
            return false;
        for (int k = 0; k < animation_frames && !run_done(r); k++)
            if (!run_frame(r))
                return false;
        if (!ui_tabbar_remove_tab(&b->tabbar, b->n_tabs - 1))
            return false;
        b->n_tabs--;
        set_labels(b);
        b->now_ms += OPEN_SETTLE_MS;
        (void)ui_surface_tick(&b->surface, b->now_ms);
        ui_surface_prepare(&b->surface);
    }
    return true;
}

static bool run_menu(struct run *r)
{
    while (!run_done(r))
        if (!run_frame(r))
            return false;
    return true;
}

static bool run_scenario(enum scenario s, double *samples, int n)
{
    struct bench *b = calloc(1, sizeof(*b));
    if (!b)
        return false;
    bool ok = false;
    struct run r = { .b = b, .samples = samples, .n = n };
    switch (s) {
    case SCENARIO_IDLE:
        ok = setup_tabbar(b, IDLE_TABS) && run_idle(&r);
        break;
    case SCENARIO_HOVER:
        ok = setup_tabbar(b, IDLE_TABS) && run_hover(&r);
        break;
    case SCENARIO_TAB_OPEN:
        ok = setup_tabbar(b, OPEN_BASE_TABS) && run_tab_open(&r);
        break;
    case SCENARIO_MENU_20:
        ok = setup_menu(b) && run_menu(&r);
        break;
    default:
        break;
    }
    bench_teardown(b);
    free(b);
    return ok;
}

struct stats {
    double median;
    double p99;
    double worst;
};

static struct stats calc_stats(double *samples, int n)
{
    qsort(samples, (size_t)n, sizeof(double), cmp_double);
    int p99_idx = (n * 99 + 99) / 100 - 1;
    if (p99_idx < 0)
        p99_idx = 0;
    if (p99_idx > n - 1)
        p99_idx = n - 1;
    return (struct stats){
        .median = (n % 2 == 1) ? samples[n / 2]
                               : (samples[n / 2 - 1] + samples[n / 2]) * 0.5,
        .p99 = samples[p99_idx],
        .worst = samples[n - 1],
    };
}

static bool parse_int(const char *text, int *out)
{
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text[0] || *end || v <= 0 || v > 1000000)
        return false;
    *out = (int)v;
    return true;
}

static bool parse_us(const char *text, double *out)
{
    char *end = NULL;
    double v = strtod(text, &end);
    if (!text[0] || *end || !isfinite(v) || v <= 0)
        return false;
    *out = v;
    return true;
}

static int scenario_from_name(const char *name)
{
    for (int s = 0; s < SCENARIO_COUNT; s++)
        if (strcmp(name, SCENARIO_NAME[s]) == 0)
            return s;
    return -1;
}

static bool parse_limit_arg(const char *arg, struct limit *limits)
{
    const char *eq = strchr(arg, '=');
    if (!eq)
        return false;
    size_t len = (size_t)(eq - arg);
    char name[32];
    if (len == 0 || len >= sizeof(name))
        return false;
    memcpy(name, arg, len);
    name[len] = '\0';
    int s = scenario_from_name(name);
    if (s < 0)
        return false;
    if (!parse_us(eq + 1, &limits[s].us))
        return false;
    limits[s].set = true;
    return true;
}

static bool load_env_limits(struct limit *limits)
{
    if (getenv("UI_FRAME_BENCH_LIMIT_TAB_OPEN_50_US")) {
        fprintf(stderr, "ui-frame-bench: UI_FRAME_BENCH_LIMIT_TAB_OPEN_50_US "
                        "is now UI_FRAME_BENCH_LIMIT_TAB_OPEN_US\n");
        return false;
    }
    for (int s = 0; s < SCENARIO_COUNT; s++) {
        const char *text = getenv(SCENARIO_ENV[s]);
        if (!text || !*text)
            continue;
        if (!parse_us(text, &limits[s].us)) {
            fprintf(stderr, "ui-frame-bench: bad %s\n", SCENARIO_ENV[s]);
            return false;
        }
        limits[s].set = true;
    }
    return true;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: ui-frame-bench [--frames N] [--limit SCENARIO=US]...\n"
                 "scenarios: idle, hover, tab-open, menu-20\n");
}

int main(int argc, char **argv)
{
    int frames = DEFAULT_FRAMES;
    struct limit limits[SCENARIO_COUNT] = {0};
    const char *env_frames = getenv("UI_FRAME_BENCH_FRAMES");
    if (env_frames && *env_frames && !parse_int(env_frames, &frames)) {
        fprintf(stderr, "ui-frame-bench: bad UI_FRAME_BENCH_FRAMES\n");
        return 2;
    }
    if (!load_env_limits(limits))
        return 2;
    if (!font_system_init()) {
        fprintf(stderr, "ui-frame-bench: font initialization failed\n");
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &frames)) {
                fprintf(stderr, "ui-frame-bench: bad --frames\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            if (!parse_limit_arg(argv[++i], limits)) {
                fprintf(stderr, "ui-frame-bench: bad --limit\n");
                usage(stderr);
                return 2;
            }
        } else {
            usage(stderr);
            return 2;
        }
    }

    printf("=== UI Frame Benchmark (%d timed frames per scenario) ===\n", frames);
    int failed = 0;
    double *samples = malloc((size_t)frames * sizeof(double));
    if (!samples)
        return 2;
    for (int s = 0; s < SCENARIO_COUNT; s++) {
        if (!run_scenario(s, samples, frames)) {
            fprintf(stderr, "[ui-frame-bench] %s: frame failed\n", SCENARIO_NAME[s]);
            free(samples);
            font_system_shutdown();
            return 2;
        }
        struct stats st = calc_stats(samples, frames);
        printf("[ui-frame-bench] %s (%s):\n", SCENARIO_NAME[s], SCENARIO_DESC[s]);
        printf("           median: %.2f us | p99: %.2f us | worst: %.2f us\n",
               st.median, st.p99, st.worst);
        if (!limits[s].set) {
            printf("           limit: none (report only)\n");
            continue;
        }
        bool pass = st.median <= limits[s].us;
        printf("           limit: %g us on median: %s\n",
               limits[s].us, pass ? "PASS" : "FAIL");
        if (!pass)
            failed++;
    }
    printf("==============================\n");
    free(samples);
    font_system_shutdown();
    return failed ? 1 : 0;
}
