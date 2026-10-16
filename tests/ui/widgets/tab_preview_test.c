#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/debug.h"
#include "paint/scene.h"
#include "ui/ui.h"

static uint32_t pixels[400 * 300];

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_tab_preview card;
    struct css_sheet sheet;
    struct ui_scene scene;
    struct ui_image image;
    char title[32];
    int tabs;
    int captures;
    int last_width;
};

static bool capture(void *userdata, int index, int width, bool dragging,
                    const char **title, const struct ui_image **image)
{
    (void)dragging;
    struct fixture *f = userdata;
    if (index < 0 || index >= f->tabs)
        return false;
    f->captures++;
    f->last_width = width;
    snprintf(f->title, sizeof(f->title), "tab %d", index);
    f->image = (struct ui_image){
        .pixels = pixels, .size = { .x = width, .y = width / 2 },
        .gen = (uint64_t)f->captures,
    };
    *title = f->title;
    *image = &f->image;
    return true;
}

static void setup(struct fixture *f, const char *css)
{
    *f = (struct fixture){ .tabs = 4 };
    if (css)
        assert(css_parse(&f->sheet, css));
    ui_widget_init(&f->root, NULL, NULL);
    ui_tab_preview_init(&f->card);
    ui_tab_preview_set_source(&f->card, &(struct ui_tab_preview_source){
        .capture = capture, .userdata = f,
    });
    assert(ui_widget_add_child(&f->root, ui_tab_preview_widget(&f->card)));
    if (css)
        ui_surface_set_sheet(&f->surface, &f->sheet);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 400, .y = 300 });
    ui_surface_prepare(&f->surface);
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(&f->root);
    ui_scene_destroy(&f->scene);
    css_sheet_clear(&f->sheet);
}

static void tick(struct fixture *f, uint64_t now_ms)
{
    ui_surface_tick(&f->surface, now_ms);
    ui_surface_prepare(&f->surface);
}

static struct rect2i bounds(struct fixture *f)
{
    return ui_tab_preview_widget(&f->card)->bounds;
}

static const struct ui_debug_part *find_part(const struct ui_debug_parts *parts,
                                             const char *name)
{
    for (int i = 0; i < parts->n; i++)
        if (!strcmp(parts->items[i].name, name))
            return &parts->items[i];
    return NULL;
}

static const struct ui_scene_op *find_op(const struct ui_scene *s,
                                         enum ui_scene_op_kind kind)
{
    for (int i = 0; i < s->n_ops; i++)
        if (s->ops[i].kind == kind)
            return &s->ops[i];
    return NULL;
}

static const struct rect2i TAB0 = { .pos = { .x = 10 }, .size = { .x = 150, .y = 30 } };
static const struct rect2i TAB1 = { .pos = { .x = 170 }, .size = { .x = 150, .y = 30 } };

static void hover_shows_card_below_tab_after_delay_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 120px; padding: 4px; gap: 2px; }");
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    assert(!ui_tab_preview_is_visible(&f.card));
    uint64_t due = ui_surface_next_deadline(&f.surface);
    assert(due > 1000);
    tick(&f, due - 1);
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == 0);
    tick(&f, due);
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.last_width == ui_tab_preview_image_width(&f.card, false));
    assert(f.last_width >= 100 && f.last_width <= 112);

    struct rect2i b = bounds(&f);
    assert(b.y >= rect2i_end(TAB0).y && b.x == TAB0.x);
    assert(b.width == 120);

    assert(ui_surface_build_scene(&f.surface, &f.scene));
    const struct ui_scene_op *image = find_op(&f.scene, UI_SCENE_OP_IMAGE);
    assert(image && image->image == &f.image);
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(ui_tab_preview_widget(&f.card), &parts);
    const struct ui_debug_part *title = find_part(&parts, "title");
    const struct ui_debug_part *pic = find_part(&parts, "image");
    assert(title && pic && !strcmp(title->text, "tab 0"));
    assert(rect2i_end(title->rect).y <= pic->rect.y);
    assert(vector2i_eq(pic->rect.pos, image->rect.pos));
    assert(rect2i_eq(rect2i_intersect(b, pic->rect), pic->rect));
    ui_debug_parts_free(&parts);
    struct ui_debug_view view = {
        .root = &f.root, .scene = &f.scene, .surface = &f.surface,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);

    ui_tab_preview_hover(&f.card, -1, (struct rect2i){0});
    assert(!ui_tab_preview_is_visible(&f.card));
    tick(&f, due + 10);
    assert(ui_surface_next_deadline(&f.surface) == 0);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(!find_op(&f.scene, UI_SCENE_OP_IMAGE));
    teardown(&f);
}

