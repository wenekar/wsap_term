#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "ui/box.h"
#include "ui/debug.h"
#include "ui/surface.h"
#include "ui/widget_ops.h"
#include "ui/widgets/label.h"
#include "ui/widgets/scrollview.h"

struct host_probe {
    int requests;
    int primary_copies, primary_pastes, text_inputs;
};

static void request_frame(void *userdata)
{
    ((struct host_probe *)userdata)->requests++;
}

static void copy_primary(void *userdata, const char *text)
{
    (void)text;
    ((struct host_probe *)userdata)->primary_copies++;
}

static void paste_primary(void *userdata)
{
    ((struct host_probe *)userdata)->primary_pastes++;
}

static void set_text_input(void *userdata, const struct ui_text_input *input)
{
    if (input)
        ((struct host_probe *)userdata)->text_inputs++;
}

static void paint_widget(const struct ui_widget *widget,
                         struct ui_paint_context *paint)
{
    uint32_t color = (uintptr_t)widget->userdata;
    assert(ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = 1, .y = 2 }, .size = { .x = 4, .y = 5 },
    }, color));
}

static const struct ui_widget_ops OPS = { .paint = paint_widget };

static void independent_surfaces_test(void)
{
    struct host_probe first_host = {0};
    struct host_probe second_host = {0};
    const struct ui_surface_host host = {
        .request_frame = request_frame,
    };
    struct ui_surface first = {0};
    struct ui_surface second = {0};
    struct ui_widget first_root = {0};
    struct ui_widget second_root = {0};
    ui_widget_init(&first_root, &OPS, (void *)(uintptr_t)0xff000001);
    ui_widget_init(&second_root, &OPS, (void *)(uintptr_t)0xff000002);

    ui_surface_init(&first, &host, &first_host);
    ui_surface_init(&second, &host, &second_host);
    ui_surface_set_root(&first, &first_root);
    ui_surface_set_root(&second, &second_root);
    ui_surface_set_scale(&first, (struct css_scale_context){ .dpi_scale = 1.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    ui_surface_resize(&first, (struct vector2i){ .x = 80, .y = 40 });
    ui_surface_set_scale(&second, (struct css_scale_context){ .dpi_scale = 2.0f,
                                                   .cell_w = 9.0f,
                                                   .cell_h = 18.0f });
    ui_surface_resize(&second, (struct vector2i){ .x = 120, .y = 60 });

    assert(first_host.requests == 1 && second_host.requests == 1);
    assert(ui_surface_viewport(&first).x == 80);
    assert(ui_surface_viewport(&second).x == 120);
    assert(ui_surface_scale(&first).dpi_scale == 1.0f);
    assert(ui_surface_scale(&second).dpi_scale == 2.0f);

    struct ui_scene first_scene = {0};
    struct ui_scene second_scene = {0};
    assert(ui_surface_build_scene(&first, &first_scene));
    assert(ui_surface_build_scene(&second, &second_scene));
    assert(first_scene.n_ops == 1 && second_scene.n_ops == 1);
    assert(first_scene.ops[0].color == 0xff000001);
    assert(second_scene.ops[0].color == 0xff000002);

    assert(ui_surface_frame_pending(&first));
    assert(ui_surface_take_frame_request(&first));
    assert(!ui_surface_frame_pending(&first));
    ui_surface_destroy(&first);

    assert(ui_surface_root(&first) == NULL);
    ui_surface_destroy(&second);

    ui_widget_destroy(&first_root);
    ui_widget_destroy(&second_root);
    ui_scene_destroy(&first_scene);
    ui_scene_destroy(&second_scene);
}

struct flex_probe {
    struct vector2i preferred;
    int measures;
};

static struct vector2i flex_measure(const struct ui_widget *widget,
                                    struct ui_constraints constraints)
{
    struct flex_probe *probe = widget->userdata;
    probe->measures++;
    return ui_constraints_clamp(constraints, probe->preferred);
}

static const struct ui_widget_ops FLEX_OPS = { .measure = flex_measure };

static void primary_and_text_input_host_test(void)
{
    struct host_probe probe = {0};
    const struct ui_surface_host host = {
        .copy_primary = copy_primary,
        .paste_primary = paste_primary,
        .set_text_input = set_text_input,
    };
    struct ui_surface surface = {0};
    ui_surface_init(&surface, &host, &probe);

    assert(ui_surface_can_copy_primary(&surface));
    assert(ui_surface_can_paste_primary(&surface));
    assert(ui_surface_copy_primary(&surface, "primary"));
    assert(ui_surface_paste_primary(&surface));
    struct ui_text_input input = {
        .purpose = UI_TEXT_NORMAL,
        .text = "ab", .length = 2, .revision = 3,
    };
    assert(ui_surface_set_text_input(&surface, &input));
    assert(probe.primary_copies == 1 && probe.primary_pastes == 1);
    assert(probe.text_inputs == 1);

    ui_surface_destroy(&surface);
}

static void style_application_grow_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".grow { width: grow; }"));

    struct ui_box row = {0};
    ui_box_init(&row);

    struct flex_probe fixed_probe = { .preferred = { .x = 20, .y = 10 } };
    struct flex_probe grow_probe = { .preferred = { .x = 20, .y = 10 } };
    struct ui_widget fixed = {0};
    struct ui_widget grow = {0};
    ui_widget_init(&fixed, &FLEX_OPS, &fixed_probe);
    ui_widget_init(&grow, &FLEX_OPS, &grow_probe);
    ui_widget_set_style_identity(&grow, "grow", NULL, NULL);

    assert(ui_widget_add_child(&row.widget, &fixed));
    assert(ui_widget_add_child(&row.widget, &grow));

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, &row.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){0});
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 10 });

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));

    assert(fixed.bounds.width == 20);
    assert(grow.bounds.width == 80);
    assert(grow.bounds.x == 20 && fixed.bounds.x == 0);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static bool scene_has_color(const struct ui_scene *scene, uint32_t color)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].color == color)
            return true;
    return false;
}

