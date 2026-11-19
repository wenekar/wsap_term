#include "ui/ui.h"
#include "ui/widget_ops.h"
#include "ui/debug.h"

#include <assert.h>
#include <math.h>

struct probe {
    struct ui_widget widget;
    struct vector2i preferred;
};

static struct vector2i measure(const struct ui_widget *widget,
                               struct ui_constraints constraints)
{
    const struct probe *p = ui_container_of_const(widget, struct probe, widget);
    return ui_constraints_clamp(constraints, p->preferred);
}

static const struct ui_widget_ops OPS = { .name = "probe", .measure = measure };

static void probe_init(struct probe *p, const char *class_name,
                        struct vector2i preferred)
{
    ui_widget_init(&p->widget, &OPS, NULL);
    p->preferred = preferred;
    ui_widget_set_style_identity(&p->widget, class_name, NULL, NULL);
}

static void show(struct ui_surface *surface, struct ui_widget *root,
                  struct css_sheet *sheet, struct vector2i size)
{
    ui_surface_init(surface, NULL, NULL);
    ui_surface_set_sheet(surface, sheet);
    ui_surface_set_root(surface, root);
    ui_surface_resize(surface, size);
}

static void row_sizing_test(void)
{
    struct ui_box row = {0};
    struct probe fixed = {0}, first = {0}, second = {0};
    struct ui_surface surface = {0};
    struct css_sheet sheet = {0};
    ui_box_init(&row);
    ui_widget_set_style_identity(&row.widget, "row", NULL, NULL);
    assert(css_parse(&sheet,
        ".row { padding: 2px; gap: 3px; align-items: center; }"
        ".fixed { width: 20px; height: 10px; }"
        ".grow { width: grow; height: 10px; }"));
    probe_init(&fixed, "fixed", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&first, "grow", (struct vector2i){ .x = 5, .y = 10 });
    probe_init(&second, "grow", (struct vector2i){ .x = 30, .y = 10 });
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &fixed.widget, &first.widget, &second.widget }, 3));
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 100, .y = 30 });
    assert(fixed.widget.bounds.width == 20);
    assert(first.widget.bounds.width == 35 && second.widget.bounds.width == 35);
    assert(first.widget.bounds.x == 25 && second.widget.bounds.x == 63);
    assert(first.widget.bounds.y == 10);
    ui_widget_set_hidden(&fixed.widget, true);
    ui_surface_prepare(&surface);
    assert(first.widget.bounds.x == 2);
    assert(first.widget.bounds.width + second.widget.bounds.width == 93);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}

static void nested_percent_and_limits_test(void)
{
    struct ui_box root = {0}, column = {0};
    struct probe first = {0}, second = {0};
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    ui_box_init(&root);
    ui_box_init(&column);
    ui_widget_set_style_identity(&root.widget, "root", NULL, NULL);
    ui_widget_set_style_identity(&column.widget, "column", NULL, NULL);
    probe_init(&first, "first", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&second, "second", (struct vector2i){ .x = 10, .y = 10 });
    assert(css_parse(&sheet,
        ".root { padding: 10px; align-items: stretch; }"
        ".column { width: 50%; flex-direction: column; align-items: stretch; }"
        ".first { height: grow; max-height: 20px; }"
        ".second { height: grow; min-height: 30px; }"));
    assert(ui_widget_add_child(&root.widget, &column.widget));
    assert(ui_widget_add_children(&column.widget, (struct ui_widget *[]){
        &first.widget, &second.widget }, 2));
    show(&surface, &root.widget, &sheet, (struct vector2i){ .x = 220, .y = 100 });
    assert(column.widget.bounds.width == 100);
    assert(first.widget.bounds.width == 100 && second.widget.bounds.width == 100);
    assert(first.widget.bounds.height == 20 && second.widget.bounds.height == 60);
    ui_surface_resize(&surface, (struct vector2i){ .x = 120, .y = 70 });
    assert(column.widget.bounds.width == 50);
    assert(first.widget.bounds.height == 20 && second.widget.bounds.height == 30);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    css_sheet_clear(&sheet);
}

