#include "ui/ui.h"
#include "ui/widget_impl.h"
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
    ui_box_set_align(&column, UI_BOX_ALIGN_STRETCH);
    ui_widget_set_style_identity(&column.widget, "column", NULL, NULL);
    ui_label_init(&label);
    assert(ui_label_set_text(&label, "aaaaa bbbbb ccccc"));
    ui_button_init(&button);
    ui_button_set_text(&button, "OK");
    assert(css_parse(&sheet, ".column { width: 50%; }"));
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

int main(void)
{
    wrapped_label_test();
    row_sizing_test();
    nested_percent_and_limits_test();
    float_edges_test();
    fixed_and_fit_overflow_test();
    return 0;
}
