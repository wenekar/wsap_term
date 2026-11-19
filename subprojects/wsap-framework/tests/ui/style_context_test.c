#include <assert.h>
#include <string.h>

#include "ui/style_context.h"
#include "ui/widget_ops.h"
#include "ui/widgets/button.h"
#include "ui/widgets/label.h"
#include "ui/box.h"

static void panel_init(struct ui_box *panel)
{
    ui_box_init(panel);
    ui_widget_set_style_identity(&panel->widget, "panel", NULL, "panel");
}

static void style_resolution_and_inheritance_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".panel { color: #112233; opacity: 0.5; }"
        ".button:hover { color: #abcdef; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &sheet);

    struct ui_box panel = {0};
    struct ui_button button = {0};
    panel_init(&panel);
    ui_button_init(&button);
    assert(ui_widget_add_child(ui_box_widget(&panel), &button.widget));
    ui_widget_set_style_pseudo(&button.widget, CSS_PSEUDO_HOVER);

    ui_style_context_apply(&context, ui_box_widget(&panel));
    const struct css_style *parent = ui_widget_resolved_style(ui_box_widget(&panel));
    const struct css_style *child = ui_widget_resolved_style(&button.widget);
    assert(parent && child);
    assert(parent->has_text_color && parent->text_color == 0xFF112233);
    assert(child->has_text_color && child->text_color == 0xFFABCDEF);
    assert(child->opacity > 0.49f && child->opacity < 0.51f);
    assert(ui_widget_is_style_current(&button.widget));

    ui_widget_destroy(ui_box_widget(&panel));
    ui_style_context_destroy(&context);
    css_sheet_clear(&sheet);
}

static void set_sheet_rebuilds_same_pointer_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".panel { background: #112233; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &sheet);

    struct ui_box panel = {0};
    panel_init(&panel);
    ui_style_context_apply(&context, ui_box_widget(&panel));
    const struct css_style *style =
        ui_widget_resolved_style(ui_box_widget(&panel));
    assert(style && style->has_bg_color && style->bg_color == 0xFF112233);

    css_sheet_clear(&sheet);
    assert(css_parse(&sheet, ".panel { background: #445566; }"));
    ui_style_context_set_sheet(&context, &sheet);
    ui_style_context_apply(&context, ui_box_widget(&panel));
    style = ui_widget_resolved_style(ui_box_widget(&panel));
    assert(style->has_bg_color && style->bg_color == 0xFF445566);

    ui_widget_destroy(ui_box_widget(&panel));
    ui_style_context_destroy(&context);
    css_sheet_clear(&sheet);
}

static void scale_feeds_widget_resolution_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".fixed { width: 10px; height: 5px; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &sheet);
    ui_style_context_set_scale(&context, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f });

    struct ui_widget widget = {0};
    ui_widget_init(&widget, NULL, NULL);
    ui_widget_set_style_identity(&widget, "fixed", NULL, NULL);
    ui_style_context_apply(&context, &widget);
    assert(widget.style_scale.dpi_scale == 1.0f);
    struct vector2i size = ui_widget_measure(&widget,
                                           ui_constraints_unbounded());
    assert(size.x == 10 && size.y == 5);

    ui_style_context_set_scale(&context, (struct css_scale_context){
        .dpi_scale = 2.0f, .cell_w = 8.0f, .cell_h = 16.0f });
    ui_style_context_apply(&context, &widget);
    assert(widget.style_scale.dpi_scale == 2.0f);
    size = ui_widget_measure(&widget, ui_constraints_unbounded());
    assert(size.x == 20 && size.y == 10);

    struct ui_widget child = {0};
    ui_widget_init(&child, NULL, NULL);
    ui_widget_set_style_identity(&child, "fixed", NULL, NULL);
    assert(ui_widget_add_child(&widget, &child));
    ui_style_context_apply(&context, &widget);
    assert(ui_widget_is_style_current(&child) && child.style_scale.dpi_scale == 2.0f);
    size = ui_widget_measure(&child, ui_constraints_unbounded());
    assert(size.x == 20 && size.y == 10);

    ui_widget_destroy(&widget);
    ui_style_context_destroy(&context);
    css_sheet_clear(&sheet);
}