static void shown_card_follows_hover_and_refreshes_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card) && f.captures == 1);

    ui_tab_preview_hover(&f.card, 1, TAB1);
    ui_surface_prepare(&f.surface);
    assert(f.captures == 2 && !strcmp(f.title, "tab 1"));
    assert(bounds(&f).y >= rect2i_end(TAB1).y);

    uint64_t now = 3000;
    tick(&f, now);
    int captures = f.captures;
    uint64_t refresh = ui_surface_next_deadline(&f.surface);
    assert(refresh > now);
    tick(&f, refresh - 1);
    assert(f.captures == captures);
    tick(&f, refresh);
    assert(f.captures == captures + 1);

    f.tabs = 1;
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(!ui_tab_preview_is_visible(&f.card));
    teardown(&f);
}

static void suppress_lasts_until_pointer_leaves_tabs_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    ui_tab_preview_hover(&f.card, 0, TAB0);
    ui_tab_preview_suppress(&f.card);
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 1000);
    tick(&f, 5000);
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == 0);

    ui_tab_preview_hover(&f.card, -1, (struct rect2i){0});
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 6000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    teardown(&f);
}

static void dragged_card_follows_pointer_inside_surface_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview.dragging { width: 100px; }");
    ui_tab_preview_follow(&f.card, 2, (struct vector2i){ .x = 200, .y = 150 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    struct rect2i b = bounds(&f);
    assert(b.width == 100);
    assert(b.x + b.width / 2 == 200 && b.y + b.height / 2 == 150);

    ui_tab_preview_follow(&f.card, 2, (struct vector2i){ .x = 395, .y = 295 });
    ui_surface_prepare(&f.surface);
    b = bounds(&f);
    assert(b.x >= 0 && b.y >= 0);
    assert(rect2i_end(b).x <= 400 && rect2i_end(b).y <= 300);

    ui_tab_preview_hover(&f.card, 0, TAB0);
    assert(f.title[4] == '2');
    teardown(&f);
}

static void hover_picture_is_larger_than_drag_picture_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    int hover = ui_tab_preview_image_width(&f.card, false);
    int drag = ui_tab_preview_image_width(&f.card, true);
    assert(hover > drag && drag > 0);
    teardown(&f);
}

static bool has_text_color(const struct ui_scene *s, uint32_t color)
{
    for (int i = 0; i < s->n_ops; i++)
        if (s->ops[i].kind == UI_SCENE_OP_TEXT && s->ops[i].color == color)
            return true;
    return false;
}

static void restyled_sheet_updates_card_and_title_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 120px; }");
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card) && bounds(&f).width == 120);
    int before = ui_tab_preview_image_width(&f.card, false);

    css_sheet_clear(&f.sheet);
    assert(css_parse(&f.sheet,
                     ".tab-preview { width: 200px; } "
                     ".tab-preview.dragging { width: 90px; } "
                     ".tab-preview-title { color: #112233; }"));
    ui_surface_set_sheet(&f.surface, &f.sheet);
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card) && bounds(&f).width == 200);
    assert(ui_tab_preview_image_width(&f.card, false) > before);

    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(has_text_color(&f.scene, 0xff112233u));

    ui_tab_preview_follow(&f.card, 1, (struct vector2i){ .x = 200, .y = 150 });
    ui_surface_prepare(&f.surface);
    assert(bounds(&f).width == 90);
    teardown(&f);
}

int main(void)
{
    hover_shows_card_below_tab_after_delay_test();
    shown_card_follows_hover_and_refreshes_test();
    suppress_lasts_until_pointer_leaves_tabs_test();
    dragged_card_follows_pointer_inside_surface_test();
    hover_picture_is_larger_than_drag_picture_test();
    restyled_sheet_updates_card_and_title_test();
    puts("PASS: tab preview tests");
    return 0;
}
