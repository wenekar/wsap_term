#include <assert.h>
#include <stdint.h>

#include "paint/scene.h"
#include "ui/box.h"
#include "ui/css.h"
#include "ui/surface.h"
#include "ui/widget.h"

/* A box that takes no event is not a pointer target. Give it a handler so
 * that the surface tracks the hover, as a hover area widget does. */
static bool quiet_event(struct ui_widget *widget, const struct ui_event *event,
                        void *userdata)
{
    (void)widget;
    (void)event;
    (void)userdata;
    return false;
}

static bool scene_has_color(const struct ui_scene *scene, uint32_t color)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].color == color)
            return true;
    return false;
}

static uint32_t scene_top_color(const struct ui_scene *scene)
{
    uint32_t color = 0;
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].color >> 24)
            color = scene->ops[i].color;
    return color;
}

/* A box fades from its background color to its hover color. The CSS
 * transition-duration sets the time. */
static void box_hover_transition_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".panel { background: #202020; transition-duration: 100ms; }"
        ".panel:hover { background: #E0E0E0; }"));

    struct ui_box box = {0};
    ui_box_init(&box);
    ui_widget_set_style_identity(&box.widget, "panel", NULL, "box");
    ui_widget_set_event_handler(&box.widget, quiet_event, NULL);
    ui_widget_set_fixed_size(&box.widget, (struct vector2i){ .x = 40, .y = 20 });

    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, &box.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 60 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_color(&scene, 0xFF202020));

    /* A pointer over the box starts the transition. The first frame shows a
     * color between the two, and the deadline asks for the next frame. */
    struct ui_event motion = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = { .x = 10, .y = 10 },
    }, 0);
    /* A box takes no event. The surface still tracks the hover. */
    ui_surface_dispatch(&surface, &motion);
    ui_surface_tick(&surface, 1000);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(ui_surface_next_deadline(&surface) == 1100);
    ui_surface_tick(&surface, 1050);
    assert(ui_surface_build_scene(&surface, &scene));
    uint32_t middle = scene_top_color(&scene);

    assert(middle != 0xFF202020 && middle != 0xFFE0E0E0);

    /* The transition ends at its duration. */
    ui_surface_tick(&surface, 1100);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_color(&scene, 0xFFE0E0E0));
    assert(ui_surface_next_deadline(&surface) == 0);

    /* The pointer leaves, and the color fades back. */
    struct ui_event leave = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_LEAVE,
    }, 1200);
    ui_surface_dispatch(&surface, &leave);
    ui_surface_tick(&surface, 1200);
    assert(ui_surface_build_scene(&surface, &scene));
    ui_surface_tick(&surface, 1250);
    assert(ui_surface_build_scene(&surface, &scene));
    uint32_t back = scene_top_color(&scene);
    assert(back != 0xFFE0E0E0);
    ui_surface_tick(&surface, 1300);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_color(&scene, 0xFF202020));

    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

/* A widget without a transition-duration takes the hover color at once. */
static void box_without_transition_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".plain { background: #202020; }"
        ".plain:hover { background: #E0E0E0; }"));

    struct ui_box box = {0};
    ui_box_init(&box);
    ui_widget_set_style_identity(&box.widget, "plain", NULL, "box");
    ui_widget_set_event_handler(&box.widget, quiet_event, NULL);
    ui_widget_set_fixed_size(&box.widget, (struct vector2i){ .x = 40, .y = 20 });
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, &box.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 60 });

    struct ui_event motion = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = { .x = 10, .y = 10 },
    }, 0);
    /* A box takes no event. The surface still tracks the hover. */
    ui_surface_dispatch(&surface, &motion);
    ui_surface_tick(&surface, 1000);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_color(&scene, 0xFFE0E0E0));
    assert(ui_surface_next_deadline(&surface) == 0);

    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

int main(void)
{
    box_hover_transition_test();
    box_without_transition_test();
    return 0;
}