static void generic_style_sizing_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".wide { width: 10ch; }"
        ".tall { height: 40px; }"
        ".floor { min-width: 50px; }"
        ".half { width: 50%; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &sheet);
    ui_style_context_set_scale(&context, (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f });

    struct ui_label label = {0};
    ui_label_init(&label);
    ui_label_set_text(&label, "ab");

    ui_widget_set_style_identity(&label.widget, "wide", NULL, "label");
    ui_style_context_apply(&context, &label.widget);
    struct vector2i size = ui_widget_measure(&label.widget,
                                             ui_constraints_unbounded());
    assert(size.x == 80 && size.y == 16);

    ui_widget_set_style_identity(&label.widget, "tall", NULL, "label");
    ui_style_context_apply(&context, &label.widget);
    size = ui_widget_measure(&label.widget, ui_constraints_unbounded());
    assert(size.x == 16 && size.y == 40);

    ui_widget_set_style_identity(&label.widget, "floor", NULL, "label");
    ui_style_context_apply(&context, &label.widget);
    size = ui_widget_measure(&label.widget, ui_constraints_unbounded());
    assert(size.x == 50 && size.y == 16);

    ui_widget_set_style_identity(&label.widget, "half", NULL, "label");
    ui_style_context_apply(&context, &label.widget);
    size = ui_widget_measure(&label.widget, (struct ui_constraints){
        .min = { .x = 0, .y = 0 }, .max = { .x = 200, .y = 100 } });
    assert(size.x == 100 && size.y == 16);

    ui_widget_destroy(&label.widget);
    ui_style_context_destroy(&context);
    css_sheet_clear(&sheet);
}

