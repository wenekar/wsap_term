#include "ui/yasl/accessibility/accessibility.h"
#include "yasl/yasl.h"

#import <AppKit/AppKit.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ui_accessibility_action_request received;
static char *received_value;
static unsigned received_count;

static void action_sink(void *userdata,
                         const struct ui_accessibility_action_request *request)
{
    (void)userdata;
    received = *request;
    free(received_value);
    received_value = request->value ? strdup(request->value) : NULL;
    received.value = received_value;
    received_count++;
}

static void wake_sink(void *userdata)
{
    *(bool *)userdata = true;
}

static id<NSAccessibility> find_node(id<NSAccessibility> root, NSString *title)
{
    if ([[root accessibilityTitle] isEqualToString:title])
        return root;
    for (id child in [root accessibilityChildren]) {
        id found = find_node(child, title);
        if (found)
            return found;
    }
    return nil;
}

static void add(struct ui_accessibility_tree *tree,
                 struct ui_accessibility_node_spec spec)
{
    assert(ui_accessibility_tree_add(tree, &spec));
}

int main(void)
{
    @autoreleasepool {
        struct yasl_surface *surface = yasl_surface_create(
            &(struct yasl_surface_options){.title = "Accessibility test",
                .width = 800, .height = 600, .type = YASL_SURFACE_NATIVE});
        assert(surface);
        struct yasl_native native = {0};
        assert(!yasl_surface_get_native(surface, &native) && native.ns_view);
        NSView *view = native.ns_view;
        struct ui_accessibility_provider provider = {0};
        assert(ui_accessibility_provider_attach(&provider, ui_accessibility_provider_ops(),
            surface, action_sink, NULL));
        bool woke = false;
        ui_accessibility_set_wake_callback(&provider, wake_sink, &woke);
        (void)[view accessibilityChildren];
        assert(woke && ui_accessibility_provider_is_active(&provider));

        struct ui_accessibility_tree tree = {0};
        add(&tree, (struct ui_accessibility_node_spec){
            .id = 1, .role = UI_ACCESSIBILITY_ROLE_WINDOW, .name = "Fixture",
        });
        add(&tree, (struct ui_accessibility_node_spec){
            .id = UINT64_MAX, .parent_id = 1, .role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD,
            .name = "Editor", .value = "Aé🙂\nβ", .selection_start = 1,
            .selection_end = 7, .caret_offset = 7,
            .states = UI_ACCESSIBILITY_STATE_EDITABLE | UI_ACCESSIBILITY_STATE_MULTILINE,
            .actions = UI_ACCESSIBILITY_ACTION_SET_SELECTION | UI_ACCESSIBILITY_ACTION_SET_VALUE,
        });
        add(&tree, (struct ui_accessibility_node_spec){
            .id = 3, .parent_id = 1, .order = 1,
            .role = UI_ACCESSIBILITY_ROLE_BUTTON, .name = "Run",
            .actions = UI_ACCESSIBILITY_ACTION_INVOKE,
        });
        add(&tree, (struct ui_accessibility_node_spec){
            .id = 4, .parent_id = 1, .order = 2,
            .role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD, .name = "Secret",
            .states = UI_ACCESSIBILITY_STATE_PASSWORD, .value = "hidden secret",
        });
        assert(ui_accessibility_provider_publish_snapshot(&provider, &tree));
        ui_accessibility_tree_destroy(&tree);
        id<NSAccessibility> editor = find_node((id)view, @"Editor");
        assert(editor);
        assert([[editor accessibilityValue] isEqualToString:@"Aé🙂\nβ"]);
        assert(NSEqualRanges([editor accessibilitySelectedTextRange], NSMakeRange(1, 3)));
        [editor setAccessibilitySelectedTextRange:NSMakeRange(2, 3)];
        assert(received_count == 1 && received.node_id == UINT64_MAX);
        assert(received.action == UI_ACCESSIBILITY_ACTION_SET_SELECTION);
        assert(received.selection_start == 3 && received.selection_end == 8);
        [editor setAccessibilityValue:@"更新🙂"];
        assert(received_count == 2 && received.action == UI_ACCESSIBILITY_ACTION_SET_VALUE);
        assert(!strcmp(received.value, "更新🙂"));
        id<NSAccessibility> button = find_node((id)view, @"Run");
        assert(button && [button accessibilityPerformPress]);
        assert(received_count == 3 && received.node_id == 3);
        id<NSAccessibility> secret = find_node((id)view, @"Secret");
        assert(secret && ![[secret accessibilityValue] isEqualToString:@"hidden secret"]);
        assert([[secret accessibilitySubrole] isEqualToString:NSAccessibilitySecureTextFieldSubrole]);

        id<NSAccessibility> stale = [editor retain];
        ui_accessibility_provider_detach(&provider);
        [stale setAccessibilityValue:@"stale"];
        assert(received_count == 3);
        [stale release];
        free(received_value);
        assert(!yasl_surface_destroy(surface));
        assert(!yasl_shutdown());
        puts("PASS: AppKit text, UTF-16 ranges, actions, passwords, and detach");
    }
    return 0;
}