static void float_edges_test(void)
{
    struct ui_box row = {0};
    struct probe children[3] = {0};
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    ui_box_init(&row);
    assert(css_parse(&sheet, ".grow { width: grow; height: 8.5px; }"));
    for (int i = 0; i < 3; i++) {
        probe_init(&children[i], "grow", (struct vector2i){ .x = 10, .y = 8 });
        assert(ui_widget_add_child(&row.widget, &children[i].widget));
    }
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 130, .y = 20 });
    int end = 0;
    for (int i = 0; i < 3; i++) {
        assert(children[i].widget.bounds.x == end);
        end = rect2i_end(children[i].widget.bounds).x;
        assert(fabsf(children[i].widget.layout_bounds.size.x - 130.0f / 3) < 0.001f);
        assert(children[i].widget.layout_bounds.size.y == 8.5f);
    }
    assert(end == 130);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}

static void fixed_and_fit_overflow_test(void)
{
    struct ui_box row = {0};
    struct probe fixed = {0}, fit = {0};
    struct ui_surface surface = {0};
    struct css_sheet sheet = {0};
    ui_box_init(&row);
    probe_init(&fixed, "fixed", (struct vector2i){ .x = 30, .y = 10 });
    probe_init(&fit, "fit", (struct vector2i){ .x = 60, .y = 10 });
    assert(css_parse(&sheet,
        ".fixed { width: 30px; } .fit { width: fit; min-width: 10px; }"));
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &fixed.widget, &fit.widget }, 2));
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 50, .y = 20 });
    assert(fixed.widget.bounds.width == 30 && fit.widget.bounds.width == 20);
    ui_surface_resize(&surface, (struct vector2i){ .x = 35, .y = 20 });
    assert(fixed.widget.bounds.width == 30 && fit.widget.bounds.width == 10);
    assert(css_parse(&sheet, ".fit { max-width: 0px; min-width: 0px; }"));
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_prepare(&surface);
    assert(fit.widget.bounds.width == 0);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}

