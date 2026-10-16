#include "ui/ui.h"
#include "ui/accessibility.h"
#include "ui/widget_impl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void tree_ids_and_reuse(void)
{
    struct ui_accessibility_tree tree = {0};
    struct ui_accessibility_node_spec spec = {
        .id = UINT64_MAX,
        .role = UI_ACCESSIBILITY_ROLE_GROUP,
        .name = "Root",
    };
    assert(ui_accessibility_tree_add(&tree, &spec));
    assert(!ui_accessibility_tree_add(&tree, &spec));
    spec.id = 1;
    spec.parent_id = 2;
    assert(!ui_accessibility_tree_add(&tree, &spec));
    spec.parent_id = UINT64_MAX;
    for (uint64_t id = 1; id <= 4096; id++) {
        spec.id = id;
        spec.order = (uint32_t)(id - 1);
        assert(ui_accessibility_tree_add(&tree, &spec));
    }
    for (uint64_t id = 1; id <= 4096; id++) {
        const struct ui_accessibility_node *node = ui_accessibility_tree_find(&tree, id);
        assert(node && node->parent_id == UINT64_MAX);
        assert(node->order == id - 1);
    }
    assert(!ui_accessibility_tree_find(&tree, 4097));
    assert(!ui_accessibility_tree_find(&tree, 0));
    ui_accessibility_tree_clear(&tree);
    assert(!ui_accessibility_tree_find(&tree, 1));
    spec.id = 1;
    spec.parent_id = 0;
    assert(ui_accessibility_tree_add(&tree, &spec));
    assert(ui_accessibility_tree_find(&tree, 1));
    assert(!ui_accessibility_tree_find(&tree, UINT64_MAX));
    ui_accessibility_tree_destroy(&tree);
    assert(!ui_accessibility_tree_find(&tree, 1));
    assert(ui_accessibility_tree_add(&tree, &spec));
    ui_accessibility_tree_destroy(&tree);
}

static void text_copy_and_password(void)
{
    struct ui_accessibility_tree tree = {0};
    char value[] = "a\xff" "😀z";
    struct ui_accessibility_node_spec spec = {
        .id = 1,
        .role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD,
        .value = value,
        .selection_start = 2,
        .selection_end = SIZE_MAX,
        .caret_offset = 4,
    };
    assert(ui_accessibility_tree_add(&tree, &spec));
    value[0] = 'b';
    const struct ui_accessibility_node *node = ui_accessibility_tree_find(&tree, 1);
    assert(!strcmp(node->value, "a😀z"));
    assert(node->selection_start == 1 && node->selection_end == 6);
    assert(node->caret_offset == 1);

    spec.id = 2;
    spec.states = UI_ACCESSIBILITY_STATE_PASSWORD;
    assert(ui_accessibility_tree_add(&tree, &spec));
    node = ui_accessibility_tree_find(&tree, 2);
    assert(!node->value);
    assert(!node->caret_offset && !node->selection_start && !node->selection_end);
    ui_accessibility_tree_destroy(&tree);
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

static void live_actions_and_modal_scope(void)
{
    struct ui_box root = {0}, dialog = {0};
    struct ui_button outside = {0}, inside = {0};
    struct ui_input input = {0};
    struct ui_surface surface = {0};
    struct ui_widget *root_widget = ui_box_widget(&root);
    struct ui_widget *dialog_widget = ui_box_widget(&dialog);
    struct ui_widget *outside_widget = ui_button_widget(&outside);
    struct ui_widget *inside_widget = ui_button_widget(&inside);
    struct ui_widget *input_widget = ui_input_widget(&input);
    assert(ui_widget_add_child(root_widget, outside_widget));
    assert(ui_widget_add_child(root_widget, dialog_widget));
    assert(ui_widget_add_child(dialog_widget, inside_widget));
    assert(ui_widget_add_child(dialog_widget, input_widget));
    ui_surface_set_root(&surface, root_widget);
    int calls = 0;
    ui_widget_set_action_handler(outside_widget, activated, &calls);
    ui_widget_set_action_handler(inside_widget, activated, &calls);

    struct ui_accessibility_action_request request = {
        .node_id = ui_widget_accessibility_id(outside_widget),
        .action = UI_ACCESSIBILITY_ACTION_INVOKE,
    };
    assert(ui_accessibility_dispatch_action(root_widget, &request));
    ui_button_set_disabled(&outside, true);
    assert(!ui_accessibility_dispatch_action(root_widget, &request));
    ui_button_set_disabled(&outside, false);
    ui_widget_set_modal(dialog_widget, true);
    assert(!ui_accessibility_dispatch_action(root_widget, &request));
    request.node_id = ui_widget_accessibility_id(inside_widget);
    assert(ui_accessibility_dispatch_action(root_widget, &request));
    assert(calls == 2);
    request.action = UI_ACCESSIBILITY_ACTION_INVOKE | UI_ACCESSIBILITY_ACTION_FOCUS;
    assert(!ui_accessibility_dispatch_action(root_widget, &request));
    request.action = UI_ACCESSIBILITY_ACTION_SET_VALUE;
    assert(!ui_accessibility_dispatch_action(root_widget, &request));

    request.node_id = ui_widget_accessibility_id(input_widget);
    request.value = "a😀z";
    request.caret_offset = SIZE_MAX;
    assert(ui_accessibility_dispatch_action(root_widget, &request));
    assert(!strcmp(ui_input_text(&input), "a😀z"));
    assert(ui_input_caret(&input) == 6);
    request.action = UI_ACCESSIBILITY_ACTION_SET_SELECTION;
    request.selection_start = 2;
    request.selection_end = 5;
    assert(ui_accessibility_dispatch_action(root_widget, &request));
    int start, end;
    assert(ui_input_selection(&input, &start, &end));
    assert(start == 1 && end == 5);
    ui_widget_set_hidden(dialog_widget, true);
    assert(!ui_accessibility_dispatch_action(root_widget, &request));
    ui_widget_set_hidden(dialog_widget, false);
    ui_widget_set_accessibility_ignored(input_widget, true);
    assert(!ui_accessibility_dispatch_action(root_widget, &request));

    struct ui_accessibility_tree tree = {0};
    assert(ui_accessibility_tree_build(root_widget, &tree));
    assert(!ui_accessibility_tree_find(&tree, ui_widget_accessibility_id(outside_widget)));
    assert(!ui_accessibility_tree_find(&tree, ui_widget_accessibility_id(input_widget)));
    assert(ui_accessibility_tree_find(&tree, ui_widget_accessibility_id(inside_widget)));
    ui_accessibility_tree_destroy(&tree);
    ui_surface_destroy(&surface);
    ui_widget_destroy(root_widget);
}

int main(void)
{
    tree_ids_and_reuse();
    text_copy_and_password();
    live_actions_and_modal_scope();
    puts("PASS: accessibility tree and actions");
    return 0;
}