static void hover_restyle_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".tint:hover { background: #112233; }"
        ".grow:hover { padding: 0 10px; }"));

    struct ui_box row = {0};
    ui_box_init(&row);
    struct ui_label tint = {0}, grow = {0};
    ui_label_init(&tint);
    ui_label_init(&grow);
    ui_label_set_text(&tint, "ab");
    ui_label_set_text(&grow, "ab");
    ui_widget_set_style_identity(&tint.widget, "tint", NULL, "label");
    ui_widget_set_style_identity(&grow.widget, "grow", NULL, "label");
    struct flex_probe probe = { .preferred = { .x = 30, .y = 10 } };
    struct ui_widget meter = {0};
    ui_widget_init(&meter, &FLEX_OPS, &probe);
    assert(ui_widget_add_child(&row.widget, &tint.widget));
    assert(ui_widget_add_child(&row.widget, &grow.widget));
    assert(ui_widget_add_child(&row.widget, &meter));

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, &row.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    ui_surface_resize(&surface, (struct vector2i){ .x = 200, .y = 40 });
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(!scene_has_color(&scene, 0xFF112233));
    int measures = probe.measures;
    int grow_width = grow.widget.bounds.width;

    ui_widget_set_style_pseudo(&tint.widget, CSS_PSEUDO_HOVER);
    assert(ui_surface_frame_pending(&surface));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene_has_color(&scene, 0xFF112233));
    assert(probe.measures == measures);

    ui_widget_set_style_pseudo(&grow.widget, CSS_PSEUDO_HOVER);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(grow.widget.bounds.width == grow_width + 20);
    assert(meter.bounds.x == grow.widget.bounds.x + grow.widget.bounds.width);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static void update_cycle_test(void)
{
    struct host_probe host = {0};
    const struct ui_surface_host surface_host = {
        .request_frame = request_frame,
    };
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    ui_box_init(&box);
    ui_box_set_direction(&box, UI_BOX_COLUMN);
    struct ui_label label = {0};
    ui_label_init(&label);
    struct flex_probe probe = { .preferred = { .x = 30, .y = 10 } };
    struct ui_widget meter = {0};
    ui_widget_init(&meter, &FLEX_OPS, &probe);
    assert(ui_widget_add_child(&box.widget, &label.widget));
    assert(ui_widget_add_child(&box.widget, &meter));

    ui_surface_init(&surface, &surface_host, &host);
    ui_surface_set_root(&surface, &box.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    ui_surface_resize(&surface, (struct vector2i){ .x = 200, .y = 100 });
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(ui_surface_take_frame_request(&surface));
    assert(label.widget.bounds.height == 16);

    int requests = host.requests;
    ui_widget_set_fixed_size(&label.widget, (struct vector2i){ .x = 80, .y = 24 });
    ui_label_set_text(&label, "changed");
    assert(host.requests == requests + 1);

    assert(ui_surface_build_scene(&surface, &scene));
    assert(label.widget.bounds.width == 80);
    assert(label.widget.bounds.height == 24);
    assert(!ui_widget_needs_layout(&box.widget));

    int measures = probe.measures;
    ui_widget_invalidate(&meter, UI_WIDGET_INVALIDATE_LAYOUT);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(probe.measures == measures);
    assert(!ui_widget_needs_layout(&meter));

    ui_surface_destroy(&surface);
    ui_widget_destroy(&box.widget);
    ui_scene_destroy(&scene);
}

static void zero_size_retains_layout_test(void)
{
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    ui_box_init(&box);
    struct flex_probe probe = { .preferred = { .x = 30, .y = 10 } };
    struct ui_widget meter = {0};
    ui_widget_init(&meter, &FLEX_OPS, &probe);
    assert(ui_widget_add_child(&box.widget, &meter));

    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &box.widget);
    assert(probe.measures == 1);

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    /* A zero viewport keeps the layout pending. ui_widget_needs_layout() also
     * covers measure, but the count above pins that measure already ran. */
    assert(ui_widget_needs_layout(&box.widget));
    assert(probe.measures == 1);

    ui_surface_set_scale(&surface, (struct css_scale_context){0});
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 50 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(!ui_widget_needs_layout(&box.widget));
    assert(meter.bounds.width == 30 && meter.bounds.height == 10);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&box.widget);
    ui_scene_destroy(&scene);
}