static void wrapped_label_test(void)
{
    struct ui_box root = {0}, column = {0};
    struct ui_label label = {0};
    struct ui_button button = {0};
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    ui_box_init(&root);
    ui_box_init(&column);
    ui_box_set_direction(&column, UI_BOX_COLUMN);
    ui_widget_set_style_identity(&column.widget, "column", NULL, NULL);
    ui_label_init(&label);
    assert(ui_label_set_text(&label, "aaaaa bbbbb ccccc"));
    ui_button_init(&button);
    ui_button_set_text(&button, "OK");
    assert(css_parse(&sheet,
        ".column { width: 50%; align-items: stretch; }"));
    assert(ui_widget_add_child(&root.widget, &column.widget));
    assert(ui_widget_add_children(&column.widget, (struct ui_widget *[]){
        &label.widget, &button.widget }, 2));
    show(&surface, &root.widget, &sheet, (struct vector2i){ .x = 200, .y = 100 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(label.widget.bounds.height == 32);
    assert(button.widget.bounds.y >= rect2i_end(label.widget.bounds).y);
    struct ui_debug_view view = { .root = &root.widget, .surface = &surface, .scene = &scene };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    ui_surface_resize(&surface, (struct vector2i){ .x = 100, .y = 160 });
    assert(ui_surface_build_scene(&surface, &scene));
    assert(label.widget.bounds.height == 48);
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    struct vector2i natural = ui_widget_measure(&root.widget,
        (struct ui_constraints){ .max = { .x = 100, .y = 1000 } });
    assert(natural.y >= label.widget.bounds.height + button.widget.bounds.height);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    css_sheet_clear(&sheet);
    ui_scene_destroy(&scene);
}

static void box_border_and_paint_test(void)
{
    struct ui_box box = {0}, stack = {0};
    struct probe child = {0}, stack_child = {0};
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_scene scene = {0};
    struct ui_debug_parts parts = {0};

    ui_box_init(&box);
    ui_widget_set_style_identity(&box.widget, "card", NULL, NULL);
    probe_init(&child, "child", (struct vector2i){ .x = 20, .y = 10 });
    assert(ui_widget_add_child(&box.widget, &child.widget));

    assert(css_parse(&sheet,
        ".card { padding: 4px; border-color: #112233; border-width: 2px; "
        "border-radius: 5px; background: #445566; align-items: stretch; }"));
    show(&surface, &box.widget, &sheet, (struct vector2i){ .x = 100, .y = 40 });
    struct vector2i natural = ui_widget_measure(&box.widget,
        (struct ui_constraints){ .max = { .x = 200, .y = 200 } });
    assert(vector2i_eq(natural, (struct vector2i){ .x = 32, .y = 22 }));
    assert(rect2i_eq(child.widget.bounds, (struct rect2i){
        .pos = { .x = 6, .y = 6 }, .size = { .x = 20, .y = 28 },
    }));
    ui_debug_collect_parts(&box.widget, &parts);
    assert(parts.n == 1 && !strcmp(parts.items[0].name, "content"));
    assert(rect2i_eq(parts.items[0].rect, (struct rect2i){
        .pos = { .x = 6, .y = 6 }, .size = { .x = 88, .y = 28 },
    }));
    assert(ui_surface_build_scene(&surface, &scene));
    assert(scene.n_ops == 2);
    assert(scene.ops[0].kind == UI_SCENE_OP_ROUNDED_RECT &&
           scene.ops[0].radius == 5 && scene.ops[0].color == 0xFF112233u);
    assert(scene.ops[1].kind == UI_SCENE_OP_ROUNDED_RECT &&
           scene.ops[1].radius == 3 && scene.ops[1].color == 0xFF445566u &&
           rect2i_eq(scene.ops[1].rect, (struct rect2i){
               .pos = { .x = 2, .y = 2 }, .size = { .x = 96, .y = 36 },
           }));

    ui_box_init(&stack);
    ui_box_set_direction(&stack, UI_BOX_STACK);
    ui_widget_set_style_identity(&stack.widget, "card", NULL, NULL);
    probe_init(&stack_child, "child", (struct vector2i){ .x = 20, .y = 10 });
    assert(ui_widget_add_child(&stack.widget, &stack_child.widget));
    ui_surface_set_root(&surface, &stack.widget);
    ui_surface_prepare(&surface);
    natural = ui_widget_measure(&stack.widget,
        (struct ui_constraints){ .max = { .x = 200, .y = 200 } });
    assert(vector2i_eq(natural, (struct vector2i){ .x = 32, .y = 22 }));
    assert(rect2i_eq(stack_child.widget.bounds, (struct rect2i){
        .pos = { .x = 6, .y = 6 }, .size = { .x = 88, .y = 28 },
    }));

    ui_debug_parts_free(&parts);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&box.widget);
    ui_widget_destroy(&stack.widget);
    css_sheet_clear(&sheet);
    ui_scene_destroy(&scene);
}

static void min_over_max_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface standalone_surface = {0};
    struct ui_surface row_surface = {0};
    struct ui_box row = {0};
    struct probe standalone = {0}, child = {0};
    assert(css_parse(&sheet, ".overlap { min-width: 80px; max-width: 20px; }"));

    probe_init(&standalone, "overlap", (struct vector2i){ .x = 10, .y = 10 });
    show(&standalone_surface, &standalone.widget, &sheet,
         (struct vector2i){ .x = 400, .y = 100 });
    struct vector2i natural = ui_widget_measure(&standalone.widget,
        (struct ui_constraints){ .max = { .x = 400, .y = 100 } });
    assert(natural.x == 80);

    ui_box_init(&row);
    probe_init(&child, "overlap", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_child(&row.widget, &child.widget));
    show(&row_surface, &row.widget, &sheet,
         (struct vector2i){ .x = 400, .y = 100 });
    assert(child.widget.bounds.width == 80);

    ui_surface_destroy(&standalone_surface);
    ui_surface_destroy(&row_surface);
    ui_widget_destroy(&row.widget);
    ui_widget_destroy(&standalone.widget);
    css_sheet_clear(&sheet);
}

