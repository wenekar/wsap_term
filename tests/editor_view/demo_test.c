#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "editor_view/editor_view.h"
#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "ui/ui.h"
#include "ui/widget_ops.h"

/* A demo screen for the editor view. It mounts the widget in a container and
 * checks the painted scene with the UI inspector lint. */

struct demo {
    struct ui_surface surface;
    struct ui_box box;
    struct ui_editor_view view;
    struct ui_scene scene;
};

/* Paints the demo tree. The private tree helper is not visible here. */
static void paint_widget(const struct ui_widget *widget, struct ui_scene *scene)
{
    if (!widget || !widget->ops)
        return;
    struct ui_paint_context paint = { .scene = scene,
                                      .size = widget->bounds.size };
    if (widget->ops->paint)
        widget->ops->paint(widget, &paint);
    for (struct ui_widget *child = widget->first_child; child;
         child = child->next_sibling)
        paint_widget(child, scene);
}

int main(void)
{
    static const char source[] =
        "int main(void)\n"
        "{\n"
        "    size_t count = 0;\n"
        "    /* loop */\n"
        "    for (;;) {\n"
        "        count++;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
    struct demo d = {0};
    ui_widget_shutdown();
    ui_surface_init(&d.surface, &(struct ui_surface_host){0}, NULL);
    ui_surface_set_scale(&d.surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_box_init(&d.box);
    ui_editor_view_init(&d.view);
    assert(ui_editor_view_set_text(&d.view, source, sizeof(source) - 1));
    assert(ui_editor_view_add_fold(&d.view, 1, 8, "main"));
    assert(ui_editor_view_toggle_fold(&d.view, 1));
    ui_editor_view_set_mark(&d.view, 3, UI_EDITOR_MARK_BREAKPOINT);
    ui_editor_view_set_mark(&d.view, 4, UI_EDITOR_MARK_GIT_MODIFIED);
    assert(ui_editor_view_add_inlay(&d.view, 1, "2 references",
                                    UI_EDITOR_INLAY_ABOVE));
    uint32_t matches[] = { 2, 5 };
    assert(ui_editor_view_set_matches(&d.view, matches, 2));
    ui_widget_set_placement(ui_editor_view_widget(&d.view),
                            (struct ui_placement){ .kind = UI_PLACE_FILL });
    assert(ui_widget_add_child(ui_box_widget(&d.box),
                               ui_editor_view_widget(&d.view)));
    ui_surface_set_root(&d.surface, ui_box_widget(&d.box));
    ui_surface_resize(&d.surface, (struct vector2i){ .x = 480, .y = 320 });

    ui_scene_init(&d.scene);
    paint_widget(ui_box_widget(&d.box), &d.scene);
    assert(d.scene.n_ops > 0);

    struct ui_debug_view view = {
        .name = "editor-demo",
        .root = ui_box_widget(&d.box),
        .surface = &d.surface,
        .scene = &d.scene,
    };
    struct ui_debug_lint findings[16];
    int count = ui_debug_lint_scene(&view, findings, 16);
    if (count != 0) {
        for (int i = 0; i < count; i++)
            fprintf(stderr, "lint %d: kind %d op %d other %d offset %d\n", i,
                    (int)findings[i].kind, findings[i].op, findings[i].other_op,
                    findings[i].offset);
    }
    assert(count == 0);

    ui_scene_destroy(&d.scene);
    ui_editor_view_clear(&d.view);
    ui_surface_destroy(&d.surface);
    ui_widget_shutdown();
    puts("editor-view-demo-test: lint reports no findings");
    return 0;
}
