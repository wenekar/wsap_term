#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/tabs/tabs.h"
#include "app/thumbnail.h"
#include "helpers/app_fixture.h"
#include "paint/scene.h"
#include "ui/debug.h"
#include "ui/surface.h"
#include "ui/widget.h"
#include "ui/yasl/client.h"
#include "theme/look.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

struct preview_fixture {
    struct test_app app;
    struct window *w;
    struct ui_scene scene;
};

static struct window *create_preview_window(struct app *app)
{
    struct window *w = window_create(&app->host, &(struct window_spec){0});
    assert(w);
    assert(app_tabs_mount(w, &(struct app_tabs_spec){
        .config = &app->config.terminal,
        .model = &app->tabs,
        .preview_capture = app_thumbnail_capture,
        .preview_release = app_thumbnail_release,
        .preview_ud = app,
        .empty = true,
    }));
    return w;
}

static void setup_fixture(struct preview_fixture *f, struct vector2i size,
                          int tabs)
{
    *f = (struct preview_fixture){0};
    test_app_init(&f->app, "/bin/cat", size);
    f->w = create_preview_window(f->app.app);
    /* The tab preview is an app screen, so its rules come from the theme
       sheet, as in app.c. */
    window_ui_set_style(f->w, look_sheet(f->app.app->look),
                        look_palette(f->app.app->look)->bg);
    ui_surface_set_scale(window_ui_surface(f->w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    if (size.x > 0 && size.y > 0)
        ui_surface_resize(window_ui_surface(f->w), size);
    for (int i = 0; i < tabs; i++)
        assert(app_tabs_new(f->w));
    app_tabs_relayout(f->w);
}

static void teardown_fixture(struct preview_fixture *f)
{
    ui_scene_destroy(&f->scene);
    window_close(f->w);
    test_app_fini(&f->app);
}

static void settle_surface(struct preview_fixture *f)
{
    struct ui_surface *ui = window_ui_surface(f->w);
    uint64_t now = window_frame_time_ms(f->w);
    uint64_t end = now + 2000;
    while (now < end) {
        ui_surface_tick(ui, now);
        ui_surface_prepare(ui);
        uint64_t next = ui_surface_next_deadline(ui);
        if (!next)
            break;
        now = next > now ? next : now + 16;
    }
    app_tabs_service(&f->app.app->tabs, now + APP_TABS_CLOSE_MS);
    ui_surface_prepare(ui);
}

static struct rect2i bar_part_rect(struct window *w, const char *name,
                                   int index)
{
    const struct ui_widget *bar = ui_debug_find(window_ui_root(w), "tabbar");
    assert(bar);
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(bar, &parts);
    struct rect2i rect = {0};
    bool found = false;
    for (int i = 0; i < parts.n; i++) {
        if (parts.items[i].index == index &&
            !strcmp(parts.items[i].name, name)) {
            rect = parts.items[i].rect;
            found = true;
            break;
        }
    }
    ui_debug_parts_free(&parts);
    assert(found);
    return rect;
}

static void pointer(struct window *w, enum ui_pointer_kind kind,
                    struct vector2i pos, int button, uint64_t time_ms)
{
    struct ui_surface *ui = window_ui_surface(w);
    struct ui_event ev = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos, .button = button,
    }, time_ms);
    ui_surface_dispatch(ui, &ev);
    ui_surface_tick(ui, time_ms);
    ui_surface_prepare(ui);
}

static void hover_tab(struct window *w, int index)
{
    struct rect2i tab = bar_part_rect(w, "tab", index);
    pointer(w, UI_POINTER_MOTION, rect2i_center(tab), 0,
            window_frame_time_ms(w));
}

static void click_at(struct window *w, struct vector2i pos)
{
    uint64_t now = window_frame_time_ms(w);
    pointer(w, UI_POINTER_MOTION, pos, 0, now);
    pointer(w, UI_POINTER_PRESS, pos, 0, now);
    pointer(w, UI_POINTER_RELEASE, pos, 0, now);
}

static const struct ui_widget *preview_widget(struct window *w)
{
    const struct ui_widget *preview = ui_debug_find(window_ui_root(w),
                                                    "tab-preview");
    assert(preview);
    return preview;
}

static bool preview_shown(struct window *w)
{
    const struct ui_widget *preview = preview_widget(w);
    if (rect2i_is_empty(preview->bounds))
        return false;
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(preview, &parts);
    bool has_parts = parts.n > 0;
    ui_debug_parts_free(&parts);
    return has_parts;
}

static const struct ui_scene_op *find_image_op(const struct ui_scene *scene)
{
    for (int i = 0; i < scene->n_ops; i++)
        if (scene->ops[i].kind == UI_SCENE_OP_IMAGE)
            return &scene->ops[i];
    return NULL;
}