struct focus_probe {
    int focus_in;
    int focus_out;
};

static bool focus_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct focus_probe *probe = widget->userdata;
    if (event && event->kind == UI_EVENT_FOCUS_IN)
        probe->focus_in++;
    if (event && event->kind == UI_EVENT_FOCUS_OUT)
        probe->focus_out++;
    return true;
}

static const struct ui_widget_ops FOCUS_OPS = { .event = focus_event };

static void destroy_releases_focus_capture_test(void)
{
    struct host_probe host = {0};
    struct ui_surface surface = {0};
    struct ui_widget root = {0}, child = {0};
    struct focus_probe probe = {0};
    ui_widget_init(&root, NULL, NULL);
    ui_widget_init(&child, &FOCUS_OPS, &probe);
    assert(ui_widget_add_child(&root, &child));

    ui_surface_init(&surface, NULL, &host);
    ui_surface_set_root(&surface, &root);
    ui_surface_focus(&surface, &child);
    ui_widget_capture_pointer(&child);
    assert(probe.focus_in == 1);
    assert(ui_surface_focused(&surface) == &child);
    assert(ui_surface_captured(&surface) == &child);

    ui_surface_destroy(&surface);
    assert(probe.focus_out == 1);
    assert(ui_widget_surface(&root) == NULL && ui_widget_surface(&child) == NULL);
    assert(ui_surface_root(&surface) == NULL);

    ui_widget_destroy(&root);
}

static void destroy_detaches_external_root_test(void)
{
    struct host_probe host = {0};
    struct ui_surface surface = {0};
    struct ui_widget root = {0};
    ui_widget_init(&root, &OPS, (void *)(uintptr_t)0xff000003);
    ui_surface_init(&surface, NULL, &host);
    ui_surface_set_root(&surface, &root);
    assert(ui_widget_surface(&root) == &surface);

    ui_surface_destroy(&surface);
    assert(ui_widget_surface(&root) == NULL);

    ui_widget_destroy(&root);
}

