#include <assert.h>
#include <stdint.h>

#include "ui/box.h"
#include "ui/debug.h"
#include "ui/surface.h"
#include "ui/widget_impl.h"
#include "ui/widgets/label.h"
#include "ui/widgets/scrollview.h"

struct host_probe {
    int requests;
};

static void request_frame(void *userdata)
{
    ((struct host_probe *)userdata)->requests++;
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

/* A hover that only changes colors repaints without measuring the rest
 * of the tree again. A hover that changes padding still lays out. */
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
    assert(!((&box.widget)->impl.invalidation &
             (UI_WIDGET_INVALIDATE_MEASURE | UI_WIDGET_INVALIDATE_LAYOUT)));

    int measures = probe.measures;
    ui_widget_invalidate(&meter, UI_WIDGET_INVALIDATE_LAYOUT);
    assert(ui_surface_build_scene(&surface, &scene));
    assert(probe.measures == measures);
    assert(!((&meter)->impl.invalidation & UI_WIDGET_INVALIDATE_LAYOUT));

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
    assert((&box.widget)->impl.invalidation & UI_WIDGET_INVALIDATE_LAYOUT);
    assert(probe.measures == 1);

    ui_surface_set_scale(&surface, (struct css_scale_context){0});
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 50 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(!((&box.widget)->impl.invalidation &
             UI_WIDGET_INVALIDATE_LAYOUT));
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

    /* A scale-only update re-resolves px sizes on the next build
     * without a resize. */
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

    /* Keep the viewport fixed while the font and display scale change. */
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

struct dialog_action_probe {
    int activated_id;
    int actions_seen;
};

static bool on_dialog_action(struct ui_widget *source,
                             const struct ui_action *action,
                             void *userdata)
{
    (void)source;
    struct dialog_action_probe *probe = userdata;
    probe->actions_seen++;
    if (action->kind == UI_ACTION_ACTIVATE) {
        probe->activated_id = action->id;
    }
    return false;
}

static void scratch_arena_and_dialog_test(void)
{
    struct ui_surface surface = {0};
    struct ui_box root = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_root(&surface, UI_WIDGET(&root));
    ui_surface_resize(&surface, (struct vector2i){ .x = 400, .y = 300 });

    /* 1. Scratch alloc test. */
    void *p1 = ui_surface_scratch_alloc(&surface, 64);
    assert(p1 != NULL);
    void *p2 = ui_surface_scratch_alloc(&surface, 128);
    assert(p2 != NULL && p2 > p1);

    ui_surface_scratch_reset(&surface);
    void *p3 = ui_surface_scratch_alloc(&surface, 64);
    assert(p3 == p1);
    ui_surface_scratch_reset(&surface);

    /* 2. Show dialog test. */
    struct dialog_action_probe probe = { .activated_id = -1 };
    struct ui_widget *dialog = ui_dialog_show(&surface, &(struct ui_dialog_desc){
        .title = "Confirm",
        .message = "Do you want to proceed?",
        .buttons = {
            { .text = "Yes", .action_id = 101 },
            { .text = "No",  .action_id = 102 },
        },
        .button_count = 2,
        .on_action = on_dialog_action,
        .userdata = &probe,
    });
    assert(dialog != NULL);
    assert(ui_surface_has_modal(&surface));

    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&surface, &scene));
    assert(dialog->bounds.width > 0 && dialog->bounds.height > 0);

    /* 3. Simulate pressing the focused button (Enter). */
    struct ui_event enter = ui_event_key((struct ui_key_event){
        .key = UI_KEY_ENTER,
    }, 0);
    ui_surface_dispatch(&surface, &enter);

    /* Dialog should have activated ID 101 and automatically closed. */
    assert(probe.activated_id == 101);
    assert(!ui_surface_has_modal(&surface));

    ui_surface_destroy(&surface);
    ui_widget_destroy(UI_WIDGET(&root));
    ui_scene_destroy(&scene);
}

int main(void)
{
    measurement_change_requests_another_frame_test();
    independent_surfaces_test();
    style_application_grow_test();
    hover_restyle_test();
    update_cycle_test();
    zero_size_retains_layout_test();
    destroy_releases_focus_capture_test();
    destroy_detaches_external_root_test();
    set_scale_rescales_styled_size_test();
    nested_scale_change_keeps_text_visible_test();
    zero_surface_test();
    scratch_arena_and_dialog_test();
    return 0;
}
