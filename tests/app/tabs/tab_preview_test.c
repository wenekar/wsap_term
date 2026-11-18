#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/debug.h"
#include "paint/scene.h"
#include "ui/ui.h"
#include "ui/widget_ops.h"
#include "app/style.h"
#include "ui/widgets/tab_preview.h"
#include "util/json.h"

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
    int last_max_height;
    int rows_extra;
    bool unchanged;
};

static bool capture(void *userdata, int index, int width, int max_height,
                    bool dragging, const char **title,
                    const struct ui_image **image)
{
    (void)dragging;
    struct fixture *f = userdata;
    if (index < 0 || index >= f->tabs)
        return false;
    f->captures++;
    f->last_width = width;
    f->last_max_height = max_height;
    snprintf(f->title, sizeof(f->title), "tab %d", index);
    *title = f->title;
    if (max_height <= 0) {
        f->image = (struct ui_image){0};
        *image = NULL;
        return true;
    }
    int img_w = width;
    int img_h = img_w / 2 + f->rows_extra;
    if (img_h > max_height) {
        img_w = img_h > 0 ? img_w * max_height / img_h : 0;
        img_h = max_height;
    }
    if (img_w <= 0 || img_h <= 0) {
        f->image = (struct ui_image){0};
        *image = NULL;
        return true;
    }
    f->image = (struct ui_image){
        .pixels = pixels,
        .size = { .x = img_w, .y = img_h },
        .gen = f->unchanged ? f->image.gen : (uint64_t)f->captures,
    };
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
    ui_surface_set_sheet(&f->surface, css ? &f->sheet : app_style_sheet());
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
    assert(image && image->image.src->pixels == f.image.pixels);
    assert(vector2i_eq(image->image.src->size, f.image.size));
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

enum card_dirty {
    CARD_CLEAN = 0,
    CARD_MEASURE,
};

/* The layout state of the card, as the inspector reports it. */
static enum card_dirty card_dirty(struct fixture *f)
{
    struct json_writer j = {0};
    struct ui_debug_view view = {
        .root = &f->root, .surface = &f->surface, .scene = &f->scene,
    };
    ui_debug_write_widget(&j, &view, ui_tab_preview_widget(&f->card));
    char *dump = json_take(&j, NULL);
    const char *flags = dump ? strstr(dump, "\"dirty\":[") : NULL;
    assert(flags);
    flags += strlen("\"dirty\":[");
    const char *end = strchr(flags, ']');
    assert(end);
    enum card_dirty dirty = CARD_CLEAN;
    for (const char *p = flags; p + 7 < end; p++)
        if (!strncmp(p, "measure", 7))
            dirty = CARD_MEASURE;
    free(dump);
    return dirty;
}

static void unchanged_refresh_requests_no_redraw_test(void)
{
    struct fixture f;
    setup(&f, NULL);
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    assert(ui_surface_build_scene(&f.surface, &f.scene));

    /* A poll that finds the same picture and the same title keeps the same
     * scene. */
    f.unchanged = true;
    int captures = f.captures;
    uint64_t gen = f.image.gen;
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(f.captures == captures + 1);
    assert(card_dirty(&f) == CARD_CLEAN);
    assert(f.image.gen == gen);

    /* A new picture of the same size repaints the card, and only that. */
    f.unchanged = false;
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(f.captures == captures + 2);
    assert(card_dirty(&f) == CARD_CLEAN);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    const struct ui_scene_op *image = find_op(&f.scene, UI_SCENE_OP_IMAGE);
    assert(image && image->image.src->gen == (uint64_t)f.captures);

    /* A taller picture relayouts the card. */
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    struct rect2i was = bounds(&f);
    f.rows_extra = 8;
    ui_surface_tick(&f.surface, ui_surface_next_deadline(&f.surface));
    assert(card_dirty(&f) == CARD_MEASURE);
    ui_surface_prepare(&f.surface);
    assert(bounds(&f).height == was.height + 8);
    teardown(&f);
}

static void assert_lint_clean(struct fixture *f)
{
    assert(ui_surface_build_scene(&f->surface, &f->scene));
    struct ui_debug_view view = {
        .root = &f->root, .scene = &f->scene, .surface = &f->surface,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
}

static void short_window_hides_too_small_preview_and_shows_title_only_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 200px; padding: 4px; gap: 6px; border: 1px solid #444; }");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 60 });
    ui_surface_prepare(&f.surface);

    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(!ui_tab_preview_is_visible(&f.card));
    assert(f.captures == 0);
    assert(rect2i_is_empty(bounds(&f)));
    assert_lint_clean(&f);
    assert(!find_op(&f.scene, UI_SCENE_OP_IMAGE));
    assert(!find_op(&f.scene, UI_SCENE_OP_TEXT));

    /* Growing enough for the title row shows a title-only card below TAB0. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 66 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.last_max_height == 0);
    struct rect2i b = bounds(&f);
    assert(b.y >= rect2i_end(TAB0).y);
    assert(rect2i_end(b).y <= 66);
    assert_lint_clean(&f);
    assert(!find_op(&f.scene, UI_SCENE_OP_IMAGE));
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(ui_tab_preview_widget(&f.card), &parts);
    assert(find_part(&parts, "title") && !find_part(&parts, "image"));
    ui_debug_parts_free(&parts);

    /* Shrinking below the title card height hides the shown card. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 60 });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));
    assert(rect2i_is_empty(bounds(&f)));
    assert_lint_clean(&f);
    teardown(&f);
}

static void height_constrained_window_narrows_and_resizes_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 200px; padding: 4px; gap: 6px; border: 1px solid #444; }");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 120 });
    ui_surface_prepare(&f.surface);

    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.last_max_height == 52);
    assert(f.image.size.y > 0 && f.image.size.y <= 52);
    struct rect2i b = bounds(&f);
    assert(b.y >= rect2i_end(TAB0).y && rect2i_end(b).y <= 120);
    assert(rect2i_end(b).x <= 400);
    assert_lint_clean(&f);
    assert(find_op(&f.scene, UI_SCENE_OP_IMAGE));

    /* Narrowing width clamps the card cleanly inside the viewport. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 100, .y = 120 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    b = bounds(&f);
    assert(b.width <= 100 && rect2i_end(b).x <= 100);
    assert(ui_tab_preview_image_width(&f.card, false) <= 90);
    assert_lint_clean(&f);

    /* Growing gives back full image dimensions. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.last_max_height > 52);
    assert(f.image.size.y > 52);
    b = bounds(&f);
    assert(b.width == 200 && b.y >= rect2i_end(TAB0).y);
    assert(rect2i_end(b).y <= 300);
    assert_lint_clean(&f);

    /* Shrinking to title-only height drops the image without covering TAB0. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 64 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    b = bounds(&f);
    assert(b.y >= rect2i_end(TAB0).y && rect2i_end(b).y <= 64);
    assert_lint_clean(&f);
    assert(!find_op(&f.scene, UI_SCENE_OP_IMAGE));

    /* Shrinking too narrow for chrome hides the preview and growing restores it. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 10, .y = 120 });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    assert_lint_clean(&f);
    teardown(&f);
}

static void style_scale_change_recomputes_budget_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 200px; padding: 4px; gap: 6px; border: 1px solid #444; }");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 80 });
    ui_surface_prepare(&f.surface);

    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));

    /* At 2x scale the title card no longer fits below TAB0 inside 80px. */
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32,
    });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));
    assert_lint_clean(&f);

    /* Restoring 1x scale reveals the preview again without covering TAB0. */
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    struct rect2i b = bounds(&f);
    assert(b.y >= rect2i_end(TAB0).y && rect2i_end(b).y <= 80);
    assert_lint_clean(&f);
    teardown(&f);
}