static void margin_moves_child_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface row_surface = {0}, column_surface = {0},
                      stack_surface = {0};
    struct ui_box row = {0}, column = {0}, stack = {0};
    struct probe margined[3] = {0}, next[2] = {0};
    assert(css_parse(&sheet,
        ".row { flex-direction: row; padding: 4px; }"
        ".column { flex-direction: column; padding: 4px; }"
        ".stack { padding: 4px; }"
        ".margined { width: 20px; height: 10px; margin: 6px 2px 4px 3px; }"
        ".next { width: 10px; height: 10px; }"));

    ui_box_init(&row);
    ui_widget_set_style_identity(&row.widget, "row", NULL, NULL);
    probe_init(&margined[0], "margined", (struct vector2i){ .x = 20, .y = 10 });
    probe_init(&next[0], "next", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &margined[0].widget, &next[0].widget }, 2));
    show(&row_surface, &row.widget, &sheet, (struct vector2i){ .x = 100, .y = 40 });
    assert(margined[0].widget.bounds.x == 7 && margined[0].widget.bounds.y == 10);
    assert(next[0].widget.bounds.x == 29);

    ui_box_init(&column);
    ui_box_set_direction(&column, UI_BOX_COLUMN);
    ui_widget_set_style_identity(&column.widget, "column", NULL, NULL);
    probe_init(&margined[1], "margined", (struct vector2i){ .x = 20, .y = 10 });
    probe_init(&next[1], "next", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_children(&column.widget, (struct ui_widget *[]){
        &margined[1].widget, &next[1].widget }, 2));
    show(&column_surface, &column.widget, &sheet,
         (struct vector2i){ .x = 100, .y = 40 });
    assert(margined[1].widget.bounds.x == 7 && margined[1].widget.bounds.y == 10);
    assert(next[1].widget.bounds.y == 24);

    ui_box_init(&stack);
    ui_box_set_direction(&stack, UI_BOX_STACK);
    ui_widget_set_style_identity(&stack.widget, "stack", NULL, NULL);
    probe_init(&margined[2], "margined", (struct vector2i){ .x = 20, .y = 10 });
    assert(ui_widget_add_child(&stack.widget, &margined[2].widget));
    show(&stack_surface, &stack.widget, &sheet,
         (struct vector2i){ .x = 100, .y = 40 });
    assert(margined[2].widget.bounds.x == 7 && margined[2].widget.bounds.y == 10);

    ui_surface_destroy(&row_surface);
    ui_surface_destroy(&column_surface);
    ui_surface_destroy(&stack_surface);
    ui_widget_destroy(&row.widget);
    ui_widget_destroy(&column.widget);
    ui_widget_destroy(&stack.widget);
    css_sheet_clear(&sheet);
}

static void wide_sibling_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box root = {0}, column = {0};
    struct probe cells[40] = {0}, tail = {0};
    assert(css_parse(&sheet,
        ".column { flex-direction: column; }"
        ".cell { width: 10px; height: 5px; }"));

    ui_box_init(&root);
    ui_box_set_direction(&root, UI_BOX_COLUMN);
    ui_widget_set_style_identity(&root.widget, "root", NULL, NULL);
    ui_box_init(&column);
    ui_widget_set_style_identity(&column.widget, "column", NULL, NULL);
    for (size_t i = 0; i < 40; i++) {
        probe_init(&cells[i], "cell", (struct vector2i){ .x = 10, .y = 5 });
        assert(ui_widget_add_child(&column.widget, &cells[i].widget));
    }
    probe_init(&tail, "cell", (struct vector2i){ .x = 10, .y = 5 });
    assert(ui_widget_add_children(&root.widget, (struct ui_widget *[]){
        &column.widget, &tail.widget }, 2));

    show(&surface, &root.widget, &sheet, (struct vector2i){ .x = 100, .y = 300 });
    assert(column.widget.bounds.height == 200);
    assert(cells[39].widget.bounds.y == 195);
    assert(tail.widget.bounds.y == 200);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    css_sheet_clear(&sheet);
}

static void indefinite_percent_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct probe half = {0};
    assert(css_parse(&sheet, ".half { width: 50%; height: 50%; }"));

    probe_init(&half, "half", (struct vector2i){ .x = 40, .y = 10 });
    show(&surface, &half.widget, &sheet, (struct vector2i){ .x = 100, .y = 100 });
    struct vector2i natural = ui_widget_measure(&half.widget,
        (struct ui_constraints){ .max = { .x = INT_MAX, .y = INT_MAX } });
    assert(vector2i_eq(natural, (struct vector2i){ .x = 40, .y = 10 }));

    ui_surface_destroy(&surface);
    ui_widget_destroy(&half.widget);
    css_sheet_clear(&sheet);
}

static void root_measure_only_test(void)
{
    struct ui_surface surface = {0};
    struct probe root = {0}, child = {0};
    probe_init(&root, "root", (struct vector2i){ .x = 40, .y = 20 });
    probe_init(&child, "child", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_child(&root.widget, &child.widget));

    show(&surface, &root.widget, NULL, (struct vector2i){ .x = 100, .y = 50 });
    assert(rect2i_eq(child.widget.bounds, (struct rect2i){
        .size = { .x = 100, .y = 50 },
    }));

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
}

