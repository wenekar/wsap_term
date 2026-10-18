#include "bench.h"
#include "font/font.h"
#include "paint/scene.h"
#include "paint/text_run.h"
#include "render/scene_paint.h"
#include "ui/box.h"
#include "ui/damage.h"
#include "ui/internal/tree.h"
#include "ui/style_context.h"
#include "ui/surface.h"

#include <string.h>

static void bench_text(void)
{
    static const struct { const char *name, *text; } cases[] = {
        { "text/ascii", "File Edit View Terminal Tabs Help Configuration Options Preferences" },
        { "text/mixed", "Türkçe: İstanbul — αβγ 日本語 🚀 mixed utf-8" },
    };
    volatile int sink = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t len = strlen(cases[c].text);
        BENCH_RUN(cases[c].name, 100000, {
            for (int i = 0; i < 100000; i++)
                sink += ui_text_run_cells(cases[c].text, len);
        });
    }
    (void)sink;
}

static void bench_css(void)
{
    struct ui_style_context context;
    ui_style_context_init(&context);
    const char *classes[] = { "tab", "tab active", "tab-close", "scrollbar-thumb",
                              "menu-item", "menu-label", "tooltip", "unknown-class" };
    BENCH_RUN("css/resolve", 50000, {
        for (int i = 0; i < 50000; i++) {
            struct css_style style = ui_style_context_style(&context,
                classes[i % 8], i & 1 ? CSS_PSEUDO_HOVER : CSS_PSEUDO_NONE);
            __asm__ volatile("" : : "m"(style) : "memory");
        }
    });
    ui_style_context_destroy(&context);
}

static void bench_scene(void)
{
    struct ui_scene scene = {0};
    struct ui_damage_tracker tracker = {0};
    struct ui_damage damage;
    char owners[16];
    ui_scene_init(&scene);
    for (int i = 0; i < 16; i++) {
        struct rect2i bounds = { .size = { .x = 50, .y = 30 } };
        ui_scene_begin_node(&scene, &owners[i], (struct vector2i){ .x = i * 50 });
        ui_scene_push_clip(&scene, bounds);
        ui_scene_rect(&scene, bounds, 0xFF1E1E2E);
        ui_scene_blend_rect(&scene, bounds, 0xFF89B4FA, 128);
        ui_scene_text_run(&scene, bounds, (struct ui_text_style){ .advance = 8, .cell_h = 16 },
                          0xFFCDD6F4, "TabTitle", 8);
        ui_scene_pop_clip(&scene);
        ui_scene_end_scope(&scene);
    }
    BENCH_RUN("scene/damage", 25000, {
        for (int i = 0; i < 25000; i++)
            ui_damage_track(&tracker, &scene, &damage);
    });
    ui_damage_tracker_destroy(&tracker);
    ui_scene_destroy(&scene);
}

static void bench_layout(void)
{
    struct ui_box root, rows[16];
    struct ui_widget children[64];
    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_COLUMN);
    ui_box_set_gap(&root, 4);
    ui_box_set_padding(&root, insets2i_splat(8));
    for (int r = 0; r < 16; r++) {
        ui_box_init(&rows[r]);
        ui_box_set_direction(&rows[r], UI_BOX_ROW);
        ui_box_set_gap(&rows[r], 2);
        ui_widget_add_child(&root.widget, &rows[r].widget);
        for (int c = 0; c < 4; c++) {
            struct ui_widget *child = &children[r * 4 + c];
            ui_widget_init(child, NULL, NULL);
            ui_widget_set_fixed_size(child, (struct vector2i){ .x = 80, .y = 24 });
            ui_widget_add_child(&rows[r].widget, child);
        }
    }
    struct rect2i bounds = { .size = { .x = 1024, .y = 768 } };
    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &root.widget);
    ui_surface_resize(&surface, bounds.size);
    BENCH_RUN("layout/16x4", 1000, {
        for (int i = 0; i < 1000; i++) {
            ui_widget_measure(&root.widget, (struct ui_constraints){ .max = bounds.size });
            ui_widget_layout(&root.widget, bounds);
        }
    });
    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
}

static void bench_blend(void)
{
    struct vector2i size = { .x = 1920, .y = 1080 };
    uint32_t *pixels = calloc((size_t)size.x * size.y, sizeof(*pixels));
    if (!pixels)
        exit(EXIT_FAILURE);
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    for (int i = 0; i < 10; i++)
        ui_scene_blend_rect(&scene, (struct rect2i){
            .pos = { .x = 100 + i * 50, .y = 50 + i * 30 },
            .size = { .x = 800, .y = 400 },
        }, 0xFF89B4FA, 128);
    BENCH_RUN("paint/blend", 1, {
        if (!ui_scene_paint(font_instance_default(), pixels, size.x, size.y, &scene))
            exit(EXIT_FAILURE);
    });
    ui_scene_destroy(&scene);
    free(pixels);
}

int main(void)
{
    bench_text();
    bench_css();
    bench_scene();
    bench_layout();
    bench_blend();
    return 0;
}