static void short_window_drag_budget_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 200px; padding: 4px; gap: 6px; border: 1px solid #444; }");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 80 });
    ui_surface_prepare(&f.surface);

    ui_tab_preview_follow(&f.card, 0, (struct vector2i){ .x = 200, .y = 40 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    struct rect2i b = bounds(&f);
    assert(b.y >= 0 && rect2i_end(b).y <= 80);
    assert_lint_clean(&f);

    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 20 });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));

    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 200 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    b = bounds(&f);
    assert(b.y >= 0 && rect2i_end(b).y <= 200);
    assert_lint_clean(&f);
    teardown(&f);
}

static bool capture_oversized(void *userdata, int index, int width,
                              int max_height, bool dragging,
                              const char **title,
                              const struct ui_image **image)
{
    (void)index;
    (void)max_height;
    (void)dragging;
    struct fixture *f = userdata;
    f->captures++;
    snprintf(f->title, sizeof(f->title), "oversized");
    f->image = (struct ui_image){
        .pixels = pixels,
        .size = { .x = width, .y = 200 },
        .gen = (uint64_t)f->captures,
    };
    *title = f->title;
    *image = &f->image;
    return true;
}

static void bottom_anchor_flips_above_within_budget_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 200px; padding: 4px; gap: 6px; border: 1px solid #444; }");
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 120 });
    ui_surface_prepare(&f.surface);

    struct rect2i bottom_tab = { .pos = { .x = 10, .y = 90 }, .size = { .x = 150, .y = 30 } };
    ui_tab_preview_hover(&f.card, 0, bottom_tab);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.last_max_height == 52);
    struct rect2i b = bounds(&f);
    assert(b.y >= 0 && rect2i_end(b).y <= bottom_tab.y);
    assert_lint_clean(&f);

    /* Moving anchor into a shorter bottom window leaves too little room above. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 50 });
    bottom_tab.y = 20;
    ui_tab_preview_hover(&f.card, 0, bottom_tab);
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));
    teardown(&f);
}

static void oversized_source_image_is_dropped_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 200px; padding: 4px; gap: 6px; border: 1px solid #444; }");
    ui_tab_preview_set_source(&f.card, &(struct ui_tab_preview_source){
        .capture = capture_oversized, .userdata = &f,
    });
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 120 });
    ui_surface_prepare(&f.surface);

    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    struct rect2i b = bounds(&f);
    assert(b.y >= rect2i_end(TAB0).y && rect2i_end(b).y <= 120);
    assert_lint_clean(&f);
    assert(!find_op(&f.scene, UI_SCENE_OP_IMAGE));
    teardown(&f);
}

static void hidden_for_size_transitions_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 120px; padding: 4px; gap: 2px; border: 1px solid #444; }");

    /* Switch hovered tab while hidden for tiny window, then grow back. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 40 });
    ui_surface_prepare(&f.surface);
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == 0);

    ui_tab_preview_hover(&f.card, 1, TAB1);
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == 0);

    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    assert(!strcmp(f.title, "tab 1"));
    assert(bounds(&f).x >= TAB1.x && bounds(&f).y >= rect2i_end(TAB1).y);
    assert_lint_clean(&f);

    /* Leaving tabs while hidden cancels restoration on later growth. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 40 });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));
    ui_tab_preview_hover(&f.card, -1, (struct rect2i){0});
    int captures = f.captures;
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == captures);

    /* A fresh hover after leaving restarts the normal show delay. */
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 3000);
    assert(!ui_tab_preview_is_visible(&f.card));
    uint64_t due = ui_surface_next_deadline(&f.surface);
    assert(due > 3000);
    tick(&f, due);
    assert(ui_tab_preview_is_visible(&f.card) && !strcmp(f.title, "tab 0"));

    /* Suppressing while hidden keeps previews blocked after growing back. */
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 40 });
    ui_surface_prepare(&f.surface);
    assert(!ui_tab_preview_is_visible(&f.card));
    ui_tab_preview_suppress(&f.card);
    captures = f.captures;
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    ui_surface_prepare(&f.surface);
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 5000);
    tick(&f, 7000);
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == captures);

    ui_tab_preview_hover(&f.card, -1, (struct rect2i){0});
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 8000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card) && !strcmp(f.title, "tab 1"));
    teardown(&f);
}