static void fixed_size_limits_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface standalone_surface = {0}, row_surface = {0};
    struct ui_box row = {0};
    struct probe standalone = {0}, child = {0};
    assert(css_parse(&sheet,
        ".bounded { max-width: 10px; min-height: 30px; }"));

    probe_init(&standalone, "bounded", (struct vector2i){ .x = 5, .y = 5 });
    ui_widget_set_fixed_size(&standalone.widget,
                             (struct vector2i){ .x = 20, .y = 10 });
    show(&standalone_surface, &standalone.widget, &sheet,
         (struct vector2i){ .x = 400, .y = 400 });
    struct vector2i natural = ui_widget_measure(&standalone.widget,
        (struct ui_constraints){ .max = { .x = 400, .y = 400 } });
    assert(vector2i_eq(natural, (struct vector2i){ .x = 10, .y = 30 }));

    ui_box_init(&row);
    probe_init(&child, "bounded", (struct vector2i){ .x = 5, .y = 5 });
    ui_widget_set_fixed_size(&child.widget,
                             (struct vector2i){ .x = 20, .y = 10 });
    assert(ui_widget_add_child(&row.widget, &child.widget));
    show(&row_surface, &row.widget, &sheet,
         (struct vector2i){ .x = 400, .y = 400 });
    assert(child.widget.bounds.width == 10 && child.widget.bounds.height == 30);

    ui_surface_destroy(&standalone_surface);
    ui_surface_destroy(&row_surface);
    ui_widget_destroy(&row.widget);
    ui_widget_destroy(&standalone.widget);
    css_sheet_clear(&sheet);
}

static void justify_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_box rows[3] = {0};
    struct probe cells[3] = {0};
    struct ui_surface surfaces[3] = {0};
    static const char *const classes[3] = {
        "justify-start", "justify-center", "justify-end",
    };
    assert(css_parse(&sheet,
        ".justify-start { justify-content: start; }"
        ".justify-center { justify-content: center; }"
        ".justify-end { justify-content: end; }"
        ".cell { width: 10px; height: 10px; }"));

    for (size_t i = 0; i < 3; i++) {
        ui_box_init(&rows[i]);
        ui_widget_set_style_identity(&rows[i].widget, classes[i], NULL, NULL);
        probe_init(&cells[i], "cell", (struct vector2i){ .x = 10, .y = 10 });
        assert(ui_widget_add_child(&rows[i].widget, &cells[i].widget));
        show(&surfaces[i], &rows[i].widget, &sheet,
             (struct vector2i){ .x = 100, .y = 40 });
    }
    assert(cells[0].widget.bounds.x == 0);
    assert(cells[1].widget.bounds.x == 45);
    assert(cells[2].widget.bounds.x == 90);

    for (size_t i = 0; i < 3; i++) {
        ui_surface_destroy(&surfaces[i]);
        ui_widget_destroy(&rows[i].widget);
    }
    css_sheet_clear(&sheet);
}

static void align_and_stack_placement_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_box rows[2] = {0}, stacks[2] = {0};
    struct probe cells[4] = {0};
    struct ui_surface surfaces[4] = {0};
    static const char *const row_classes[2] = {
        "align-center", "align-end",
    };
    static const char *const stack_classes[2] = {
        "stack-center", "stack-end",
    };
    assert(css_parse(&sheet,
        ".align-center { align-items: center; }"
        ".align-end { align-items: end; }"
        ".stack-center { align-items: center; }"
        ".stack-end { align-items: end; }"
        ".cell { width: 10px; height: 10px; }"
        ".wide { width: 20px; height: 10px; }"));

    for (size_t i = 0; i < 2; i++) {
        ui_box_init(&rows[i]);
        ui_widget_set_style_identity(&rows[i].widget, row_classes[i], NULL, NULL);
        probe_init(&cells[i], "cell", (struct vector2i){ .x = 10, .y = 10 });
        assert(ui_widget_add_child(&rows[i].widget, &cells[i].widget));
        show(&surfaces[i], &rows[i].widget, &sheet,
             (struct vector2i){ .x = 100, .y = 40 });

        ui_box_init(&stacks[i]);
        ui_box_set_direction(&stacks[i], UI_BOX_STACK);
        ui_widget_set_style_identity(&stacks[i].widget, stack_classes[i],
                                     NULL, NULL);
        probe_init(&cells[i + 2], "wide", (struct vector2i){ .x = 20, .y = 10 });
        assert(ui_widget_add_child(&stacks[i].widget, &cells[i + 2].widget));
        show(&surfaces[i + 2], &stacks[i].widget, &sheet,
             (struct vector2i){ .x = 100, .y = 40 });
    }
    assert(cells[0].widget.bounds.y == 15);
    assert(cells[1].widget.bounds.y == 30);
    assert(cells[2].widget.bounds.x == 40 && cells[2].widget.bounds.y == 15);
    assert(cells[3].widget.bounds.x == 80 && cells[3].widget.bounds.y == 30);

    for (size_t i = 0; i < 4; i++)
        ui_surface_destroy(&surfaces[i]);
    for (size_t i = 0; i < 2; i++) {
        ui_widget_destroy(&rows[i].widget);
        ui_widget_destroy(&stacks[i].widget);
    }
    css_sheet_clear(&sheet);
}


