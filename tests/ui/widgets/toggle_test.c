#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/debug.h"
#include "ui/widgets/toggle.h"
#include "ui/ui.h"
#include "ui/widget_ops.h"

struct fixture {
    struct ui_surface surface;
    struct ui_widget root;
    struct ui_toggle toggle;
    int activations;
    int last_index;
};

static bool on_action(struct ui_widget *source, const struct ui_action *action,
                      void *userdata)
{
    (void)source;
    struct fixture *f = userdata;
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    f->activations++;
    f->last_index = action->index;
    return true;
}

static void show(struct fixture *f, enum ui_toggle_kind kind)
{
    *f = (struct fixture){0};
    ui_widget_init(&f->root, NULL, NULL);
    ui_toggle_init(&f->toggle, kind);
    ui_toggle_set_label(&f->toggle, "Wrap lines");
    ui_toggle_set_action_id(&f->toggle, 7);
    assert(ui_widget_add_child(&f->root, ui_toggle_widget(&f->toggle)));
    ui_widget_set_action_handler(&f->root, on_action, f);
    ui_surface_set_root(&f->surface, &f->root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 320, .y = 120 });
}

static void teardown(struct fixture *f)
{
    ui_surface_destroy(&f->surface);
}

static void key(struct fixture *f, enum ui_key code, enum ui_key_action action)
{
    struct ui_event event = ui_event_key((struct ui_key_event){
        .key = code, .action = action,
    }, 0);
    (void)ui_surface_dispatch(&f->surface, &event);
}

static void press_space(struct fixture *f)
{
    key(f, UI_KEY_CHAR, UI_KEY_PRESS);
    struct ui_event event = ui_event_key((struct ui_key_event){
        .key = UI_KEY_CHAR, .text = ' ', .action = UI_KEY_PRESS,
    }, 1);
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

static void accessibility(struct fixture *f, enum ui_accessibility_role role)
{
    struct ui_accessibility_tree tree = {0};
    assert(ui_accessibility_tree_build(&f->root, &tree));
    bool found = false;
    for (size_t i = 0; i < tree.count; i++) {
        if (tree.nodes[i].role != role)
            continue;
        found = true;
        bool checked = ui_toggle_is_checked(&f->toggle);
        assert((tree.nodes[i].states & UI_ACCESSIBILITY_STATE_CHECKED) ==
               (checked ? UI_ACCESSIBILITY_STATE_CHECKED : 0));
        assert(!strcmp(tree.nodes[i].name, "Wrap lines"));
    }
    assert(found);
    ui_accessibility_tree_destroy(&tree);
}

static void checkbox_state_test(void)
{
    struct fixture f;
    show(&f, UI_TOGGLE_CHECKBOX);
    assert(!ui_toggle_is_checked(&f.toggle));
    assert(ui_toggle_kind_of(&f.toggle) == UI_TOGGLE_CHECKBOX);
    ui_toggle_set_checked(&f.toggle, true);
    assert(ui_toggle_is_checked(&f.toggle));
    assert(f.activations == 0);
    accessibility(&f, UI_ACCESSIBILITY_ROLE_CHECKBOX);
    ui_toggle_set_checked(&f.toggle, true);
    assert(f.activations == 0);
    lint(&f);
    teardown(&f);
}

static void checkbox_keyboard_test(void)
{
    struct fixture f;
    show(&f, UI_TOGGLE_CHECKBOX);
    ui_widget_focus(ui_toggle_widget(&f.toggle));
    press_space(&f);
    assert(ui_toggle_is_checked(&f.toggle));
    assert(f.activations == 1);
    assert(f.last_index == 1);
    press_space(&f);
    assert(!ui_toggle_is_checked(&f.toggle));
    assert(f.activations == 2);
    assert(f.last_index == 0);
    key(&f, UI_KEY_ENTER, UI_KEY_PRESS);
    assert(ui_toggle_is_checked(&f.toggle));
    key(&f, UI_KEY_ENTER, UI_KEY_RELEASE);
    assert(f.activations == 3);
    lint(&f);
    teardown(&f);
}

static void disabled_test(void)
{
    struct fixture f;
    show(&f, UI_TOGGLE_CHECKBOX);
    ui_toggle_set_disabled(&f.toggle, true);
    assert(ui_toggle_is_disabled(&f.toggle));
    assert(!ui_widget_is_focusable(ui_toggle_widget(&f.toggle)));
    ui_widget_focus(ui_toggle_widget(&f.toggle));
    press_space(&f);
    assert(!ui_toggle_is_checked(&f.toggle));
    assert(f.activations == 0);
    ui_toggle_set_checked(&f.toggle, true);
    assert(ui_toggle_is_checked(&f.toggle));
    ui_toggle_set_disabled(&f.toggle, false);
    assert(!ui_toggle_is_disabled(&f.toggle));
    lint(&f);
    teardown(&f);
}

static void radio_range_test(void)
{
    struct fixture f;
    show(&f, UI_TOGGLE_RADIO);
    assert(ui_toggle_kind_of(&f.toggle) == UI_TOGGLE_RADIO);
    ui_widget_focus(ui_toggle_widget(&f.toggle));
    press_space(&f);
    assert(ui_toggle_is_checked(&f.toggle));
    press_space(&f);
    assert(ui_toggle_is_checked(&f.toggle));
    assert(f.activations == 2);
    accessibility(&f, UI_ACCESSIBILITY_ROLE_RADIO_BUTTON);
    lint(&f);
    teardown(&f);
}

static void switch_test(void)
{
    struct fixture f;
    show(&f, UI_TOGGLE_SWITCH);
    assert(ui_toggle_kind_of(&f.toggle) == UI_TOGGLE_SWITCH);
    ui_widget_focus(ui_toggle_widget(&f.toggle));
    press_space(&f);
    assert(ui_toggle_is_checked(&f.toggle));
    assert(f.activations == 1);
    lint(&f);
    teardown(&f);
}

int main(void)
{
    checkbox_state_test();
    checkbox_keyboard_test();
    disabled_test();
    radio_range_test();
    switch_test();
    printf("toggle tests passed\n");
    return 0;
}