static void set_scale_rescales_styled_size_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".fixed { width: 20px; height: 10px; }"));

    struct ui_surface surface = {0};
    struct ui_box box = {0};
    struct ui_widget child = {0};
    ui_box_init(&box);
    ui_widget_init(&child, NULL, NULL);
    ui_widget_set_style_identity(&child, "fixed", NULL, NULL);
    assert(ui_widget_add_child(&box.widget, &child));
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, &box.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){ .dpi_scale = 1.0f,
                                                   .cell_w = 8.0f,
                                                   .cell_h = 16.0f });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 50 });
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(child.measured_size.x == 20 && child.measured_size.y == 10);
    assert(child.bounds.width == 20 && child.bounds.height == 10);

    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 2.0f, .cell_w = 8.0f, .cell_h = 16.0f });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(child.measured_size.x == 40 && child.measured_size.y == 20);
    assert(child.bounds.width == 40 && child.bounds.height == 20);

    ui_widget_destroy(&box.widget);
    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static void nested_scale_change_keeps_text_visible_test(void)
{
    struct ui_widget root = {0};
    struct ui_box panel = {0}, fields = {0};
    struct ui_label title = {0}, first = {0}, second = {0};
    struct ui_scrollview scroll = {0};
    ui_widget_init(&root, NULL, NULL);
    ui_box_init(&panel);
    ui_box_set_direction(&panel, UI_BOX_COLUMN);
    ui_box_init(&fields);
    ui_box_set_direction(&fields, UI_BOX_COLUMN);
    ui_label_init(&title);
    ui_label_init(&first);
    ui_label_init(&second);
    ui_label_set_text(&title, "Settings");
    ui_label_set_text(&first, "First");
    ui_label_set_text(&second, "Second");
    ui_scrollview_init(&scroll);
    ui_widget_set_style_identity(&scroll.widget, "view", NULL, NULL);
    assert(ui_widget_add_child(&root, &panel.widget));
    assert(ui_widget_add_child(&panel.widget, &title.widget));
    assert(ui_widget_add_child(&panel.widget, &scroll.widget));
    assert(ui_widget_add_child(&scroll.widget, &fields.widget));
    assert(ui_widget_add_child(&fields.widget, &first.widget));
    assert(ui_widget_add_child(&fields.widget, &second.widget));

    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".view { height: grow; }"));
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, &root);
    ui_surface_resize(&surface, (struct vector2i){ .x = 240, .y = 80 });

    const int scales[] = { 1, 2, 1, 2 };
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        int scale = scales[i];
        ui_surface_set_scale(&surface, (struct css_scale_context){
            .dpi_scale = (float)scale,
            .cell_w = (float)(8 * scale),
            .cell_h = (float)(16 * scale),
        });
        for (int frame = 0; frame < 3; frame++) {
            if (frame == 1)
                ui_scrollview_scroll_by(&scroll, 8);
            assert(ui_surface_build_scene(&surface, &scene));
            int text_runs = 0;
            for (int op = 0; op < scene.n_ops; op++)
                if (scene.ops[op].kind == UI_SCENE_OP_TEXT)
                    text_runs++;
            assert(text_runs == 3);
            const struct ui_debug_view view = {
                .root = &root, .surface = &surface, .scene = &scene,
            };
            assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
        }
    }

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
    ui_scene_destroy(&scene);
    css_sheet_clear(&sheet);
}

static void zero_surface_test(void)
{
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    assert(!ui_surface_dispatch(&surface, NULL));
    assert(!ui_surface_tick(&surface, 0));
    assert(!ui_surface_frame_pending(&surface));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene.n_ops == 0);
    ui_surface_destroy(&surface);
    ui_scene_destroy(&scene);
}

/* A widget that fills its placement, takes the pointer, and records events. */
struct layer_probe {
    struct ui_widget widget;
    int presses;
    uint32_t color;
    bool small;
};

static void layer_paint(const struct ui_widget *widget,
                        struct ui_paint_context *paint)
{
    const struct layer_probe *probe = widget->userdata;
    ui_paint_rect(paint, ui_paint_bounds(paint), probe->color);
}

static bool layer_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct layer_probe *probe = (struct layer_probe *)widget->userdata;
    if (event->kind != UI_EVENT_POINTER ||
        event->data.pointer.kind != UI_POINTER_PRESS)
        return false;
    probe->presses++;
    return true;
}

static struct vector2i overlay_measure(const struct ui_widget *widget,
                                       struct ui_constraints constraints)
{
    const struct layer_probe *probe = widget->userdata;
    return probe->small ? (struct vector2i){ .x = 20, .y = 20 }
                        : constraints.max;
}

static const struct ui_widget_ops LAYER_OPS = {
    .name = "overlay-probe",
    .measure = overlay_measure,
    .event = layer_event,
    .paint = layer_paint,
};


struct changing_measure {
    int width;
    bool change_during_measure;
};

static struct vector2i changing_measure_cb(const struct ui_widget *widget,
                                          struct ui_constraints constraints)
{
    struct changing_measure *state = widget->userdata;
    int width = state->width;
    if (state->change_during_measure) {
        state->change_during_measure = false;
        state->width = 40;
        ui_widget_invalidate((struct ui_widget *)widget,
                             UI_WIDGET_INVALIDATE_MEASURE);
    }
    return ui_constraints_clamp(constraints,
        (struct vector2i){ .x = width, .y = 10 });
}