static void fractional_scale_rounding_test(void)
{
    static const float scales[] = { 1.0f, 1.25f, 1.5f, 2.0f };
    static const int values[] = { 1, 3, 5, 7 };

    /* One length rounds to the nearest pixel, and a rectangle rounds each
       edge. The CSS int resolver, the fixed helper, and the layout must use
       the same rule at every scale. */
    for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
        struct css_scale_context ctx = {
            .dpi_scale = scales[s],
            .cell_w = 8.0f * scales[s],
            .cell_h = 16.0f * scales[s],
        };
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            struct css_value v = {
                .val = (float)values[i],
                .unit = CSS_UNIT_PX,
            };
            int expected = (int)lroundf((float)values[i] * scales[s]);
            assert(ui_px(scales[s], values[i]) == expected);
            assert(ui_css_resolve_int(v, 0, 0, &ctx, true) == expected);
            assert(ui_ctx_size(&ctx, v, 0) == expected);
        }
    }

    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".row { padding: 1.5px; align-items: stretch; }"
        ".half { width: 2.5px; }"
        ".tail { width: 5.5px; }"));

    struct ui_box row = {0};
    struct probe half = {0}, tail = {0};
    ui_box_init(&row);
    ui_widget_set_style_identity(&row.widget, "row", NULL, NULL);
    probe_init(&half, "half", (struct vector2i){ .x = 0, .y = 4 });
    probe_init(&tail, "tail", (struct vector2i){ .x = 0, .y = 4 });
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &half.widget, &tail.widget }, 2));

    for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
        float scale = scales[s];
        struct css_scale_context ctx = {
            .dpi_scale = scale,
            .cell_w = 8.0f * scale,
            .cell_h = 16.0f * scale,
        };
        struct ui_surface surface = {0};
        show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 20, .y = 10 });
        ui_surface_set_scale(&surface, ctx);

        int pad = (int)lroundf(1.5f * scale);
        float mid_edge = (float)pad + 2.5f * scale;
        int mid = (int)lroundf(mid_edge);
        int end = (int)lroundf(mid_edge + 5.5f * scale);
        assert(half.widget.bounds.x == pad);
        assert(half.widget.bounds.width == mid - pad);
        assert(tail.widget.bounds.x == mid);
        assert(tail.widget.bounds.width == end - mid);
        assert(rect2i_end(half.widget.bounds).x == tail.widget.bounds.x);
        assert(half.widget.layout_bounds.pos.x == (float)pad);
        assert(half.widget.layout_bounds.size.x == 2.5f * scale);
        assert(tail.widget.layout_bounds.pos.x == mid_edge);
        assert(tail.widget.layout_bounds.size.x == 5.5f * scale);

        struct vector2i natural = ui_widget_measure(&row.widget,
            (struct ui_constraints){ .max = { .x = 400, .y = 100 } });
        assert(natural.x == (int)lroundf(2.0f * (float)pad + 8.0f * scale));
        ui_surface_destroy(&surface);
    }

    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}


