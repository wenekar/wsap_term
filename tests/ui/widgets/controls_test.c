#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/debug.h"
#include "ui/ui.h"
#include "ui/widget_ops.h"
#include "ui/widgets/icon.h"
#include "ui/widgets/progress.h"
#include "ui/widgets/slider.h"
#include "ui/widgets/spinner.h"

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
};

static void show(struct fixture *f, struct ui_widget *child)
{
    *f = (struct fixture){0};
    ui_widget_init(&f->root, NULL, NULL);
    assert(ui_widget_add_child(&f->root, child));
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 320, .y = 120 });
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
}

static void key(struct fixture *f, enum ui_key code)
{
    struct ui_event event = ui_event_key((struct ui_key_event){
        .key = code, .action = UI_KEY_PRESS,
    }, 0);
    (void)ui_surface_dispatch(&f->surface, &event);
}

static void lint(struct fixture *f)
{
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(&f->surface, &scene));
    struct ui_debug_view view = {
        .root = &f->root, .surface = &f->surface, .scene = &scene,
    };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    ui_scene_destroy(&scene);
}

static void pointer(struct fixture *f, enum ui_pointer_kind kind,
                    struct vector2i pos)
{
    struct ui_event event = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos,
    }, 0);
    (void)ui_surface_dispatch(&f->surface, &event);
}

static bool activated(struct ui_widget *widget, const struct ui_action *action,
                      void *userdata)
{
    (void)widget;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    (*(int *)userdata)++;
    return true;
}

static void slider_range_test(void)
{
    struct ui_slider slider;
    ui_slider_init(&slider);
    ui_slider_set_range(&slider, 0.0f, 10.0f);
    ui_slider_set_step(&slider, 1.0f);
    assert(ui_slider_min(&slider) == 0.0f);
    assert(ui_slider_max(&slider) == 10.0f);
    ui_slider_set_value(&slider, 4.4f);
    assert(ui_slider_value(&slider) == 4.0f);
    ui_slider_set_value(&slider, 99.0f);
    assert(ui_slider_value(&slider) == 10.0f);
    ui_slider_set_value(&slider, -5.0f);
    assert(ui_slider_value(&slider) == 0.0f);
    ui_widget_destroy(ui_slider_widget(&slider));
}

static void slider_keyboard_test(void)
{
    struct ui_slider slider;
    struct fixture f;
    ui_slider_init(&slider);
    ui_slider_set_range(&slider, 0.0f, 1.0f);
    ui_slider_set_step(&slider, 0.25f);
    show(&f, ui_slider_widget(&slider));
    ui_widget_focus(ui_slider_widget(&slider));
    key(&f, UI_KEY_RIGHT);
    assert(ui_slider_value(&slider) == 0.25f);
    key(&f, UI_KEY_LEFT);
    key(&f, UI_KEY_LEFT);
    assert(ui_slider_value(&slider) == 0.0f);
    key(&f, UI_KEY_END);
    assert(ui_slider_value(&slider) == 1.0f);
    key(&f, UI_KEY_HOME);
    assert(ui_slider_value(&slider) == 0.0f);
    lint(&f);
    teardown(&f);
}

static void slider_disabled_test(void)
{
    struct ui_slider slider;
    struct fixture f;
    ui_slider_init(&slider);
    ui_slider_set_range(&slider, 0.0f, 1.0f);
    ui_slider_set_step(&slider, 0.25f);
    show(&f, ui_slider_widget(&slider));
    ui_slider_set_disabled(&slider, true);
    assert(ui_slider_is_disabled(&slider));
    assert(!ui_widget_is_focusable(ui_slider_widget(&slider)));
    ui_widget_focus(ui_slider_widget(&slider));
    key(&f, UI_KEY_RIGHT);
    assert(ui_slider_value(&slider) == 0.0f);
    ui_slider_set_value(&slider, 0.5f);
    assert(ui_slider_value(&slider) == 0.5f);
    ui_slider_set_disabled(&slider, false);
    assert(!ui_slider_is_disabled(&slider));
    assert(ui_widget_is_focusable(ui_slider_widget(&slider)));
    lint(&f);
    teardown(&f);
}

