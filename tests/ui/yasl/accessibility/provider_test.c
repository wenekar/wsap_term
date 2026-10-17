#define _GNU_SOURCE
#include "ui/yasl/accessibility/accessibility.h"

#include <assert.h>
#include <gio/gio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACCESSIBLE "org.a11y.atspi.Accessible"
#define TEXT "org.a11y.atspi.Text"
#define ROOT_PATH "/org/a11y/atspi/accessible/root"

struct reference { char *bus, *path; };
static GDBusConnection *bus;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct ui_accessibility_action_request received;
static char *received_value;
static unsigned received_count;
static bool wake_requested;

static GVariant *try_call(const char *dest, const char *path, const char *iface,
                           const char *method, GVariant *args)
{
    return g_dbus_connection_call_sync(bus, dest, path, iface, method, args,
        NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
}

static GVariant *call(struct reference ref, const char *iface,
                      const char *method, GVariant *args)
{
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(bus, ref.bus, ref.path, iface,
        method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    if (!result)
        g_error("%s.%s: %s", iface, method, error->message);
    return result;
}

static GVariant *property(struct reference ref, const char *iface, const char *name)
{
    GVariant *reply = call(ref, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", iface, name));
    GVariant *value;
    g_variant_get(reply, "(v)", &value);
    g_variant_unref(reply);
    return value;
}

static void reference_clear(struct reference *ref)
{
    g_free(ref->bus);
    g_free(ref->path);
    *ref = (struct reference){0};
}

static bool find_node(struct reference root, const char *name, unsigned depth,
                      struct reference *found)
{
    GVariant *reply = try_call(root.bus, root.path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", ACCESSIBLE, "Name"));
    if (!reply)
        return false;
    GVariant *value;
    g_variant_get(reply, "(v)", &value);
    bool match = !strcmp(g_variant_get_string(value, NULL), name);
    g_variant_unref(value);
    g_variant_unref(reply);
    if (match) {
        *found = (struct reference){g_strdup(root.bus), g_strdup(root.path)};
        return true;
    }
    if (!depth)
        return false;
    reply = try_call(root.bus, root.path, ACCESSIBLE, "GetChildren", NULL);
    if (!reply)
        return false;
    GVariant *children;
    g_variant_get(reply, "(@a(so))", &children);
    GVariantIter iter;
    g_variant_iter_init(&iter, children);
    const char *dest, *path;
    while (!match && g_variant_iter_loop(&iter, "(&s&o)", &dest, &path))
        match = find_node((struct reference){(char *)dest, (char *)path},
                          name, depth - 1, found);
    g_variant_unref(children);
    g_variant_unref(reply);
    return match;
}

static struct reference await_node(const char *name)
{
    for (unsigned attempt = 0; attempt < 300; attempt++) {
        GVariant *reply = try_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "ListNames", NULL);
        assert(reply);
        GVariant *names;
        g_variant_get(reply, "(@as)", &names);
        GVariantIter iter;
        g_variant_iter_init(&iter, names);
        const char *dest;
        struct reference found = {0};
        while (!found.bus && g_variant_iter_loop(&iter, "&s", &dest))
            if (dest[0] == ':')
                find_node((struct reference){(char *)dest, ROOT_PATH}, name, 12, &found);
        g_variant_unref(names);
        g_variant_unref(reply);
        if (found.bus)
            return found;
        g_usleep(10000);
    }
    g_error("Accessible node did not appear: %s", name);
}

static void action_sink(void *userdata,
                         const struct ui_accessibility_action_request *request)
{
    (void)userdata;
    pthread_mutex_lock(&lock);
    received = *request;
    free(received_value);
    received_value = request->value ? strdup(request->value) : NULL;
    received.value = received_value;
    received_count++;
    pthread_mutex_unlock(&lock);
}

static void wake_sink(void *userdata)
{
    (void)userdata;
    pthread_mutex_lock(&lock);
    wake_requested = true;
    pthread_mutex_unlock(&lock);
}

static void await_activation(struct ui_accessibility_provider *provider,
                              const struct ui_accessibility_tree *tree)
{
    for (unsigned i = 0; i < 300; i++) {
        pthread_mutex_lock(&lock);
        bool ready = wake_requested;
        wake_requested = false;
        pthread_mutex_unlock(&lock);
        if (ready) {
            assert(ui_accessibility_provider_is_active(provider));
            assert(ui_accessibility_provider_publish_snapshot(provider, tree));
            return;
        }
        g_usleep(10000);
    }
    assert(!"Activation did not wake the UI thread");
}

static void await_action(enum ui_accessibility_action action, uint64_t id,
                         size_t start, size_t end, const char *value)
{
    for (unsigned i = 0; i < 300; i++) {
        pthread_mutex_lock(&lock);
        bool ready = received_count != 0;
        if (ready) {
            if (received.selection_start != start || received.selection_end != end)
                fprintf(stderr, "Action %u: range %zu..%zu, expected %zu..%zu\n",
                        received.action, received.selection_start, received.selection_end,
                        start, end);
            assert(received.action == action && received.node_id == id);
            assert(received.selection_start == start && received.selection_end == end);
            assert(!value || (received.value && !strcmp(received.value, value)));
            received_count = 0;
        }
        pthread_mutex_unlock(&lock);
        if (ready)
            return;
        g_usleep(10000);
    }
    assert(!"Native action did not reach the provider sink");
}

static void assert_text(struct reference ref, const char *expected)
{
    GVariant *reply = call(ref, TEXT, "GetText", g_variant_new("(ii)", 0, -1));
    const char *text;
    g_variant_get(reply, "(&s)", &text);
    assert(!strcmp(text, expected));
    g_variant_unref(reply);
}

static void invoke(struct reference ref, const char *iface, const char *method,
                    GVariant *args)
{
    GVariant *reply = call(ref, iface, method, args);
    gboolean accepted;
    g_variant_get(reply, "(b)", &accepted);
    assert(accepted);
    g_variant_unref(reply);
}

static void add(struct ui_accessibility_tree *tree,
                 struct ui_accessibility_node_spec spec)
{
    assert(ui_accessibility_tree_add(tree, &spec));
}

int main(void)
{
    GError *error = NULL;
    GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!session)
        return 77;
    GVariant *reply = g_dbus_connection_call_sync(session, "org.a11y.Bus",
        "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.a11y.Status", "IsEnabled", g_variant_new_boolean(true)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    if (!reply) {
        fprintf(stderr, "SKIP: AT-SPI bus is unavailable: %s\n", error->message);
        g_error_free(error);
        g_object_unref(session);
        return 77;
    }
    g_variant_unref(reply);
    reply = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus",
        "org.a11y.Bus", "GetAddress", NULL, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    assert(reply);
    const char *address;
    g_variant_get(reply, "(&s)", &address);
    bus = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
    assert(bus);
    g_variant_unref(reply);

    struct ui_accessibility_tree tree = {0};
    add(&tree, (struct ui_accessibility_node_spec){
        .id = UINT64_MAX, .role = UI_ACCESSIBILITY_ROLE_WINDOW, .name = "Fixture",
        .bounds = {.width = 800, .height = 600},
    });
    const char *unicode = "Aé🙂\r\nβ\n";
    add(&tree, (struct ui_accessibility_node_spec){
        .id = UINT64_MAX - 1, .parent_id = UINT64_MAX, .order = 1,
        .role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD, .name = "Editor", .value = unicode,
        .states = UI_ACCESSIBILITY_STATE_EDITABLE | UI_ACCESSIBILITY_STATE_MULTILINE |
                  UI_ACCESSIBILITY_STATE_FOCUSABLE | UI_ACCESSIBILITY_STATE_FOCUSED,
        .actions = UI_ACCESSIBILITY_ACTION_SET_VALUE | UI_ACCESSIBILITY_ACTION_SET_SELECTION |
                   UI_ACCESSIBILITY_ACTION_FOCUS,
        .selection_start = 1, .selection_end = 7, .caret_offset = 7,
    });
    add(&tree, (struct ui_accessibility_node_spec){
        .id = 2, .parent_id = UINT64_MAX, .order = 0,
        .role = UI_ACCESSIBILITY_ROLE_LABEL, .name = "Static label",
    });
    add(&tree, (struct ui_accessibility_node_spec){
        .id = 3, .parent_id = UINT64_MAX, .order = 2,
        .role = UI_ACCESSIBILITY_ROLE_BUTTON, .name = "Run",
        .actions = UI_ACCESSIBILITY_ACTION_INVOKE,
    });
    add(&tree, (struct ui_accessibility_node_spec){
        .id = 4, .parent_id = UINT64_MAX, .order = 3,
        .role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD, .name = "Secret", .value = "hidden secret",
        .states = UI_ACCESSIBILITY_STATE_PASSWORD, .caret_offset = 9,
    });
    add(&tree, (struct ui_accessibility_node_spec){
        .id = 5, .parent_id = UINT64_MAX, .order = 4,
        .role = UI_ACCESSIBILITY_ROLE_LINK, .name = "Selectable link", .value = "xyé",
        .actions = UI_ACCESSIBILITY_ACTION_INVOKE | UI_ACCESSIBILITY_ACTION_SET_SELECTION,
    });
    add(&tree, (struct ui_accessibility_node_spec){
        .id = 6, .parent_id = UINT64_MAX, .order = 5,
        .role = UI_ACCESSIBILITY_ROLE_BUTTON, .name = "Disabled",
        .states = UI_ACCESSIBILITY_STATE_DISABLED,
        .actions = UI_ACCESSIBILITY_ACTION_INVOKE,
    });
    struct ui_accessibility_provider provider = {0};
    assert(ui_accessibility_provider_attach(&provider, ui_accessibility_provider_ops(),
                                            NULL, action_sink, NULL));
    ui_accessibility_set_wake_callback(&provider, wake_sink, NULL);
    await_activation(&provider, &tree);
    /* The provider owns an independent copy of this snapshot. */
    ui_accessibility_tree_destroy(&tree);
    struct reference editor = await_node("Editor");
    assert_text(editor, unicode);
    GVariant *caret = property(editor, TEXT, "CaretOffset");
    assert(g_variant_get_int32(caret) == 3);
    g_variant_unref(caret);
    reply = call(editor, ACCESSIBLE, "GetIndexInParent", NULL);
    int index;
    g_variant_get(reply, "(i)", &index);
    assert(index == 1);
    g_variant_unref(reply);
    invoke(editor, "org.a11y.atspi.Component", "GrabFocus", NULL);
    await_action(UI_ACCESSIBILITY_ACTION_FOCUS, UINT64_MAX - 1, 0, 0, NULL);
    reply = call(editor, TEXT, "GetSelection", g_variant_new("(i)", 0));
    int start, end;
    g_variant_get(reply, "(ii)", &start, &end);
    assert(start == 1 && end == 3);
    g_variant_unref(reply);
    invoke(editor, TEXT, "SetSelection", g_variant_new("(iii)", 0, 2, 6));
    await_action(UI_ACCESSIBILITY_ACTION_SET_SELECTION, UINT64_MAX - 1, 3, 11, NULL);
    invoke(editor, TEXT, "SetCaretOffset", g_variant_new("(i)", 7));
    await_action(UI_ACCESSIBILITY_ACTION_SET_SELECTION, UINT64_MAX - 1, 12, 12, NULL);
    invoke(editor, "org.a11y.atspi.EditableText", "SetTextContents",
           g_variant_new("(s)", "Nexté"));
    await_action(UI_ACCESSIBILITY_ACTION_SET_VALUE, UINT64_MAX - 1, 6, 6, "Nexté");
    struct reference button = await_node("Run");
    invoke(button, "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0));
    await_action(UI_ACCESSIBILITY_ACTION_INVOKE, 3, 0, 0, NULL);
    struct reference label = await_node("Static label");
    reference_clear(&label);
    struct reference secret = await_node("Secret");
    assert_text(secret, "");
    reference_clear(&secret);
    struct reference link_text = await_node("xyé");
    assert_text(link_text, "xyé");
    invoke(link_text, TEXT, "SetCaretOffset", g_variant_new("(i)", 3));
    await_action(UI_ACCESSIBILITY_ACTION_SET_SELECTION, 5, 4, 4, NULL);
    reference_clear(&link_text);
    struct reference disabled = await_node("Disabled");
    GVariant *rejected = try_call(disabled.bus, disabled.path, "org.a11y.atspi.Action",
                                   "DoAction", g_variant_new("(i)", 0));
    if (rejected) g_variant_unref(rejected);
    reference_clear(&disabled);

    add(&tree, (struct ui_accessibility_node_spec){
        .id = UINT64_MAX, .role = UI_ACCESSIBILITY_ROLE_WINDOW, .name = "Fixture",
    });
    add(&tree, (struct ui_accessibility_node_spec){
        .id = UINT64_MAX - 1, .parent_id = UINT64_MAX,
        .role = UI_ACCESSIBILITY_ROLE_TEXT_FIELD, .name = "Editor", .value = "Updated",
        .states = UI_ACCESSIBILITY_STATE_READ_ONLY,
    });
    assert(ui_accessibility_provider_publish_snapshot(&provider, &tree));
    ui_accessibility_tree_destroy(&tree);
    assert_text(editor, "Updated");
    reply = call(editor, "org.a11y.atspi.EditableText", "SetTextContents",
                   g_variant_new("(s)", "Cannot edit"));
    gboolean accepted;
    g_variant_get(reply, "(b)", &accepted);
    assert(!accepted);
    g_variant_unref(reply);
    /* A removed native reference must not deliver a stale action. */
    GVariant *stale = try_call(button.bus, button.path, "org.a11y.atspi.Action",
                               "DoAction", g_variant_new("(i)", 0));
    if (stale) g_variant_unref(stale);
    g_usleep(100000);
    pthread_mutex_lock(&lock);
    assert(received_count == 0);
    pthread_mutex_unlock(&lock);
    ui_accessibility_provider_detach(&provider);
    for (unsigned i = 0; i < 20; i++) {
        assert(ui_accessibility_provider_attach(&provider, ui_accessibility_provider_ops(),
                                                NULL, action_sink, NULL));
        ui_accessibility_provider_detach(&provider);
    }
    stale = try_call(editor.bus, editor.path, TEXT, "SetCaretOffset", g_variant_new("(i)", 1));
    if (stale) g_variant_unref(stale);
    g_usleep(100000);
    pthread_mutex_lock(&lock);
    assert(received_count == 0);
    pthread_mutex_unlock(&lock);
    reference_clear(&editor);
    reference_clear(&button);
    free(received_value);
    g_object_unref(bus);
    g_object_unref(session);
    puts("PASS: AT-SPI text, actions, snapshot updates, passwords, and detach");
    return 0;
}