static void measurement_change_requests_another_frame_test(void)
{
    struct ui_surface surface = {0};
    struct ui_box box = {0};
    struct ui_widget child = {0};
    struct changing_measure state = { .width = 20 };
    const struct ui_widget_ops ops = { .measure = changing_measure_cb };
    ui_box_init(&box);
    ui_widget_init(&child, &ops, &state);
    assert(ui_widget_add_child(&box.widget, &child));
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &box.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){0});
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 50 });
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    ui_surface_take_frame_request(&surface);
    state.change_during_measure = true;
    ui_widget_invalidate(&child, UI_WIDGET_INVALIDATE_MEASURE);
    assert(ui_surface_take_frame_request(&surface));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(ui_surface_take_frame_request(&surface));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(child.bounds.width == 40);
    assert(!ui_surface_frame_pending(&surface));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&box.widget);
    ui_scene_destroy(&scene);
}

static void overlay_layer_test(void)
{
    struct ui_surface surface = {0};
    struct ui_box root = {0};
    struct layer_probe first = { .color = 0xFF000001 };
    struct layer_probe second = { .color = 0xFF000002 };
    struct ui_widget child = {0};
    ui_box_init(&root);
    ui_widget_init(&first.widget, &LAYER_OPS, &first);
    ui_widget_init(&second.widget, &LAYER_OPS, &second);
    ui_widget_init(&child, NULL, NULL);
    assert(ui_widget_add_child(&root.widget, &child));

    struct host_probe host = {0};
    const struct ui_surface_host surface_host = { .request_frame = request_frame };
    ui_surface_init(&surface, &surface_host, &host);
    ui_surface_set_root(&surface, &root.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 80 });

    /* A widget that another widget holds is not an overlay. */
    assert(!ui_surface_add_overlay(&surface, &child));
    assert(ui_surface_add_overlay(&surface, &first.widget));
    assert(ui_surface_add_overlay(&surface, &second.widget));
    assert(ui_surface_overlay_count(&surface) == 2);
    assert(ui_surface_overlay_at(&surface, 0) == &first.widget);
    assert(ui_surface_overlay_at(&surface, 1) == &second.widget);

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    /* The overlays are placed against the viewport and painted last. */
    assert(rect2i_eq(first.widget.bounds,
                     (struct rect2i){ .size = { .x = 100, .y = 80 } }));
    assert(rect2i_eq(second.widget.bounds, first.widget.bounds));
    assert(scene.n_ops == 2);
    assert(scene.ops[0].color == 0xFF000001);
    assert(scene.ops[1].color == 0xFF000002);

    /* The topmost overlay takes the press. */
    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 10, .y = 10 },
    }, 0);
    assert(ui_surface_dispatch(&surface, &press));
    assert(second.presses == 1 && first.presses == 0);

    assert(ui_surface_remove_overlay(&surface, &second.widget));
    assert(ui_surface_overlay_count(&surface) == 1);
    assert(!ui_surface_remove_overlay(&surface, &second.widget));
    assert(ui_widget_surface(&second.widget) == NULL);
    press.data.pointer.pos = (struct vector2i){ .x = 20, .y = 20 };
    assert(ui_surface_dispatch(&surface, &press));
    assert(first.presses == 1);

    ui_surface_destroy(&surface);
    assert(ui_surface_overlay_count(&surface) == 0);
    ui_widget_destroy(&root.widget);
    ui_widget_destroy(&first.widget);
    ui_widget_destroy(&second.widget);
    ui_scene_destroy(&scene);
}

static void overlay_growth_test(void)
{
    enum { OVERLAYS = 6 };
    struct ui_box root = {0};
    struct layer_probe layers[OVERLAYS] = {0};
    ui_box_init(&root);
    for (int i = 0; i < OVERLAYS; i++) {
        layers[i].color = 0xFF000100u + (uint32_t)i;
        ui_widget_init(&layers[i].widget, &LAYER_OPS, &layers[i]);
    }

    struct host_probe host = {0};
    const struct ui_surface_host surface_host = { .request_frame = request_frame };
    struct ui_surface surface = {0};
    ui_surface_init(&surface, &surface_host, &host);
    ui_surface_set_root(&surface, &root.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 80 });

    /* The surface grows its list, so it takes more than the first four. */
    for (int i = 0; i < OVERLAYS; i++)
        assert(ui_surface_add_overlay(&surface, &layers[i].widget));
    assert(ui_surface_overlay_count(&surface) == OVERLAYS);
    for (int i = 0; i < OVERLAYS; i++)
        assert(ui_surface_overlay_at(&surface, (size_t)i) ==
               &layers[i].widget);

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene.n_ops == OVERLAYS);
    for (int i = 0; i < OVERLAYS; i++) {
        assert(rect2i_eq(layers[i].widget.bounds,
                         (struct rect2i){ .size = { .x = 100, .y = 80 } }));
        assert(scene.ops[i].color == layers[i].color);
    }

    /* The last overlay in the list takes the press. */
    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 10, .y = 10 },
    }, 0);
    assert(ui_surface_dispatch(&surface, &press));
    assert(layers[OVERLAYS - 1].presses == 1);
    for (int i = 0; i < OVERLAYS - 1; i++)
        assert(layers[i].presses == 0);

    /* A remove in the middle keeps the order of the rest. */
    assert(ui_surface_remove_overlay(&surface, &layers[2].widget));
    assert(ui_widget_surface(&layers[2].widget) == NULL);
    assert(ui_surface_overlay_count(&surface) == OVERLAYS - 1);
    const int order[] = { 0, 1, 3, 4, 5 };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        assert(ui_surface_overlay_at(&surface, i) ==
               &layers[order[i]].widget);

    ui_surface_destroy(&surface);
    assert(ui_surface_overlay_count(&surface) == 0);
    ui_widget_destroy(&root.widget);
    for (int i = 0; i < OVERLAYS; i++)
        ui_widget_destroy(&layers[i].widget);
    ui_scene_destroy(&scene);
}