struct sync_root {
    struct ui_tab_preview *card;
    int sync_index;
    struct rect2i sync_anchor;
};

static struct vector2i sync_root_measure(const struct ui_widget *widget,
                                         struct ui_constraints constraints)
{
    struct sync_root *sr = widget->userdata;
    ui_widget_measure(ui_tab_preview_widget(sr->card), constraints);
    return ui_constraints_clamp(constraints, constraints.max);
}

static void sync_root_layout(struct ui_widget *widget, struct rect2i bounds)
{
    struct sync_root *sr = widget->userdata;
    if (ui_tab_preview_sync_anchor(sr->card, sr->sync_index, sr->sync_anchor))
        ui_widget_measure(ui_tab_preview_widget(sr->card),
                          (struct ui_constraints){ .max = bounds.size });
    ui_widget_place_child(ui_tab_preview_widget(sr->card), bounds);
}

static const struct ui_widget_ops SYNC_ROOT_OPS = {
    .name = "preview-sync-root",
    .measure = sync_root_measure,
    .layout = sync_root_layout,
};

static void managed_anchor_sync_test(void)
{
    struct fixture f;
    setup(&f, ".tab-preview { width: 120px; padding: 4px; gap: 2px; border: 1px solid #444; }");
    struct sync_root sr = { .card = &f.card, .sync_index = -1 };
    f.root.ops = &SYNC_ROOT_OPS;
    f.root.userdata = &sr;
    ui_tab_preview_set_managed_anchor(&f.card, true);

    /* Sync never starts a hover when the preview is idle. */
    assert(!ui_tab_preview_sync_anchor(&f.card, 0, TAB0));
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == 0);

    /* Pending hover receives updated anchor during resize before showing. */
    struct rect2i moved = { .pos = { .x = 40 }, .size = { .x = 120, .y = 30 } };
    sr.sync_index = 0;
    sr.sync_anchor = moved;
    ui_tab_preview_hover(&f.card, 0, TAB0);
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 280 });
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    assert(bounds(&f).x == moved.x);

    /* Scale change captures once at new scale inside root_layout cleanly. */
    int before = f.captures;
    struct rect2i scaled = { .pos = { .x = 80 }, .size = { .x = 240, .y = 60 } };
    sr.sync_anchor = scaled;
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32,
    });
    (void)ui_surface_take_frame_request(&f.surface);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(!ui_surface_frame_pending(&f.surface));
    assert(f.captures == before + 1);
    assert(bounds(&f).x == scaled.x && bounds(&f).y >= rect2i_end(scaled).y);

    /* Index mismatch in layout hides card without setting frame pending. */
    sr.sync_index = -1;
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 420, .y = 280 });
    (void)ui_surface_take_frame_request(&f.surface);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(!ui_tab_preview_is_visible(&f.card) && rect2i_is_empty(bounds(&f)));
    assert(!ui_surface_frame_pending(&f.surface));

    /* Suppressed preview stays suppressed even when sync sees index < 0. */
    sr.sync_index = 0;
    sr.sync_anchor = TAB0;
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 3000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    ui_tab_preview_suppress(&f.card);
    sr.sync_index = -1;
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    before = f.captures;
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 4000);
    tick(&f, 6000);
    assert(!ui_tab_preview_is_visible(&f.card) && f.captures == before);

    /* Explicitly leaving tabs lifts suppression cleanly. */
    sr.sync_index = 1;
    sr.sync_anchor = TAB1;
    ui_tab_preview_hover(&f.card, -1, (struct rect2i){0});
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 7000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card) && !strcmp(f.title, "tab 1"));

    /* Dragging preview still updates budget on scale and sheet changes. */
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_tab_preview_follow(&f.card, 0, (struct vector2i){ .x = 200, .y = 150 });
    assert(ui_tab_preview_is_visible(&f.card));
    int drag_w_1x = f.last_width;
    before = f.captures;
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 2, .cell_w = 16, .cell_h = 32,
    });
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.captures > before && f.last_width > drag_w_1x);

    struct css_sheet narrow_sheet = {0};
    assert(css_parse(&narrow_sheet,
                     ".tab-preview { width: 80px; padding: 4px; gap: 2px; border-width: 1px; border-color: #444; }\n"
                     ".tab-preview.dragging { width: 80px; }"));
    int drag_w_2x = f.last_width;
    before = f.captures;
    ui_surface_set_sheet(&f.surface, &narrow_sheet);
    ui_surface_prepare(&f.surface);
    assert(ui_tab_preview_is_visible(&f.card));
    assert(f.captures > before && f.last_width < drag_w_2x);
    css_sheet_clear(&narrow_sheet);

    /* Bare resize on a fills-root container defers budget to parent layout. */
    struct ui_widget wrapper;
    ui_widget_init(&wrapper, NULL, NULL);
    ui_widget_remove_child(&f.root, ui_tab_preview_widget(&f.card));
    ui_surface_set_root(&f.surface, &wrapper);
    ui_widget_add_child(&wrapper, &f.root);
    ui_widget_add_child(&f.root, ui_tab_preview_widget(&f.card));
    ui_surface_set_scale(&f.surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 300 });
    sr.sync_index = 1;
    sr.sync_anchor = TAB1;
    ui_tab_preview_follow(&f.card, -1, (struct vector2i){0});
    ui_tab_preview_hover(&f.card, 1, TAB1);
    tick(&f, 8000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    before = f.captures;
    ui_surface_resize(&f.surface, (struct vector2i){ .x = 400, .y = 100 });
    (void)ui_surface_take_frame_request(&f.surface);
    assert(ui_surface_build_scene(&f.surface, &f.scene));
    assert(!ui_surface_frame_pending(&f.surface));
    assert(f.captures == before + 1);
    assert(rect2i_end(bounds(&f)).y <= 100);
    teardown(&f);
    ui_widget_destroy(&wrapper);
}