static void stack_keeps_direction_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box stack = {0};
    struct probe first = {0}, second = {0};
    assert(css_parse(&sheet,
        ".stack { flex-direction: column; }"
        ".small { width: 10px; height: 10px; }"
        ".large { width: 20px; height: 20px; }"));
    ui_box_init(&stack);
    ui_box_set_direction(&stack, UI_BOX_STACK);
    ui_widget_set_style_identity(&stack.widget, "stack", NULL, NULL);
    probe_init(&first, "small", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&second, "large", (struct vector2i){ .x = 20, .y = 20 });
    assert(ui_widget_add_children(&stack.widget, (struct ui_widget *[]){
        &first.widget, &second.widget }, 2));
    show(&surface, &stack.widget, &sheet, (struct vector2i){ .x = 100, .y = 100 });
    assert(rect2i_eq(first.widget.bounds, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 10, .y = 10 },
    }));
    assert(rect2i_eq(second.widget.bounds, (struct rect2i){
        .pos = { .x = 0, .y = 0 }, .size = { .x = 20, .y = 20 },
    }));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&stack.widget);
    css_sheet_clear(&sheet);
}

static void percent_margin_and_limits_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box row = {0}, stack = {0};
    struct probe clamped_min = {0}, clamped_max = {0}, stack_child = {0};
    assert(css_parse(&sheet,
        ".row { align-items: start; }"
        ".clamped-min { height: 10px; width: 50%; margin: 0 10px; min-width: 70px; }"
        ".clamped-max { height: 10px; width: 100%; max-width: 60px; }"
        ".stack-child { height: 10px; width: 100%; margin: 0 10px; }"));

    ui_box_init(&row);
    ui_widget_set_style_identity(&row.widget, "row", NULL, NULL);
    probe_init(&clamped_min, "clamped-min", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&clamped_max, "clamped-max", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &clamped_min.widget, &clamped_max.widget }, 2));
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 100, .y = 40 });
    /* The percentage names the space inside the margin: 50% of 80 is 40, and
       the minimum raises it to 70. The second child caps at 60. */
    assert(clamped_min.widget.bounds.width == 70);
    assert(clamped_max.widget.bounds.width == 60);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);

    ui_box_init(&stack);
    ui_box_set_direction(&stack, UI_BOX_STACK);
    probe_init(&stack_child, "stack-child", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_child(&stack.widget, &stack_child.widget));
    show(&surface, &stack.widget, &sheet, (struct vector2i){ .x = 100, .y = 40 });
    assert(stack_child.widget.bounds.width == 80);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&stack.widget);
    css_sheet_clear(&sheet);
}

static void flex_wrap_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box row = {0};
    struct probe cells[3] = {0};
    assert(css_parse(&sheet,
        ".row { flex-wrap: wrap; }"
        ".cell { width: 40px; height: 10px; }"));
    ui_box_init(&row);
    ui_widget_set_style_identity(&row.widget, "row", NULL, NULL);
    for (size_t i = 0; i < 3; i++) {
        probe_init(&cells[i], "cell", (struct vector2i){ .x = 40, .y = 10 });
        assert(ui_widget_add_child(&row.widget, &cells[i].widget));
    }
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 100, .y = 40 });
    assert(cells[0].widget.bounds.x == 0 && cells[0].widget.bounds.y == 0);
    assert(cells[1].widget.bounds.x == 40 && cells[1].widget.bounds.y == 0);
    assert(cells[2].widget.bounds.x == 0 && cells[2].widget.bounds.y == 10);
    ui_surface_resize(&surface, (struct vector2i){ .x = 200, .y = 40 });
    assert(cells[2].widget.bounds.x == 80 && cells[2].widget.bounds.y == 0);
    struct vector2i natural = ui_widget_measure(&row.widget,
        (struct ui_constraints){ .max = { .x = 100, .y = 200 } });
    assert(vector2i_eq(natural, (struct vector2i){ .x = 80, .y = 20 }));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}

static void weighted_flex_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box row = {0};
    struct probe grow_one = {0}, grow_three = {0};
    assert(css_parse(&sheet,
        ".grow-one { width: grow; flex-grow: 1; }"
        ".grow-three { width: grow; flex-grow: 3; }"
        ".shrink-one { width: fit; flex-shrink: 1; }"
        ".shrink-three { width: fit; flex-shrink: 3; }"));
    ui_box_init(&row);
    probe_init(&grow_one, "grow-one", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&grow_three, "grow-three", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &grow_one.widget, &grow_three.widget }, 2));
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 100, .y = 20 });
    assert(grow_one.widget.bounds.width == 25);
    assert(grow_three.widget.bounds.width == 75);
    ui_widget_set_hidden(&grow_three.widget, true);
    ui_widget_set_hidden(&grow_one.widget, true);

    struct probe shrink_one = {0}, shrink_three = {0};
    probe_init(&shrink_one, "shrink-one", (struct vector2i){ .x = 80, .y = 10 });

    probe_init(&shrink_three, "shrink-three", (struct vector2i){ .x = 80, .y = 10 });

    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &shrink_one.widget, &shrink_three.widget }, 2));
    ui_surface_prepare(&surface);
    assert(shrink_one.widget.bounds.width == 65);
    assert(shrink_three.widget.bounds.width == 35);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}