static void overlay_modal_focus_test(void)
{
    struct layer_probe root_button = { .color = 0xFF000003 };
    struct layer_probe layer = { .color = 0xFF000004, .small = true };
    struct layer_probe row = { .color = 0xFF000005, .small = true };
    struct ui_box root = {0};
    ui_box_init(&root);
    ui_widget_init(&root_button.widget, &LAYER_OPS, &root_button);
    ui_widget_set_focusable(&root_button.widget, true);
    ui_widget_init(&layer.widget, &LAYER_OPS, &layer);
    ui_widget_init(&row.widget, &LAYER_OPS, &row);
    ui_widget_set_focusable(&row.widget, true);
    assert(ui_widget_add_child(&layer.widget, &row.widget));
    assert(ui_widget_add_child(&root.widget, &root_button.widget));
    ui_widget_set_modal(&layer.widget, true);

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &root.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 80 });
    ui_surface_prepare(&surface);
    ui_surface_focus(&surface, &root_button.widget);
    assert(ui_surface_focused(&surface) == &root_button.widget);
    assert(!ui_surface_has_modal(&surface));

    /* A modal overlay takes the focus and masks the root. */
    ui_widget_set_placement(&layer.widget, (struct ui_placement){
        .kind = UI_PLACE_CENTER,
    });
    assert(ui_surface_add_overlay(&surface, &layer.widget));
    assert(ui_surface_has_modal(&surface));
    assert(rect2i_eq(layer.widget.bounds, (struct rect2i){
        .pos = { .x = 40, .y = 0 }, .size = { .x = 20, .y = 20 },
    }));
    assert(ui_surface_focused(&surface) == &row.widget);
    assert(ui_surface_hit_test(&surface, (struct vector2i){ .x = 45, .y = 5 })
           == &row.widget);

    struct ui_event press = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 90, .y = 70 },
    }, 0);
    assert(ui_surface_hit_test(&surface, press.data.pointer.pos) == &layer.widget);
    assert(ui_surface_dispatch(&surface, &press));
    assert(layer.presses == 1 && root_button.presses == 0);
    assert(ui_surface_focused(&surface) == &root_button.widget ||
           ui_surface_focused(&surface) == &row.widget);

    /* The focus goes back to the widget that held it before. */
    assert(ui_surface_remove_overlay(&surface, &layer.widget));
    assert(!ui_surface_has_modal(&surface));
    ui_surface_prepare(&surface);
    assert(ui_surface_focused(&surface) == &root_button.widget);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_widget_destroy(&layer.widget);
}

struct modal_probe {
    struct ui_widget widget;
    int focus_in;
    int focus_out;
};

static bool modal_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct modal_probe *probe = widget->userdata;
    if (event->kind == UI_EVENT_FOCUS_IN) {
        probe->focus_in++;
        return true;
    }
    if (event->kind == UI_EVENT_FOCUS_OUT) {
        probe->focus_out++;
        return true;
    }
    return false;
}

static const struct ui_widget_ops MODAL_OPS = {
    .name = "modal-probe",
    .event = modal_event,
};