static void card_opacity_and_border_test(void)
{
    struct fixture f;
    setup(&f,
          ".tab-preview { width: 120px; padding: 4px; gap: 2px; "
          "background: #112233; border-color: #445566; border-width: 2px; "
          "border-radius: 6px; box-shadow: 0 4px 12px #00000080; "
          "opacity: 0.5; }"
          ".tab-preview-title { color: #aabbcc; }");
    ui_tab_preview_hover(&f.card, 0, TAB0);
    tick(&f, 1000);
    tick(&f, ui_surface_next_deadline(&f.surface));
    assert(ui_tab_preview_is_visible(&f.card));
    assert_lint_clean(&f);

    const struct ui_scene_op *shadow = find_op(&f.scene, UI_SCENE_OP_SHADOW);
    assert(shadow && shadow->color == 0x40000000u && shadow->radius == 6);
    bool saw_border = false, saw_bg = false;
    for (int i = 0; i < f.scene.n_ops; i++) {
        const struct ui_scene_op *op = &f.scene.ops[i];
        if (op->kind == UI_SCENE_OP_ROUNDED_RECT && op->color == 0x80445566u &&
            op->radius == 6)
            saw_border = true;
        if (op->kind == UI_SCENE_OP_ROUNDED_RECT && op->color == 0x80112233u &&
            op->radius == 4)
            saw_bg = true;
    }
    assert(saw_border && saw_bg);
    assert(has_text_color(&f.scene, 0x80AABBCCu));
    teardown(&f);
}

int main(void)
{
    hover_shows_card_below_tab_after_delay_test();
    shown_card_follows_hover_and_refreshes_test();
    unchanged_refresh_requests_no_redraw_test();
    suppress_lasts_until_pointer_leaves_tabs_test();
    dragged_card_follows_pointer_inside_surface_test();
    hover_picture_is_larger_than_drag_picture_test();
    restyled_sheet_updates_card_and_title_test();
    short_window_hides_too_small_preview_and_shows_title_only_test();
    height_constrained_window_narrows_and_resizes_test();
    style_scale_change_recomputes_budget_test();
    short_window_drag_budget_test();
    bottom_anchor_flips_above_within_budget_test();
    oversized_source_image_is_dropped_test();
    hidden_for_size_transitions_test();
    managed_anchor_sync_test();
    card_opacity_and_border_test();
    puts("PASS: tab preview tests");
    return 0;
}