static void justify_spaces_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box rows[4] = {0};
    struct probe cells[4] = {0};
    static const char *const classes[4] = {
        "between", "around", "evenly", "between-single",
    };
    assert(css_parse(&sheet,
        ".between { justify-content: space-between; }"
        ".around { justify-content: space-around; }"
        ".evenly { justify-content: space-evenly; }"
        ".between-single { justify-content: space-between; }"
        ".cell { width: 10px; height: 10px; }"));
    for (size_t i = 0; i < 3; i++) {
        ui_box_init(&rows[i]);
        ui_widget_set_style_identity(&rows[i].widget, classes[i], NULL, NULL);
        probe_init(&cells[i], "cell", (struct vector2i){ .x = 10, .y = 10 });
        assert(ui_widget_add_child(&rows[i].widget, &cells[i].widget));
        show(&surface, &rows[i].widget, &sheet, (struct vector2i){ .x = 100, .y = 20 });
        ui_surface_destroy(&surface);
    }
    ui_box_init(&rows[3]);
    ui_widget_set_style_identity(&rows[3].widget, "between-single", NULL, NULL);
    probe_init(&cells[3], "cell", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_child(&rows[3].widget, &cells[3].widget));
    show(&surface, &rows[3].widget, &sheet, (struct vector2i){ .x = 100, .y = 20 });
    assert(fabsf(cells[3].widget.layout_bounds.pos.x - 0.0f) < 0.01f);
    ui_surface_destroy(&surface);
    for (size_t i = 0; i < 3; i++)
        ui_widget_destroy(&rows[i].widget);
    ui_widget_destroy(&rows[3].widget);
    css_sheet_clear(&sheet);
}

static void align_self_test(void)
{
    struct css_sheet sheet = {0};
    struct ui_surface surface = {0};
    struct ui_box row = {0};
    struct probe start = {0}, center = {0}, end = {0}, stretch = {0};
    assert(css_parse(&sheet,
        ".row { height: 40px; align-items: start; }"
        ".cell { width: 10px; height: 10px; }"
        ".center { align-self: center; }"
        ".end { align-self: end; }"
        ".stretch { align-self: stretch; height: auto; }"));
    ui_box_init(&row);
    ui_widget_set_style_identity(&row.widget, "row", NULL, NULL);
    probe_init(&start, "cell", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&center, "cell center", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&end, "cell end", (struct vector2i){ .x = 10, .y = 10 });
    probe_init(&stretch, "cell stretch", (struct vector2i){ .x = 10, .y = 10 });
    assert(ui_widget_add_children(&row.widget, (struct ui_widget *[]){
        &start.widget, &center.widget, &end.widget, &stretch.widget }, 4));
    show(&surface, &row.widget, &sheet, (struct vector2i){ .x = 100, .y = 40 });
    assert(start.widget.bounds.y == 0);
    assert(center.widget.bounds.y == 15);
    assert(end.widget.bounds.y == 30);
    assert(stretch.widget.bounds.y == 0 && stretch.widget.bounds.height == 40);
    ui_surface_destroy(&surface);
    ui_widget_destroy(&row.widget);
    css_sheet_clear(&sheet);
}

int main(void)
{
    stack_keeps_direction_test();
    percent_margin_and_limits_test();
    flex_wrap_test();
    weighted_flex_test();
    justify_spaces_test();
    align_self_test();
    fractional_scale_rounding_test();
    wrapped_label_test();
    row_sizing_test();
    nested_percent_and_limits_test();
    float_edges_test();
    fixed_and_fit_overflow_test();
    box_border_and_paint_test();
    min_over_max_test();
    margin_moves_child_test();
    wide_sibling_test();
    indefinite_percent_test();
    root_measure_only_test();
    fixed_size_limits_test();
    justify_test();
    align_and_stack_placement_test();
    return 0;
}