static void nested_modal_focus_test(void)
{
    struct modal_probe button = {0}, first_row = {0}, last_row = {0};
    struct modal_probe second_row = {0};
    struct ui_box root = {0};
    struct ui_widget first_modal = {0}, second_modal = {0};
    ui_box_init(&root);
    ui_widget_init(&button.widget, &MODAL_OPS, &button);
    ui_widget_init(&first_row.widget, &MODAL_OPS, &first_row);
    ui_widget_init(&last_row.widget, &MODAL_OPS, &last_row);
    ui_widget_init(&second_row.widget, &MODAL_OPS, &second_row);
    ui_widget_init(&first_modal, NULL, NULL);
    ui_widget_init(&second_modal, NULL, NULL);
    ui_widget_set_focusable(&button.widget, true);
    ui_widget_set_focusable(&first_row.widget, true);
    ui_widget_set_focusable(&last_row.widget, true);
    ui_widget_set_focusable(&second_row.widget, true);
    assert(ui_widget_add_child(&root.widget, &button.widget));
    assert(ui_widget_add_child(&first_modal, &first_row.widget));
    assert(ui_widget_add_child(&first_modal, &last_row.widget));
    assert(ui_widget_add_child(&second_modal, &second_row.widget));
    ui_widget_set_modal(&first_modal, true);
    ui_widget_set_modal(&second_modal, true);

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &root.widget);
    ui_surface_set_scale(&surface, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 80 });
    ui_surface_prepare(&surface);
    ui_surface_focus(&surface, &button.widget);
    assert(button.focus_in == 1);

    /* Each modal takes the focus, and it gives the focus back in reverse
     * order. */
    assert(ui_surface_add_overlay(&surface, &first_modal));
    assert(ui_surface_focused(&surface) == &first_row.widget);
    ui_surface_focus(&surface, &last_row.widget);
    assert(ui_surface_add_overlay(&surface, &second_modal));
    assert(ui_surface_focused(&surface) == &second_row.widget);
    assert(button.focus_out == 1 && last_row.focus_out == 1);

    /* The top modal gives the focus back to the widget that it took it from,
     * not to the widget below the first modal. */
    assert(ui_surface_remove_overlay(&surface, &second_modal));
    assert(ui_surface_focused(&surface) == &last_row.widget);
    assert(ui_surface_has_modal(&surface));
    assert(ui_surface_remove_overlay(&surface, &first_modal));
    assert(ui_surface_focused(&surface) == &button.widget);
    assert(!ui_surface_has_modal(&surface));

    /* A modal can close below the top one. The focus stays in the top modal.
     * The stored target of the closed modal is stale, so the bottom frame
     * gives the focus back to the widget below the first modal. */
    assert(ui_surface_add_overlay(&surface, &first_modal));
    assert(ui_surface_focused(&surface) == &first_row.widget);
    ui_surface_focus(&surface, &last_row.widget);
    assert(ui_surface_add_overlay(&surface, &second_modal));
    assert(ui_surface_focused(&surface) == &second_row.widget);
    assert(ui_surface_remove_overlay(&surface, &first_modal));
    assert(ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == &second_row.widget);
    assert(ui_surface_remove_overlay(&surface, &second_modal));
    assert(!ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == &button.widget);

    /* Dialogs that stop being modal give the focus back in the same way. Both
     * stored targets still live, so the bottom frame wins. */
    assert(ui_surface_add_overlay(&surface, &first_modal));
    assert(ui_surface_focused(&surface) == &first_row.widget);
    ui_surface_focus(&surface, &last_row.widget);
    assert(ui_surface_add_overlay(&surface, &second_modal));
    assert(ui_surface_focused(&surface) == &second_row.widget);
    ui_widget_set_modal(&first_modal, false);
    assert(ui_surface_focused(&surface) == &second_row.widget);
    ui_widget_set_modal(&second_modal, false);
    assert(!ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == &button.widget);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    ui_widget_destroy(&first_modal);
    ui_widget_destroy(&second_modal);
}

struct post_probe {
    int runs;
    bool ran_busy;
    struct ui_widget widget;
};

static void post_run(struct ui_surface *surface, void *userdata)
{
    struct post_probe *probe = userdata;
    probe->runs++;
    probe->ran_busy = ui_surface_busy(surface);
}

static bool post_press(struct ui_widget *widget, const struct ui_event *event)
{
    struct post_probe *probe = widget->userdata;
    if (event->kind != UI_EVENT_KEY)
        return false;
    /* A handler frees nothing while the surface is busy; it posts. */
    assert(ui_surface_busy(ui_widget_surface(widget)));
    assert(ui_surface_post(ui_widget_surface(widget), post_run, probe));
    return true;
}

static bool post_handler(struct ui_widget *widget, const struct ui_event *event,
                         void *userdata)
{
    (void)userdata;
    return post_press(widget, event);
}

static const struct ui_widget_ops POST_OPS = {
    .name = "post-probe",
    .event = post_press,
};