static void host_sheet_with_defaults_is_not_duplicated_test(void)
{
    struct css_sheet host = {0};
    css_init_default(&host);
    assert(css_parse(&host, ".panel { background: #112233; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &host);

    const struct css_sheet *sheet = ui_style_context_sheet(&context);
    assert(sheet->rule_count == css_default_sheet()->rule_count + 1);

    struct css_style menu = ui_style_context_style(&context, "context-menu", 0);
    assert(menu.has_bg_color && menu.bg_color == 0xFF1E1E2E);
    struct css_style panel = ui_style_context_style(&context, "panel", 0);
    assert(panel.has_bg_color && panel.bg_color == 0xFF112233);

    ui_style_context_destroy(&context);
    css_sheet_clear(&host);
}

static void plain_host_sheet_gets_defaults_test(void)
{
    struct css_sheet host = {0};
    assert(css_parse(&host, ".panel { background: #112233; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &host);

    const struct css_sheet *sheet = ui_style_context_sheet(&context);
    assert(sheet->rule_count == css_default_sheet()->rule_count + host.rule_count);

    struct css_style menu = ui_style_context_style(&context, "context-menu", 0);
    assert(menu.has_bg_color && menu.bg_color == 0xFF1E1E2E);
    struct css_style panel = ui_style_context_style(&context, "panel", 0);
    assert(panel.has_bg_color && panel.bg_color == 0xFF112233);

    ui_style_context_destroy(&context);
    css_sheet_clear(&host);
}

static void overlay_sheet_layers_over_promoted_host_test(void)
{
    struct css_sheet host = {0};
    css_init_default(&host);
    assert(css_parse(&host,
        ":root { --panel-bg: #112233; }"
        ".panel { background: var(--panel-bg); }"));

    /* The overlay is a theme sheet. It holds no core rule, and it reads and
       overrides the variables of the host. */
    struct css_sheet overlay = {0};
    assert(css_parse_deferred(&overlay,
        ":root { --surface: #121314; --panel-bg: #445566; }"
        ".badge { background: var(--panel-bg); color: var(--accent); }",
        NULL, NULL));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &host);
    ui_style_context_set_overlay_sheet(&context, &overlay);

    struct css_style menu = ui_style_context_style(&context, "context-menu", 0);
    assert(menu.has_bg_color && menu.bg_color == 0xFF121314u);
    struct css_style panel = ui_style_context_style(&context, "panel", 0);
    assert(panel.has_bg_color && panel.bg_color == 0xFF445566u);
    struct css_style badge = ui_style_context_style(&context, "badge", 0);
    assert(badge.has_bg_color && badge.bg_color == 0xFF445566u);
    assert(badge.has_text_color && badge.text_color == 0xFF89B4FAu);

    /* Without the overlay, the host variables apply again. */
    ui_style_context_set_overlay_sheet(&context, NULL);
    panel = ui_style_context_style(&context, "panel", 0);
    assert(panel.has_bg_color && panel.bg_color == 0xFF112233u);
    menu = ui_style_context_style(&context, "context-menu", 0);
    assert(menu.has_bg_color && menu.bg_color == 0xFF1E1E2Eu);

    ui_style_context_destroy(&context);
    css_sheet_clear(&overlay);
    css_sheet_clear(&host);
}

static void overlay_sheet_adds_core_to_plain_host_test(void)
{
    struct css_sheet host = {0};
    assert(css_parse(&host, ".panel { background: #112233; }"));

    struct css_sheet overlay = {0};
    assert(css_parse_deferred(&overlay,
        ".badge { color: var(--accent); }", NULL, NULL));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &host);
    ui_style_context_set_overlay_sheet(&context, &overlay);

    struct css_style menu = ui_style_context_style(&context, "context-menu", 0);
    assert(menu.has_bg_color && menu.bg_color == 0xFF1E1E2Eu);
    struct css_style panel = ui_style_context_style(&context, "panel", 0);
    assert(panel.has_bg_color && panel.bg_color == 0xFF112233u);
    struct css_style badge = ui_style_context_style(&context, "badge", 0);
    assert(badge.has_text_color && badge.text_color == 0xFF89B4FAu);

    ui_style_context_destroy(&context);
    css_sheet_clear(&overlay);
    css_sheet_clear(&host);
}

static void host_root_variables_override_defaults_test(void)
{
    struct css_sheet host = {0};
    assert(css_parse(&host, ":root { --accent: #ff00aa; --surface: #121314; }"));

    struct ui_style_context context = {0};
    ui_style_context_init(&context);
    ui_style_context_set_sheet(&context, &host);

    struct css_style bar = ui_style_context_style(&context, "scrollbar", 0);
    assert(!bar.has_bg_color && bar.width.unit == CSS_UNIT_PX);

    struct css_style link = ui_style_context_style(&context, "label-link", 0);
    assert(link.has_text_color && link.text_color == 0xFFFF00AAu);

    struct css_style focus = ui_style_context_style(&context, "label-focus", 0);
    assert(focus.has_border_color && focus.border_color == 0xFFFF00AAu);

    struct css_style menu = ui_style_context_style(&context, "context-menu", 0);
    assert(menu.has_bg_color && menu.bg_color == 0xFF121314u);

    struct css_style input = ui_style_context_style(&context, "text-input", 0);
    assert(input.has_bg_color && input.bg_color == 0xFF121314u);

    ui_style_context_destroy(&context);
    css_sheet_clear(&host);
}

int main(void)
{
    style_resolution_and_inheritance_test();
    set_sheet_rebuilds_same_pointer_test();
    scale_feeds_widget_resolution_test();
    generic_style_sizing_test();
    host_sheet_with_defaults_is_not_duplicated_test();
    plain_host_sheet_gets_defaults_test();
    overlay_sheet_layers_over_promoted_host_test();
    overlay_sheet_adds_core_to_plain_host_test();
    host_root_variables_override_defaults_test();
    return 0;
}