static void render_and_lint(struct preview_fixture *f)
{
    struct ui_surface *ui = window_ui_surface(f->w);
    (void)ui_surface_take_frame_request(ui);
    assert(ui_surface_build_scene(ui, &f->scene));
    assert(!ui_surface_frame_pending(ui));
    struct ui_debug_view view = {
        .root = window_ui_root(f->w),
        .scene = &f->scene,
        .surface = ui,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
}

static void show_hovered_preview(struct preview_fixture *f, int index)
{
    struct ui_surface *ui = window_ui_surface(f->w);
    hover_tab(f->w, index);
    uint64_t due = ui_surface_next_deadline(ui);
    assert(due > 0);
    ui_surface_tick(ui, due);
    ui_surface_prepare(ui);
    render_and_lint(f);
    assert(preview_shown(f->w));
}

static void zero_width_window_shows_no_preview_test(void)
{
    struct preview_fixture f;
    setup_fixture(&f, (struct vector2i){0}, 2);
    struct ui_surface *ui = window_ui_surface(f.w);
    pointer(f.w, UI_POINTER_MOTION, (struct vector2i){0}, 0,
            window_frame_time_ms(f.w));
    ui_surface_tick(ui, window_frame_time_ms(f.w) + 600);
    ui_surface_prepare(ui);
    assert(ui_surface_build_scene(ui, &f.scene));
    assert(!preview_shown(f.w));
    assert(!find_image_op(&f.scene));

    ui_surface_resize(ui, (struct vector2i){ .x = 800, .y = 480 });
    settle_surface(&f);
    show_hovered_preview(&f, 0);
    assert(find_image_op(&f.scene) != NULL);
    teardown_fixture(&f);
}

static void height_budget_and_anchor_refresh_test(void)
{
    struct preview_fixture f;
    setup_fixture(&f, (struct vector2i){ .x = 800, .y = 480 }, 2);
    settle_surface(&f);

    struct ui_surface *ui = window_ui_surface(f.w);
    const struct ui_widget *preview = preview_widget(f.w);
    show_hovered_preview(&f, 0);

    const struct ui_scene_op *op = find_image_op(&f.scene);
    assert(op && op->image);
    struct vector2i full_thumb = op->image->size;
    struct rect2i tab0 = bar_part_rect(f.w, "tab", 0);
    assert(full_thumb.x > 0 && full_thumb.y > 0);
    assert(preview->bounds.y >= rect2i_end(tab0).y);

    /* Shrink vertically so thumbnail scales down to fit budget. */
    int short_h = rect2i_end(tab0).y + 96;
    ui_surface_resize(ui, (struct vector2i){ .x = 800, .y = short_h });
    render_and_lint(&f);
    assert(preview_shown(f.w));
    op = find_image_op(&f.scene);
    assert(op && op->image);
    assert(op->image->size.x < full_thumb.x && op->image->size.y < full_thumb.y);
    assert(rect2i_end(preview->bounds).y <= short_h);

    /* Tiny max_height (< 24dp) drops sliver thumbnail to title-only card. */
    int sliver_h = rect2i_end(tab0).y + 56;
    ui_surface_resize(ui, (struct vector2i){ .x = 800, .y = sliver_h });
    render_and_lint(&f);
    assert(preview_shown(f.w));
    assert(!find_image_op(&f.scene));
    assert(rect2i_end(preview->bounds).y <= sliver_h);

    /* Zero max_height keeps title-only card. */
    int title_only_h = rect2i_end(tab0).y + 42;
    ui_surface_resize(ui, (struct vector2i){ .x = 800, .y = title_only_h });
    render_and_lint(&f);
    assert(preview_shown(f.w));
    assert(!find_image_op(&f.scene));
    assert(rect2i_end(preview->bounds).y <= title_only_h);

    /* Too small for title card hides preview cleanly. */
    ui_surface_resize(ui, (struct vector2i){
        .x = 800, .y = rect2i_end(tab0).y + 20,
    });
    render_and_lint(&f);
    assert(!preview_shown(f.w));
    assert(!find_image_op(&f.scene));

    /* Grow back, switch active tab to 0, hover tab 1, then widen window. */
    ui_surface_resize(ui, (struct vector2i){ .x = 800, .y = 480 });
    render_and_lint(&f);
    assert(preview_shown(f.w));
    assert(find_image_op(&f.scene) != NULL);

    app_tabs_select(f.w, 0);
    settle_surface(&f);
    show_hovered_preview(&f, 1);

    struct rect2i narrow_tab1 = bar_part_rect(f.w, "tab", 1);
    struct rect2i narrow_preview = preview->bounds;

    /* Widening moves tab 1 right; preview anchor follows without mouse move. */
    ui_surface_resize(ui, (struct vector2i){ .x = 1200, .y = 480 });
    render_and_lint(&f);
    struct rect2i wide_tab1 = bar_part_rect(f.w, "tab", 1);
    assert(wide_tab1.x > narrow_tab1.x);
    assert(preview->bounds.x > narrow_preview.x);
    assert(preview->bounds.x == wide_tab1.x);

    /* Doubling DPI scale without moving pointer moves preview below 2x tabbar. */
    ui_surface_set_scale(ui, (struct css_scale_context){
        .dpi_scale = 2.0f, .cell_w = 16.0f, .cell_h = 32.0f,
    });
    render_and_lint(&f);
    struct rect2i scaled_tab1 = bar_part_rect(f.w, "tab", 1);
    assert(rect2i_end(scaled_tab1).y > rect2i_end(wide_tab1).y);
    assert(preview_shown(f.w));
    assert(preview->bounds.y >= rect2i_end(scaled_tab1).y);
    assert(rect2i_end(preview->bounds).y <= 480);

    teardown_fixture(&f);
}

static void click_suppress_survives_resize_and_restyle_test(void)
{
    struct preview_fixture f;
    setup_fixture(&f, (struct vector2i){ .x = 800, .y = 480 }, 3);
    app_tabs_select(f.w, 0);
    settle_surface(&f);

    struct ui_surface *ui = window_ui_surface(f.w);
    show_hovered_preview(&f, 1);

    /* Activating tab 1 suppresses preview and must survive resize and scale. */
    click_at(f.w, rect2i_center(bar_part_rect(f.w, "tab", 1)));
    assert(app_tabs_active_index(f.w) == 1);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    ui_surface_resize(ui, (struct vector2i){ .x = 1000, .y = 480 });
    ui_surface_set_scale(ui, (struct css_scale_context){
        .dpi_scale = 2.0f, .cell_w = 16.0f, .cell_h = 32.0f,
    });
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    app_tabs_select(f.w, 0);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    ui_surface_tick(ui, window_frame_time_ms(f.w) + 1000);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    /* Moving within inactive tab 1 stays suppressed until leaving tabs. */
    hover_tab(f.w, 1);
    ui_surface_tick(ui, window_frame_time_ms(f.w) + 2000);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    /* Moving off and back onto inactive tab 1 restores hover preview. */
    pointer(f.w, UI_POINTER_LEAVE, (struct vector2i){0}, 0,
            window_frame_time_ms(f.w));
    show_hovered_preview(&f, 1);
    teardown_fixture(&f);
}

static void close_and_reorder_clear_stale_hover_test(void)
{
    struct preview_fixture f;
    setup_fixture(&f, (struct vector2i){ .x = 800, .y = 480 }, 3);
    app_tabs_select(f.w, 0);
    settle_surface(&f);

    /* Clicking close on tab 1 suppresses preview and avoids neighbor popup. */
    struct ui_surface *ui = window_ui_surface(f.w);
    show_hovered_preview(&f, 1);
    click_at(f.w, rect2i_center(bar_part_rect(f.w, "tab-close", 1)));
    assert(app_tabs_count(f.w) == 2);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    ui_surface_tick(ui, window_frame_time_ms(f.w) + APP_TABS_CLOSE_MS + 600);
    app_tabs_service(&f.app.app->tabs,
                     window_frame_time_ms(f.w) + APP_TABS_CLOSE_MS + 600);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    /* Nudging pointer over the shifted neighbor still stays suppressed. */
    hover_tab(f.w, 1);
    ui_surface_tick(ui, window_frame_time_ms(f.w) + 600);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    /* Leaving and hovering tab 1 shows preview; programmatic close hides it. */
    pointer(f.w, UI_POINTER_LEAVE, (struct vector2i){0}, 0,
            window_frame_time_ms(f.w));
    assert(app_tabs_new(f.w));
    app_tabs_select(f.w, 0);
    settle_surface(&f);
    show_hovered_preview(&f, 1);

    app_tabs_close(f.w, 1);
    ui_surface_tick(ui, window_frame_time_ms(f.w) + 600);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    app_tabs_service(&f.app.app->tabs,
                     window_frame_time_ms(f.w) + APP_TABS_CLOSE_MS + 600);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    /* Reordering hovered tab 1 away hides preview cleanly. */
    show_hovered_preview(&f, 1);
    app_tabs_reorder(f.w, 1, 0);
    ui_surface_prepare(ui);
    render_and_lint(&f);
    assert(!preview_shown(f.w));

    teardown_fixture(&f);
}

int main(void)
{
    zero_width_window_shows_no_preview_test();
    height_budget_and_anchor_refresh_test();
    click_suppress_survives_resize_and_restyle_test();
    close_and_reorder_clear_stale_hover_test();
    puts("PASS: app tabs preview tests");
    return 0;
}