static void surface_post_runs_after_dispatch_test(void)
{
    struct post_probe probe = {0};
    ui_widget_init(&probe.widget, &POST_OPS, &probe);
    ui_widget_set_focusable(&probe.widget, true);

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, &probe.widget);
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 80 });
    ui_surface_prepare(&surface);

    struct ui_event key = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = 'k',
    }, 0);
    assert(ui_surface_dispatch(&surface, &key));
    assert(probe.runs == 1 && !probe.ran_busy);

    /* An idle surface runs the function at once. */
    assert(ui_surface_post(&surface, post_run, &probe));
    assert(probe.runs == 2);

    /* A handler that runs during dispatch posts its free. */
    ui_widget_set_event_handler(&probe.widget, post_handler, &probe);
    assert(ui_surface_dispatch(&surface, &key));
    assert(ui_surface_busy(&surface) == false);
    assert(ui_surface_destroy(&surface));
    /* A destroyed surface holds no root and no overlay. */
    assert(ui_surface_root(&surface) == NULL);
    assert(ui_surface_overlay_count(&surface) == 0);
    ui_widget_destroy(&probe.widget);
}

static bool drop_over_event(struct ui_widget *widget,
                            const struct ui_event *event, void *userdata)
{
    (void)widget;
    (void)userdata;
    return event->kind == UI_EVENT_DRAG_OVER;
}

/* A drag that moves within one widget sends no leave. A drag that moves to
 * another widget, or off the surface, sends exactly one. */
static void drag_hover_leave_once_test(void)
{
    struct ui_surface surface;
    ui_surface_init(&surface, NULL, NULL);
    struct ui_widget a = {0}, b = {0};
    bool enter = false;
    assert(!ui_surface_drag_hover(&surface, &a, &enter) && enter);
    assert(!ui_surface_drag_hover(&surface, &a, &enter) && !enter);
    assert(ui_surface_drag_target(&surface) == &a);
    assert(ui_surface_drag_hover(&surface, &b, &enter) == &a && enter);
    assert(ui_surface_drag_target(&surface) == &b);
    assert(ui_surface_drag_hover(&surface, NULL, &enter) == &b);
    assert(ui_surface_drag_target(&surface) == NULL);
    /* The surface is already left, so a second leave sends nothing. */
    assert(!ui_surface_drag_hover(&surface, NULL, &enter));
    assert(ui_surface_destroy(&surface));
}

/* A drag stores the widget that it is over. The surface clears that widget
 * when it leaves, so the next drag event cannot use freed memory. Run this
 * test under ASan: the stored target is freed before the next event. */
static void drop_target_clears_on_leave_test(void)
{
    struct ui_surface surface;
    ui_surface_init(&surface, NULL, NULL);
    struct ui_widget root;
    ui_widget_init(&root, NULL, NULL);
    assert(ui_surface_set_root(&surface, &root));

    struct ui_widget *target = malloc(sizeof *target);
    assert(target);
    ui_widget_init(target, NULL, NULL);
    ui_widget_set_event_handler(target, drop_over_event, NULL);
    assert(ui_widget_add_child(&root, target));

    /* The drag enters the target. This is the record that the input path
     * keeps for the drag. */
    bool enter = false;
    assert(!ui_surface_drag_hover(&surface, target, &enter) && enter);
    assert(ui_surface_drag_target(&surface) == target);

    /* The widget leaves the surface during the drag. */
    ui_widget_destroy(target);
    free(target);

    /* The next drag event must not touch the freed widget. */
    struct ui_event over = { .kind = UI_EVENT_DRAG_OVER };
    struct ui_widget *stale = ui_surface_drag_target(&surface);
    if (stale)
        ui_widget_event(stale, &over);
    assert(ui_surface_drag_target(&surface) == NULL);

    assert(ui_surface_destroy(&surface));
    ui_widget_destroy(&root);
}

int main(void)
{
    surface_post_runs_after_dispatch_test();
    overlay_layer_test();
    overlay_growth_test();
    overlay_modal_focus_test();
    nested_modal_focus_test();
    measurement_change_requests_another_frame_test();
    independent_surfaces_test();
    primary_and_text_input_host_test();
    style_application_grow_test();
    hover_restyle_test();
    update_cycle_test();
    zero_size_retains_layout_test();
    destroy_releases_focus_capture_test();
    destroy_detaches_external_root_test();
    set_scale_rescales_styled_size_test();
    nested_scale_change_keeps_text_visible_test();
    zero_surface_test();
    drag_hover_leave_once_test();
    drop_target_clears_on_leave_test();
    return 0;
}
