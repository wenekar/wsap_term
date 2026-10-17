#define _POSIX_C_SOURCE 200809L
#include "bench.h"
#include "font/font.h"
#include "font/glyph_cache.h"
#include "paint/scene.h"
#include "render/scene_paint.h"
#include "ui/damage.h"
#include "ui/surface.h"
#include "ui/ui.h"

#include <errno.h>
#include <math.h>
#include <string.h>

static const struct { const char *name, *env; } scenarios[] = {
    { "idle", "UI_FRAME_BENCH_LIMIT_IDLE_US" },
    { "hover", "UI_FRAME_BENCH_LIMIT_HOVER_US" },
    { "tab-open", "UI_FRAME_BENCH_LIMIT_TAB_OPEN_US" },
    { "menu-20", "UI_FRAME_BENCH_LIMIT_MENU_20_US" },
};
enum { IDLE, HOVER, TAB_OPEN, MENU, SCENARIO_COUNT = 4, WARMUP = 50 };

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_tabbar tabbar;
    struct ui_list list;
    struct ui_scene scene;
    struct ui_damage_tracker damage;
    struct ui_tab_label tabs[51];
    char names[51][24];
    struct vector2i size;
    uint32_t *pixels;
    uint64_t now_ms;
};

static bool setup(struct fixture *f, int scenario)
{
    f->size = (struct vector2i){ .x = scenario == MENU ? 400 : 1920, .y = 600 };
    f->pixels = malloc((size_t)f->size.x * f->size.y * sizeof(*f->pixels));
    if (!f->pixels) return false;
    ui_surface_init(&f->surface, NULL, NULL);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_widget_init(&f->root, NULL, NULL);
    int n = scenario == MENU ? 20 : scenario == TAB_OPEN ? 51 : 12;
    for (int i = 0; i < n; i++) {
        snprintf(f->names[i], sizeof(f->names[i]),
                 scenario == MENU ? "Menu item %02d" : "tab%02d", i);
        f->tabs[i] = (struct ui_tab_label){ .text = f->names[i], .active = i == 0 };
    }
    struct ui_widget *child;
    if (scenario == MENU) {
        ui_list_init(&f->list);
        child = ui_list_widget(&f->list);
    } else {
        ui_tabbar_init(&f->tabbar);
        child = ui_tabbar_widget(&f->tabbar);
    }
    if (!ui_widget_add_child(&f->root, child)) return false;
    if (scenario == MENU) {
        struct ui_list_item items[20];
        for (int i = 0; i < 20; i++)
            items[i] = (struct ui_list_item){ .label = f->names[i], .action = i };
        if (!ui_list_set_items(&f->list, items, 20)) return false;
    } else {
        ui_tabbar_set_tabs(&f->tabbar, f->tabs, scenario == TAB_OPEN ? 50 : 12);
    }
    if (!ui_surface_set_root(&f->surface, &f->root)) return false;
    ui_surface_resize(&f->surface, f->size);
    f->now_ms = 500;
    ui_surface_tick(&f->surface, f->now_ms);
    return true;
}

static bool input(struct fixture *f, int scenario, int frame)
{
    if (scenario == HOVER) {
        struct rect2i tab;
        if (!ui_tabbar_tab_rect(&f->tabbar, frame % 12, &tab)) return false;
        struct ui_event event = ui_event_pointer((struct ui_pointer_event){
            .kind = UI_POINTER_MOTION,
            .pos = { .x = tab.x + tab.width / 2, .y = tab.y + tab.height / 2 },
        }, f->now_ms);
        return ui_surface_dispatch(&f->surface, &event);
    }
    if (scenario == TAB_OPEN && frame % UI_TABBAR_RESIZE_ANIM_MS == 0) {
        if (frame) {
            if (!ui_tabbar_remove_tab(&f->tabbar, 50)) return false;
            ui_tabbar_set_tabs(&f->tabbar, f->tabs, 50);
            f->now_ms += 1000;
            ui_surface_tick(&f->surface, f->now_ms);
            ui_surface_prepare(&f->surface);
        }
        if (!ui_tabbar_insert_tab(&f->tabbar, 50)) return false;
        ui_tabbar_set_tabs(&f->tabbar, f->tabs, 51);
        ui_surface_prepare(&f->surface);
        return ui_tabbar_begin_open(&f->tabbar, 50, f->now_ms);
    }
    return true;
}