static void progress_value_test(void)
{
    struct ui_progress progress;
    struct fixture f;
    ui_progress_init(&progress);
    assert(ui_progress_value(&progress) == 0.0f);
    ui_progress_set_value(&progress, 0.5f);
    assert(ui_progress_value(&progress) == 0.5f);
    ui_progress_set_value(&progress, 2.0f);
    assert(ui_progress_value(&progress) == 1.0f);
    ui_progress_set_value(&progress, -1.0f);
    assert(ui_progress_value(&progress) == 0.0f);
    assert(!ui_progress_is_indeterminate(&progress));
    ui_progress_set_indeterminate(&progress, true);
    assert(ui_progress_is_indeterminate(&progress));
    show(&f, ui_progress_widget(&progress));
    lint(&f);
    teardown(&f);
}

static void spinner_test(void)
{
    struct ui_spinner spinner;
    struct fixture f;
    ui_spinner_init(&spinner);
    assert(ui_spinner_is_running(&spinner));
    assert(ui_spinner_cell_span(&spinner) == 1);
    ui_spinner_set_cell_span(&spinner, 2);
    assert(ui_spinner_cell_span(&spinner) == 2);
    ui_spinner_set_cell_span(&spinner, 0);
    assert(ui_spinner_cell_span(&spinner) == 1);
    show(&f, ui_spinner_widget(&spinner));
    lint(&f);
    ui_spinner_set_running(&spinner, false);
    assert(!ui_spinner_is_running(&spinner));
    teardown(&f);
}

static void icon_test(void)
{
    static uint32_t pixels[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                                  0xFFFFFFFF };
    struct ui_icon icon = {0};
    struct fixture f;
    ui_icon_set_label(&icon, "X");
    assert(!strcmp(ui_icon_label(&icon), "X"));
    assert(!ui_icon_has_image(&icon));
    struct ui_image image = {
        .pixels = pixels, .size = { .x = 2, .y = 2 }, .gen = 1,
    };
    ui_icon_set_image(&icon, &image);
    assert(ui_icon_has_image(&icon));
    show(&f, ui_icon_widget(&icon));
    lint(&f);
    ui_icon_set_image(&icon, NULL);
    assert(!ui_icon_has_image(&icon));
    teardown(&f);
}

/* A click on the icon takes a press inside it and a release inside it. */
static void icon_click_test(void)
{
    struct ui_icon icon = {0};
    struct fixture f;
    int calls = 0;
    ui_icon_set_label(&icon, "X");
    ui_icon_set_action_id(&icon, 7);
    ui_widget_set_action_handler(ui_icon_widget(&icon), activated, &calls);
    show(&f, ui_icon_widget(&icon));
    lint(&f);
    struct rect2i bounds = ui_icon_widget(&icon)->bounds;
    struct vector2i inside = {
        .x = bounds.x + bounds.width / 2,
        .y = bounds.y + bounds.height / 2,
    };
    struct vector2i outside = { .x = 400, .y = 200 };

    /* A press that starts away from the icon does not arm it. */
    pointer(&f, UI_POINTER_PRESS, outside);
    pointer(&f, UI_POINTER_RELEASE, inside);
    assert(calls == 0);

    /* A press inside the icon and a release inside it activate it. */
    pointer(&f, UI_POINTER_PRESS, inside);
    pointer(&f, UI_POINTER_RELEASE, inside);
    assert(calls == 1);

    /* A release outside the icon cancels the press. */
    pointer(&f, UI_POINTER_PRESS, inside);
    pointer(&f, UI_POINTER_RELEASE, outside);
    assert(calls == 1);

    /* The second click still works after a cancelled press. */
    pointer(&f, UI_POINTER_PRESS, inside);
    pointer(&f, UI_POINTER_RELEASE, inside);
    assert(calls == 2);
    teardown(&f);
}

int main(void)
{
    slider_range_test();
    slider_keyboard_test();
    slider_disabled_test();
    progress_value_test();
    spinner_test();
    icon_test();
    icon_click_test();
    printf("control widget tests passed\n");
    return 0;
}