static bool run(int scenario, double *samples, int n)
{
    struct fixture f = {0};
    bool ok = false;
    if (!setup(&f, scenario)) goto done;
    for (int i = -WARMUP; i < n; i++) {
        if (!input(&f, scenario, i + WARMUP)) goto done;
        (void)ui_surface_take_frame_request(&f.surface);
        uint64_t start = bench_now_ns();
        ui_surface_tick(&f.surface, ++f.now_ms);
        ui_scene_init(&f.scene);
        if (!ui_surface_build_scene(&f.surface, &f.scene) || !f.scene.n_ops) goto done;
        struct ui_damage damage;
        ui_damage_track(&f.damage, &f.scene, &damage);
        if (!ui_scene_paint(font_instance_default(), f.pixels, f.size.x, f.size.y, &f.scene))
            goto done;
        uint64_t elapsed = bench_now_ns() - start;
        if (i >= 0) samples[i] = (double)elapsed * 1e-3;
    }
    ok = true;
done:
    ui_surface_destroy(&f.surface);
    ui_widget_destroy(&f.root);
    ui_scene_destroy(&f.scene);
    ui_damage_tracker_destroy(&f.damage);
    free(f.pixels);
    return ok;
}

static double number(const char *text, double max)
{
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || !*text || *end || !isfinite(value) || value <= 0 || value > max) {
        fprintf(stderr, "ui-frame-bench: invalid number: %s\n", text);
        exit(2);
    }
    return value;
}

static int frame_count(const char *text)
{
    double n = number(text, 1000000);
    if (n != (int)n) {
        fprintf(stderr, "ui-frame-bench: frames must be an integer\n");
        exit(2);
    }
    return (int)n;
}

static void usage(void)
{
    fprintf(stderr, "usage: ui-frame-bench [--frames N] [--limit SCENARIO=US]...\n");
    for (int s = 0; s < SCENARIO_COUNT; s++)
        fprintf(stderr, "%s%s", s ? ", " : "scenarios: ", scenarios[s].name);
    fputc('\n', stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *env = getenv("UI_FRAME_BENCH_FRAMES");
    int frames = env && *env ? frame_count(env) : 5000;
    double limits[SCENARIO_COUNT] = {0};
    if (getenv("UI_FRAME_BENCH_LIMIT_TAB_OPEN_50_US")) {
        fprintf(stderr, "use UI_FRAME_BENCH_LIMIT_TAB_OPEN_US\n");
        return 2;
    }
    for (int s = 0; s < SCENARIO_COUNT; s++) {
        env = getenv(scenarios[s].env);
        if (env && *env) limits[s] = number(env, INFINITY);
    }
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) usage();
        const char *option = argv[i], *value = argv[++i];
        if (!strcmp(option, "--frames")) {
            frames = frame_count(value);
        } else if (!strcmp(option, "--limit")) {
            const char *eq = strchr(value, '=');
            int s = 0;
            for (; eq && s < SCENARIO_COUNT; s++)
                if ((size_t)(eq - value) == strlen(scenarios[s].name) &&
                    !strncmp(value, scenarios[s].name, (size_t)(eq - value))) break;
            if (!eq || s == SCENARIO_COUNT) usage();
            limits[s] = number(eq + 1, INFINITY);
        } else {
            usage();
        }
    }
    double *samples = malloc((size_t)frames * sizeof(*samples));
    if (!samples) return 2;
    if (!font_system_init()) {
        free(samples);
        return 2;
    }
    int result = 0;
    for (int s = 0; s < SCENARIO_COUNT; s++) {
        if (!run(s, samples, frames)) {
            fprintf(stderr, "%s: frame failed\n", scenarios[s].name);
            result = 2;
            break;
        }
        struct bench_stats st = bench_stats(samples, frames);
        printf("%-10s median %.2f us | p99 %.2f us | max %.2f us", scenarios[s].name,
               st.median, st.p99, st.max);
        if (limits[s]) {
            bool pass = st.median <= limits[s];
            printf(" | limit %g us: %s", limits[s], pass ? "PASS" : "FAIL");
            if (!pass) result = 1;
        }
        putchar('\n');
    }
    font_system_shutdown();
    free(samples);
    return result;
}
