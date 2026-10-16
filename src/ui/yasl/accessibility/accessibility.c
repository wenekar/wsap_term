#define _GNU_SOURCE

#include "ui/yasl/accessibility/accessibility.h"

#include "util/geom.h"
#include "yasl/yasl.h"

#include <gio/gio.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define ATSPI_ROOT_PATH "/org/a11y/atspi/accessible/root"
#define ATSPI_NULL_PATH "/org/a11y/atspi/null"
#define DBUS_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define ATSPI_REGISTRY_NAME "org.a11y.atspi.Registry"
#define ATSPI_SOCKET_IFACE "org.a11y.atspi.Socket"
#define ATSPI_ACCESSIBLE_IFACE "org.a11y.atspi.Accessible"
#define ATSPI_APPLICATION_IFACE "org.a11y.atspi.Application"
#define ATSPI_COMPONENT_IFACE "org.a11y.atspi.Component"
#define ATSPI_ACTION_IFACE "org.a11y.atspi.Action"
#define ATSPI_TEXT_IFACE "org.a11y.atspi.Text"
#define ATSPI_EDITABLE_TEXT_IFACE "org.a11y.atspi.EditableText"
#define ATSPI_EVENT_OBJECT_IFACE "org.a11y.atspi.Event.Object"
#define ATSPI_EVENT_FOCUS_IFACE "org.a11y.atspi.Event.Focus"

/* Keep this provider on GIO. The libatspi package is a client library, not a
 * server adapter, so the provider exports the small AT-SPI D-Bus surface it
 * needs directly. */
static const char atspi_xml[] =
    "<node>"
    " <interface name='org.a11y.atspi.Accessible'>"
    "  <property name='version' type='u' access='read'/>"
    "  <property name='Name' type='s' access='read'/>"
    "  <property name='Description' type='s' access='read'/>"
    "  <property name='Parent' type='(so)' access='read'/>"
    "  <property name='ChildCount' type='i' access='read'/>"
    "  <property name='Locale' type='s' access='read'/>"
    "  <property name='AccessibleId' type='s' access='read'/>"
    "  <property name='HelpText' type='s' access='read'/>"
    "  <method name='GetChildAtIndex'><arg direction='in' type='i'/><arg "
    "direction='out' type='(so)'/></method>"
    "  <method name='GetChildren'><arg direction='out' type='a(so)'/></method>"
    "  <method name='GetIndexInParent'><arg direction='out' type='i'/></method>"
    "  <method name='GetRelationSet'><arg direction='out' "
    "type='a(ua(so))'/></method>"
    "  <method name='GetRole'><arg direction='out' type='u'/></method>"
    "  <method name='GetRoleName'><arg direction='out' type='s'/></method>"
    "  <method name='GetLocalizedRoleName'><arg direction='out' "
    "type='s'/></method>"
    "  <method name='GetState'><arg direction='out' type='au'/></method>"
    "  <method name='GetAttributes'><arg direction='out' "
    "type='a{ss}'/></method>"
    "  <method name='GetApplication'><arg direction='out' "
    "type='(so)'/></method>"
    "  <method name='GetInterfaces'><arg direction='out' type='as'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Application'>"
    "  <property name='ToolkitName' type='s' access='read'/>"
    "  <property name='ToolkitVersion' type='s' access='read'/>"
    "  <property name='AtspiVersion' type='s' access='read'/>"
    "  <property name='InterfaceVersion' type='u' access='read'/>"
    "  <property name='Id' type='i' access='readwrite'/>"
    "  <method name='GetLocale'><arg direction='in' type='u'/><arg "
    "direction='out' type='s'/></method>"
    "  <method name='GetApplicationBusAddress'><arg direction='out' "
    "type='s'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Component'>"
    "  <property name='version' type='u' access='read'/>"
    "  <method name='Contains'><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='in' type='u'/><arg "
    "direction='out' type='b'/></method>"
    "  <method name='GetAccessibleAtPoint'><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='in' type='u'/><arg "
    "direction='out' type='(so)'/></method>"
    "  <method name='GetExtents'><arg direction='in' type='u'/><arg "
    "direction='out' type='(iiii)'/></method>"
    "  <method name='GetPosition'><arg direction='in' type='u'/><arg "
    "direction='out' name='x' type='i'/><arg direction='out' name='y' "
    "type='i'/></method>"
    "  <method name='GetSize'><arg direction='out' name='width' type='i'/><arg "
    "direction='out' name='height' type='i'/></method>"
    "  <method name='GetLayer'><arg direction='out' type='u'/></method>"
    "  <method name='GetMDIZOrder'><arg direction='out' type='n'/></method>"
    "  <method name='GrabFocus'><arg direction='out' type='b'/></method>"
    "  <method name='GetAlpha'><arg direction='out' type='d'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Text'>"
    "  <property name='version' type='u' access='read'/>"
    "  <property name='CharacterCount' type='i' access='read'/>"
    "  <property name='CaretOffset' type='i' access='read'/>"
    "  <method name='GetText'><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='out' type='s'/></method>"
    "  <method name='GetCharacterCount'><arg direction='out' "
    "type='i'/></method>"
    "  <method name='GetCharacterAtOffset'><arg direction='in' type='i'/><arg "
    "direction='out' type='i'/></method>"
    "  <method name='GetCaretOffset'><arg direction='out' type='i'/></method>"
    "  <method name='GetNSelections'><arg direction='out' type='i'/></method>"
    "  <method name='GetSelection'><arg direction='in' type='i'/><arg "
    "direction='out' name='startOffset' type='i'/><arg direction='out' "
    "name='endOffset' type='i'/></method>"
    "  <method name='SetCaretOffset'><arg direction='in' type='i'/><arg "
    "direction='out' type='b'/></method>"
    "  <method name='SetSelection'><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='in' type='i'/><arg "
    "direction='out' type='b'/></method>"
    "  <method name='AddSelection'><arg direction='in' type='i'/><arg "
    "direction='in' type='i'/><arg direction='out' type='b'/></method>"
    "  <method name='RemoveSelection'><arg direction='in' type='i'/><arg "
    "direction='out' type='b'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.EditableText'>"
    "  <property name='version' type='u' access='read'/>"
    "  <method name='SetTextContents'><arg direction='in' type='s'/><arg "
    "direction='out' type='b'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Action'>"
    "  <property name='version' type='u' access='read'/>"
    "  <property name='NActions' type='i' access='read'/>"
    "  <method name='GetDescription'><arg direction='in' type='i'/><arg "
    "direction='out' type='s'/></method>"
    "  <method name='GetName'><arg direction='in' type='i'/><arg "
    "direction='out' type='s'/></method>"
    "  <method name='GetLocalizedName'><arg direction='in' type='i'/><arg "
    "direction='out' type='s'/></method>"
    "  <method name='GetKeyBinding'><arg direction='in' type='i'/><arg "
    "direction='out' type='s'/></method>"
    "  <method name='GetActions'><arg direction='out' type='a(sss)'/></method>"
    "  <method name='DoAction'><arg direction='in' type='i'/><arg "
    "direction='out' type='b'/></method>"
    " </interface>"
    "</node>";

struct provider_window
{
    struct provider_window *next;
    struct yasl_surface *native_handle;
    ui_accessibility_action_sink_fn action_sink;
    void *action_userdata;
    struct ui_accessibility_tree snapshot;
    uint32_t scale_120;
    uint64_t focused_id;
    unsigned active_sinks;
    /* The menu popup depth plus one; zero for a toplevel. */
    int popup_level;
    bool detaching;
};

struct registered_node
{
    uint64_t id;
    guint accessible_id;
    guint component_id;
    guint action_id;
    guint text_id;
    guint editable_text_id;
};

struct child_change
{
    uint64_t parent_id;
    uint64_t child_id;
    gint32 index;
    bool added;
};

enum node_event_kind
{
    NODE_EVENT_STATE_CHANGED,
    NODE_EVENT_PROPERTY_CHANGED,
    NODE_EVENT_BOUNDS_CHANGED,
    NODE_EVENT_TEXT_CHANGED,
    NODE_EVENT_TEXT_CARET_MOVED,
    NODE_EVENT_TEXT_SELECTION_CHANGED,
    NODE_EVENT_SELECTION_CHANGED,
};

struct node_event
{
    uint64_t node_id;
    enum node_event_kind kind;
    const char *detail;
    gint32 detail1;
    gint32 detail2;
    bool enabled;
    bool uint32_value;
    bool bounds_value;
    guint32 value_uint32;
    struct rect2i bounds;
    char *text;
};

struct provider_server
{
    /* A pthread lock, not a GMutex: ThreadSanitizer cannot see GLib's
     * futex locks, and the AT-SPI thread shares this state. */
    pthread_mutex_t lock;
    pthread_cond_t idle;
    GThread *thread;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *connection;
    GDBusNodeInfo *interfaces;
    GPtrArray *registered;
    struct provider_window *windows;
    int registry_id;
    bool stopping;
};

static struct provider_server server = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
};

static void atspi_method_call(GDBusConnection *connection, const gchar *sender,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data);
static GVariant *atspi_get_property(GDBusConnection *connection,
                                    const gchar *sender,
                                    const gchar *object_path,
                                    const gchar *interface_name,
                                    const gchar *property_name, GError **error,
                                    gpointer user_data);
static void schedule_source(GMainContext *context, GSourceFunc callback,
                            gpointer data, GDestroyNotify destroy);

static const GDBusInterfaceVTable atspi_vtable = {
    .method_call = atspi_method_call,
    .get_property = atspi_get_property,
};

static const char *locale_name(void)
{
    const char *const *names = g_get_language_names();
    return names && names[0] ? names[0] : "C";
}

static char *node_path(uint64_t id)
{
    return g_strdup_printf(
        "/org/a11y/atspi/accessible/n%016" G_GINT64_MODIFIER "x", (guint64)id);
}

static bool path_node_id(const char *path, uint64_t *id)
{
    static const char prefix[] = "/org/a11y/atspi/accessible/n";
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return false;
    const char *digits = path + sizeof(prefix) - 1;
    if (!*digits)
        return false;
    char *end = NULL;
    guint64 parsed = g_ascii_strtoull(digits, &end, 16);
    if (!end || *end || !parsed)
        return false;
    *id = parsed;
    return true;
}

static const struct ui_accessibility_node *
find_node_locked(uint64_t id, struct provider_window **owner)
{
    for (struct provider_window *w = server.windows; w; w = w->next)
    {
        const struct ui_accessibility_node *node =
            ui_accessibility_tree_find(&w->snapshot, id);
        if (node)
        {
            if (owner)
                *owner = w;
            return node;
        }
    }
    if (owner)
        *owner = NULL;
    return NULL;
}

static bool node_action_enabled(const struct provider_window *owner,
                                const struct ui_accessibility_node *node,
                                enum ui_accessibility_action action)
{
    return owner && owner->action_sink && node &&
           (node->actions & (uint32_t)action);
}

static size_t child_count_locked(uint64_t parent_id)
{
    size_t count = 0;
    for (struct provider_window *w = server.windows; w; w = w->next)
    {
        for (size_t i = 0; i < w->snapshot.count; i++)
        {
            if (w->snapshot.nodes[i].parent_id == parent_id)
                count++;
        }
    }
    return count;
}

static const struct ui_accessibility_node *child_at_locked(uint64_t parent_id,
                                                           size_t index)
{
    /* Tree snapshots already add siblings in order. App-level children are
     * grouped by attachment order, then each window's root order. */
    if (!parent_id)
    {
        for (struct provider_window *w = server.windows; w; w = w->next)
        {
            for (size_t i = 0; i < w->snapshot.count; i++)
            {
                const struct ui_accessibility_node *node =
                    &w->snapshot.nodes[i];
                if (node->parent_id == 0 && index-- == 0)
                    return node;
            }
        }
        return NULL;
    }
    struct provider_window *owner = NULL;
    if (!find_node_locked(parent_id, &owner) || !owner)
        return NULL;
    for (size_t i = 0; i < owner->snapshot.count; i++)
    {
        const struct ui_accessibility_node *node = &owner->snapshot.nodes[i];
        if (node->parent_id == parent_id && index-- == 0)
            return node;
    }
    return NULL;
}

static int index_in_parent_locked(uint64_t id)
{
    struct provider_window *owner = NULL;
    const struct ui_accessibility_node *node = find_node_locked(id, &owner);
    if (!node || !owner)
        return -1;
    if (!node->parent_id)
    {
        size_t index = 0;
        for (struct provider_window *w = server.windows; w; w = w->next)
        {
            for (size_t i = 0; i < w->snapshot.count; i++)
            {
                const struct ui_accessibility_node *sibling =
                    &w->snapshot.nodes[i];
                if (sibling->parent_id)
                    continue;
                if (sibling->id == id)
                    return index > INT_MAX ? -1 : (int)index;
                index++;
            }
        }
        return -1;
    }
    size_t index = 0;
    for (size_t i = 0; i < owner->snapshot.count; i++)
    {
        const struct ui_accessibility_node *sibling = &owner->snapshot.nodes[i];
        if (sibling->parent_id != node->parent_id)
            continue;
        if (sibling->id == id)
            return index > INT_MAX ? -1 : (int)index;
        index++;
    }
    return -1;
}

static uint32_t atspi_role(enum ui_accessibility_role role, uint32_t states)
{
    /* Values follow AtspiRole's stable D-Bus enum. Keep this local so GIO is
     * the only Linux provider dependency; libatspi is a client library. */
    enum
    {
        ROLE_DIALOG = 16,
        ROLE_FRAME = 23,
        ROLE_IMAGE = 27,
        ROLE_LABEL = 29,
        ROLE_LIST = 31,
        ROLE_LIST_ITEM = 32,
        ROLE_MENU = 33,
        ROLE_MENU_ITEM = 35,
        ROLE_PAGE_TAB = 37,
        ROLE_PAGE_TAB_LIST = 38,
        ROLE_PANEL = 39,
        ROLE_PASSWORD_TEXT = 40,
        ROLE_PROGRESS_BAR = 42,
        ROLE_BUTTON = 43,
        ROLE_RADIO_BUTTON = 44,
        ROLE_SCROLL_BAR = 48,
        ROLE_SLIDER = 51,
        ROLE_TERMINAL = 60,
        ROLE_TOGGLE_BUTTON = 62,
        ROLE_ENTRY = 79,
        ROLE_HEADING = 83,
        ROLE_COMBO_BOX = 11,
        ROLE_LINK = 88,
        ROLE_WINDOW = 69,
        ROLE_GROUPING = 99,
        ROLE_UNKNOWN = 67,
    };
    if (states & UI_ACCESSIBILITY_STATE_PASSWORD)
        return ROLE_PASSWORD_TEXT;
    switch (role)
    {
    case UI_ACCESSIBILITY_ROLE_WINDOW:
        return ROLE_WINDOW;
    case UI_ACCESSIBILITY_ROLE_GROUP:
        return ROLE_GROUPING;
    case UI_ACCESSIBILITY_ROLE_DIALOG:
        return ROLE_DIALOG;
    case UI_ACCESSIBILITY_ROLE_LABEL:
        return ROLE_LABEL;
    case UI_ACCESSIBILITY_ROLE_HEADING:
        return ROLE_HEADING;
    case UI_ACCESSIBILITY_ROLE_BUTTON:
        return ROLE_BUTTON;
    case UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON:
        return ROLE_TOGGLE_BUTTON;
    case UI_ACCESSIBILITY_ROLE_CHECKBOX:
        return 7;
    case UI_ACCESSIBILITY_ROLE_RADIO_BUTTON:
        return ROLE_RADIO_BUTTON;
    case UI_ACCESSIBILITY_ROLE_TAB_LIST:
        return ROLE_PAGE_TAB_LIST;
    case UI_ACCESSIBILITY_ROLE_TAB:
        return ROLE_PAGE_TAB;
    case UI_ACCESSIBILITY_ROLE_TEXT_FIELD:
    case UI_ACCESSIBILITY_ROLE_SEARCH_FIELD:
        return ROLE_ENTRY;
    case UI_ACCESSIBILITY_ROLE_LINK:
        return ROLE_LINK;
    case UI_ACCESSIBILITY_ROLE_LIST:
        return ROLE_LIST;
    case UI_ACCESSIBILITY_ROLE_LIST_ITEM:
        return ROLE_LIST_ITEM;
    case UI_ACCESSIBILITY_ROLE_MENU:
        return ROLE_MENU;
    case UI_ACCESSIBILITY_ROLE_MENU_ITEM:
        return ROLE_MENU_ITEM;
    case UI_ACCESSIBILITY_ROLE_SLIDER:
        return ROLE_SLIDER;
    case UI_ACCESSIBILITY_ROLE_PROGRESS_INDICATOR:
        return ROLE_PROGRESS_BAR;
    case UI_ACCESSIBILITY_ROLE_SCROLL_BAR:
        return ROLE_SCROLL_BAR;
    case UI_ACCESSIBILITY_ROLE_IMAGE:
        return ROLE_IMAGE;
    case UI_ACCESSIBILITY_ROLE_TERMINAL:
        return ROLE_TERMINAL;
    case UI_ACCESSIBILITY_ROLE_COMBO_BOX:
        return ROLE_COMBO_BOX;
    case UI_ACCESSIBILITY_ROLE_UNKNOWN:
        return ROLE_UNKNOWN;
    }
    return ROLE_UNKNOWN;
}

static const char *role_name(uint32_t role)
{
    switch (role)
    {
    case 7:
        return "check box";
    case 11:
        return "combo box";
    case 16:
        return "dialog";
    case 23:
        return "frame";
    case 27:
        return "image";
    case 29:
        return "label";
    case 31:
        return "list";
    case 32:
        return "list item";
    case 33:
        return "menu";
    case 35:
        return "menu item";
    case 37:
        return "page tab";
    case 38:
        return "page tab list";
    case 39:
        return "panel";
    case 40:
        return "password text";
    case 42:
        return "progress bar";
    case 43:
        return "push button";
    case 44:
        return "radio button";
    case 48:
        return "scroll bar";
    case 51:
        return "slider";
    case 60:
        return "terminal";
    case 62:
        return "toggle button";
    case 67:
        return "unknown";
    case 69:
        return "window";
    case 75:
        return "application";
    case 79:
        return "entry";
    case 83:
        return "heading";
    case 88:
        return "link";
    case 99:
        return "grouping";
    default:
        return "unknown";
    }
}

static void state_add(guint32 words[2], unsigned state)
{
    words[state / 32] |= UINT32_C(1) << (state % 32);
}

static GVariant *state_set(uint32_t states, bool is_app,
                           bool has_selectable_text)
{
    /* AT-SPI packs enum states into 32-bit words. */
    guint32 words[2] = {0};
    state_add(words, 8);  /* ENABLED */
    state_add(words, 24); /* SENSITIVE */
    if (!(states & UI_ACCESSIBILITY_STATE_HIDDEN))
    {
        state_add(words, 25); /* SHOWING */
        state_add(words, 30); /* VISIBLE */
    }
    if (is_app)
        state_add(words, 1); /* ACTIVE */
    if (states & UI_ACCESSIBILITY_STATE_BUSY)
        state_add(words, 3);
    if (states & UI_ACCESSIBILITY_STATE_CHECKED)
        state_add(words, 4);
    if (states & UI_ACCESSIBILITY_STATE_EXPANDED)
        state_add(words, 10);
    if (states & UI_ACCESSIBILITY_STATE_FOCUSABLE)
        state_add(words, 11);
    if (states & UI_ACCESSIBILITY_STATE_FOCUSED)
        state_add(words, 12);
    if (states & UI_ACCESSIBILITY_STATE_EDITABLE)
        state_add(words, 7);
    if (states & UI_ACCESSIBILITY_STATE_MULTILINE)
        state_add(words, 17);
    if (states & UI_ACCESSIBILITY_STATE_SELECTED)
        state_add(words, 23);
    if (states & UI_ACCESSIBILITY_STATE_REQUIRED)
        state_add(words, 33);
    if (states & UI_ACCESSIBILITY_STATE_INVALID)
        state_add(words, 36);
    if (has_selectable_text)
        state_add(words, 29); /* SELECTABLE_TEXT */
    if (states & UI_ACCESSIBILITY_STATE_READ_ONLY)
        state_add(words, 43);
    if (states & UI_ACCESSIBILITY_STATE_DISABLED)
    {
        words[8 / 32] &= ~(UINT32_C(1) << (8 % 32));
        words[24 / 32] &= ~(UINT32_C(1) << (24 % 32));
    }
    return g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, words,
                                     G_N_ELEMENTS(words), sizeof(words[0]));
}

#ifndef YASL_SCALE_UNIT
#define YASL_SCALE_UNIT 120
#endif

static uint32_t surface_scale_120(const struct yasl_surface *s)
{
    if (!s)
        return YASL_SCALE_UNIT;
    double scale = yasl_surface_get_info(s).scale;
    return (uint32_t)(scale * (double)YASL_SCALE_UNIT + 0.5);
}

static struct rect2i rect_to_logical(const struct yasl_surface *s,
                                    struct rect2i device)
{
    uint32_t scale = surface_scale_120(s);
    if (!scale)
        scale = YASL_SCALE_UNIT;
    return (struct rect2i){
        .pos = {
            .x = (int)(((int64_t)device.x * YASL_SCALE_UNIT) / scale),
            .y = (int)(((int64_t)device.y * YASL_SCALE_UNIT) / scale),
        },
        .size = {
            .x = (int)(((int64_t)device.width * YASL_SCALE_UNIT) / scale),
            .y = (int)(((int64_t)device.height * YASL_SCALE_UNIT) / scale),
        },
    };
}

static struct rect2i
node_bounds_locked(const struct ui_accessibility_node *node,
                   const struct provider_window *owner)
{
    return rect_to_logical(owner->native_handle, node->bounds);
}

static const struct ui_accessibility_node *
deepest_child_at_point_locked(uint64_t parent_id, int x, int y,
                              const struct provider_window *owner)
{
    size_t count = child_count_locked(parent_id);
    while (count > 0)
    {
        const struct ui_accessibility_node *child =
            child_at_locked(parent_id, --count);
        if (!child || !rect2i_contains(node_bounds_locked(child, owner),
                                       (struct vector2i){.x = x, .y = y}))
            continue;
        const struct ui_accessibility_node *nested =
            deepest_child_at_point_locked(child->id, x, y, owner);
        return nested ? nested : child;
    }
    return NULL;
}

static GVariant *object_ref(const char *bus_name, const char *path)
{
    return g_variant_new("(so)", bus_name ? bus_name : "",
                         path ? path : ATSPI_NULL_PATH);
}

static GVariant *node_ref(const char *bus_name,
                          const struct ui_accessibility_node *node)
{
    if (!node)
        return object_ref("", ATSPI_NULL_PATH);
    char *path = node_path(node->id);
    GVariant *ref = object_ref(bus_name, path);
    g_free(path);
    return ref;
}

static bool
dispatch_action(uint64_t id,
                const struct ui_accessibility_action_request *request)
{
    const uint32_t supported_actions = UI_ACCESSIBILITY_ACTION_INVOKE |
                                       UI_ACCESSIBILITY_ACTION_FOCUS |
                                       UI_ACCESSIBILITY_ACTION_SET_VALUE |
                                       UI_ACCESSIBILITY_ACTION_SET_SELECTION;
    if (!request || !request->action ||
        ((uint32_t)request->action & supported_actions) == 0 ||
        ((uint32_t)request->action & ((uint32_t)request->action - 1)) != 0)
        return false;

    ui_accessibility_action_sink_fn sink = NULL;
    void *userdata = NULL;
    struct provider_window *owner = NULL;

    pthread_mutex_lock(&server.lock);
    const struct ui_accessibility_node *node = find_node_locked(id, &owner);
    if (node && owner && !owner->detaching && owner->action_sink &&
        (node->actions & (uint32_t)request->action))
    {
        owner->active_sinks++;
        sink = owner->action_sink;
        userdata = owner->action_userdata;
    }
    pthread_mutex_unlock(&server.lock);
    if (!sink)
        return false;

    struct ui_accessibility_action_request action_request = *request;
    action_request.node_id = id;
    sink(userdata, &action_request);

    pthread_mutex_lock(&server.lock);
    owner->active_sinks--;
    if (owner->detaching && !owner->active_sinks)
        pthread_cond_broadcast(&server.idle);
    pthread_mutex_unlock(&server.lock);
    return true;
}

static bool invoke_action(uint64_t id, enum ui_accessibility_action action)
{
    const struct ui_accessibility_action_request request = {.action = action};
    return dispatch_action(id, &request);
}

static const char *node_text(const struct ui_accessibility_node *node)
{
    if (!node || (node->states & UI_ACCESSIBILITY_STATE_PASSWORD) ||
        !node->value || !g_utf8_validate(node->value, -1, NULL))
        return "";
    return node->value;
}

static size_t text_character_count(const char *text)
{
    glong count = g_utf8_strlen(text, -1);
    return count < 0 ? 0 : (size_t)count;
}

static size_t text_char_to_byte(const char *text, gint offset)
{
    size_t count = text_character_count(text);
    size_t clamped = offset < 0 ? 0 : (size_t)offset;
    if (clamped > count)
        clamped = count;
    return (size_t)(g_utf8_offset_to_pointer(text, (glong)clamped) - text);
}

static size_t text_byte_to_char(const char *text, size_t offset)
{
    size_t length = strlen(text);
    if (offset > length)
        offset = length;
    while (offset > 0 && offset < length &&
           (((unsigned char)text[offset] & 0xc0) == 0x80))
        offset--;
    return (size_t)g_utf8_pointer_to_offset(text, text + offset);
}

static size_t text_end_offset(const char *text, gint offset)
{
    return offset < 0 ? text_character_count(text)
                      : text_char_to_byte(text, offset);
}

static bool variant_node_id(const char *path, uint64_t *id)
{
    return path_node_id(path, id);
}

static void return_object_method(GDBusMethodInvocation *invocation,
                                 GVariant *object)
{
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(@(so))", object));
}

static void atspi_method_call(GDBusConnection *connection, const gchar *sender,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data)
{
    (void)connection;
    (void)sender;
    struct provider_server *s = user_data;

    /* Socket.Embed writes Application.Id. Handle Properties.Set here because
     * the set_property callback receives an invalid interface_name. */
    if (interface_name && strcmp(interface_name, DBUS_PROPERTIES_IFACE) == 0 &&
        strcmp(method_name, "Set") == 0)
    {
        const char *property_interface = NULL;
        const char *property_name = NULL;
        GVariant *boxed_value = NULL;
        g_variant_get(parameters, "(&s&s@v)", &property_interface,
                      &property_name, &boxed_value);
        GVariant *value = g_variant_get_variant(boxed_value);
        bool valid = strcmp(object_path, ATSPI_ROOT_PATH) == 0 &&
                     strcmp(property_interface, ATSPI_APPLICATION_IFACE) == 0 &&
                     strcmp(property_name, "Id") == 0 &&
                     g_variant_is_of_type(value, G_VARIANT_TYPE_INT32);
        if (valid)
        {
            pthread_mutex_lock(&s->lock);
            s->registry_id = g_variant_get_int32(value);
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("()"));
        }
        else
        {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.DBus.Error.PropertyReadOnly",
                "The requested AT-SPI property is not writable.");
        }
        g_variant_unref(value);
        g_variant_unref(boxed_value);
        return;
    }

    bool is_root = strcmp(object_path, ATSPI_ROOT_PATH) == 0;
    uint64_t id = 0;
    (void)variant_node_id(object_path, &id);

    if (strcmp(interface_name, ATSPI_APPLICATION_IFACE) == 0)
    {
        if (strcmp(method_name, "GetLocale") == 0)
        {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)", locale_name()));
            return;
        }
        if (strcmp(method_name, "GetApplicationBusAddress") == 0)
        {
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(s)", ""));
            return;
        }
    }

    if (strcmp(interface_name, ATSPI_ACCESSIBLE_IFACE) == 0)
    {
        if (strcmp(method_name, "GetChildAtIndex") == 0)
        {
            gint index = -1;
            g_variant_get(parameters, "(i)", &index);
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *child =
                index < 0 ? NULL
                          : child_at_locked(is_root ? 0 : id, (size_t)index);
            GVariant *ref = node_ref(
                g_dbus_connection_get_unique_name(s->connection), child);
            pthread_mutex_unlock(&s->lock);
            return_object_method(invocation, ref);
            return;
        }
        if (strcmp(method_name, "GetChildren") == 0)
        {
            GVariantBuilder children;
            g_variant_builder_init(&children, G_VARIANT_TYPE("a(so)"));
            pthread_mutex_lock(&s->lock);
            size_t count = child_count_locked(is_root ? 0 : id);
            for (size_t i = 0; i < count; i++)
            {
                const struct ui_accessibility_node *child =
                    child_at_locked(is_root ? 0 : id, i);
                if (child)
                    g_variant_builder_add_value(
                        &children, node_ref(g_dbus_connection_get_unique_name(
                                                s->connection),
                                            child));
            }
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(@a(so))", g_variant_builder_end(&children)));
            return;
        }
        if (strcmp(method_name, "GetIndexInParent") == 0)
        {
            pthread_mutex_lock(&s->lock);
            int index = id ? index_in_parent_locked(id) : -1;
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(i)", index));
            return;
        }
        if (strcmp(method_name, "GetRelationSet") == 0)
        {
            GVariantBuilder relations;
            g_variant_builder_init(&relations, G_VARIANT_TYPE("a(ua(so))"));
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(@a(ua(so)))",
                                          g_variant_builder_end(&relations)));
            return;
        }
        if (strcmp(method_name, "GetRole") == 0 ||
            strcmp(method_name, "GetRoleName") == 0 ||
            strcmp(method_name, "GetLocalizedRoleName") == 0 ||
            strcmp(method_name, "GetState") == 0)
        {
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            uint32_t role = is_root ? 75
                            : node  ? atspi_role(node->role, node->states)
                                    : 67;
            uint32_t states = node ? node->states : 0;
            if (strcmp(method_name, "GetRole") == 0)
            {
                pthread_mutex_unlock(&s->lock);
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(u)", role));
            }
            else if (strcmp(method_name, "GetState") == 0)
            {
                bool selectable_text =
                    node && node->value && owner && owner->action_sink &&
                    (node->actions & UI_ACCESSIBILITY_ACTION_SET_SELECTION);
                GVariant *set = state_set(states, is_root, selectable_text);
                pthread_mutex_unlock(&s->lock);
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(@au)", set));
            }
            else
            {
                const char *name = role_name(role);
                pthread_mutex_unlock(&s->lock);
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(s)", name));
            }
            return;
        }
        if (strcmp(method_name, "GetAttributes") == 0)
        {
            GVariantBuilder attributes;
            g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{ss}"));
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(@a{ss})", g_variant_builder_end(&attributes)));
            return;
        }
        if (strcmp(method_name, "GetApplication") == 0)
        {
            return_object_method(
                invocation,
                object_ref(g_dbus_connection_get_unique_name(s->connection),
                           ATSPI_ROOT_PATH));
            return;
        }
        if (strcmp(method_name, "GetInterfaces") == 0)
        {
            GVariantBuilder interfaces;
            g_variant_builder_init(&interfaces, G_VARIANT_TYPE("as"));
            g_variant_builder_add(&interfaces, "s", ATSPI_ACCESSIBLE_IFACE);
            if (is_root)
            {
                g_variant_builder_add(&interfaces, "s",
                                      ATSPI_APPLICATION_IFACE);
            }
            else
            {
                g_variant_builder_add(&interfaces, "s", ATSPI_COMPONENT_IFACE);
                pthread_mutex_lock(&s->lock);
                struct provider_window *owner = NULL;
                const struct ui_accessibility_node *node =
                    find_node_locked(id, &owner);
                bool has_action = node_action_enabled(
                    owner, node, UI_ACCESSIBILITY_ACTION_INVOKE);
                bool has_text =
                    node &&
                    ((node->states & UI_ACCESSIBILITY_STATE_PASSWORD) ||
                     (node->value && g_utf8_validate(node->value, -1, NULL)));
                bool has_editable_text =
                    has_text &&
                    node_action_enabled(owner, node,
                                        UI_ACCESSIBILITY_ACTION_SET_VALUE);
                pthread_mutex_unlock(&s->lock);
                if (has_action)
                    g_variant_builder_add(&interfaces, "s", ATSPI_ACTION_IFACE);
                if (has_text)
                    g_variant_builder_add(&interfaces, "s", ATSPI_TEXT_IFACE);
                if (has_editable_text)
                    g_variant_builder_add(&interfaces, "s",
                                          ATSPI_EDITABLE_TEXT_IFACE);
            }
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(@as)", g_variant_builder_end(&interfaces)));
            return;
        }
    }

    if (strcmp(interface_name, ATSPI_TEXT_IFACE) == 0)
    {
        if (strcmp(method_name, "GetText") == 0)
        {
            gint start = 0, end = -1;
            g_variant_get(parameters, "(ii)", &start, &end);
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            size_t start_byte = text_char_to_byte(text, start);
            size_t end_byte = text_end_offset(text, end);
            if (end_byte < start_byte)
                end_byte = start_byte;
            char *selected =
                g_strndup(text + start_byte, end_byte - start_byte);
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s)", selected));
            g_free(selected);
            return;
        }
        if (strcmp(method_name, "GetCharacterCount") == 0 ||
            strcmp(method_name, "GetCaretOffset") == 0 ||
            strcmp(method_name, "GetNSelections") == 0)
        {
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            gint32 result = 0;
            if (strcmp(method_name, "GetCharacterCount") == 0)
            {
                size_t count = text_character_count(text);
                result = count > INT_MAX ? INT_MAX : (gint32)count;
            }
            else if (strcmp(method_name, "GetCaretOffset") == 0 && node)
            {
                size_t offset = text_byte_to_char(text, node->caret_offset);
                result = offset > INT_MAX ? INT_MAX : (gint32)offset;
            }
            else if (strcmp(method_name, "GetNSelections") == 0 && node &&
                     node->selection_start != node->selection_end)
            {
                result = 1;
            }
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(i)", result));
            return;
        }
        if (strcmp(method_name, "GetCharacterAtOffset") == 0)
        {
            gint offset = -1;
            g_variant_get(parameters, "(i)", &offset);
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            size_t count = text_character_count(text);
            gint32 character = 0;
            if (offset >= 0 && (size_t)offset < count)
            {
                const char *at = g_utf8_offset_to_pointer(text, offset);
                character = (gint32)g_utf8_get_char(at);
            }
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(i)", character));
            return;
        }
        if (strcmp(method_name, "GetSelection") == 0)
        {
            gint selection = -1;
            g_variant_get(parameters, "(i)", &selection);
            gint32 start = -1, end = -1;
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            if (node && selection == 0 &&
                node->selection_start != node->selection_end)
            {
                size_t start_char =
                    text_byte_to_char(text, node->selection_start);
                size_t end_char = text_byte_to_char(text, node->selection_end);
                start = start_char > INT_MAX ? INT_MAX : (gint32)start_char;
                end = end_char > INT_MAX ? INT_MAX : (gint32)end_char;
            }
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(ii)", start, end));
            return;
        }
        if (strcmp(method_name, "SetCaretOffset") == 0)
        {
            gint offset = -1;
            g_variant_get(parameters, "(i)", &offset);
            struct ui_accessibility_action_request request = {
                .action = UI_ACCESSIBILITY_ACTION_SET_SELECTION,
            };
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            size_t byte_offset = text_char_to_byte(text, offset);
            bool valid = node && offset >= 0 &&
                         (size_t)offset <= text_character_count(text);
            pthread_mutex_unlock(&s->lock);
            request.caret_offset = request.selection_start =
                request.selection_end = byte_offset;
            bool ok = valid && dispatch_action(id, &request);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", ok));
            return;
        }
        if (strcmp(method_name, "SetSelection") == 0 ||
            strcmp(method_name, "AddSelection") == 0)
        {
            gint selection = 0, start = -1, end = -1;
            if (strcmp(method_name, "SetSelection") == 0)
                g_variant_get(parameters, "(iii)", &selection, &start, &end);
            else
                g_variant_get(parameters, "(ii)", &start, &end);
            struct ui_accessibility_action_request request = {
                .action = UI_ACCESSIBILITY_ACTION_SET_SELECTION,
            };
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            size_t count = text_character_count(text);
            bool valid =
                node &&
                (strcmp(method_name, "AddSelection") == 0 || selection == 0) &&
                start >= 0 && end >= start && (size_t)end <= count;
            size_t start_byte = text_char_to_byte(text, start);
            size_t end_byte = text_char_to_byte(text, end);
            pthread_mutex_unlock(&s->lock);
            request.caret_offset = end_byte;
            request.selection_start = start_byte;
            request.selection_end = end_byte;
            bool ok = valid && dispatch_action(id, &request);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", ok));
            return;
        }
        if (strcmp(method_name, "RemoveSelection") == 0)
        {
            gint selection = -1;
            g_variant_get(parameters, "(i)", &selection);
            struct ui_accessibility_action_request request = {
                .action = UI_ACCESSIBILITY_ACTION_SET_SELECTION,
            };
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            bool valid = node && selection == 0 &&
                         node->selection_start != node->selection_end;
            size_t byte_offset =
                node ? text_char_to_byte(text, (gint)text_byte_to_char(
                                                   text, node->selection_start))
                     : 0;
            pthread_mutex_unlock(&s->lock);
            request.caret_offset = request.selection_start =
                request.selection_end = byte_offset;
            bool ok = valid && dispatch_action(id, &request);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", ok));
            return;
        }
    }

    if (strcmp(interface_name, ATSPI_EDITABLE_TEXT_IFACE) == 0 &&
        strcmp(method_name, "SetTextContents") == 0)
    {
        const char *value = NULL;
        g_variant_get(parameters, "(&s)", &value);
        if (!g_utf8_validate(value, -1, NULL))
        {
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", FALSE));
            return;
        }
        size_t end = strlen(value);
        const struct ui_accessibility_action_request request = {
            .action = UI_ACCESSIBILITY_ACTION_SET_VALUE,
            .value = value,
            .caret_offset = end,
            .selection_start = end,
            .selection_end = end,
        };
        bool ok = dispatch_action(id, &request);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(b)", ok));
        return;
    }

    if (strcmp(interface_name, ATSPI_ACTION_IFACE) == 0)
    {
        if (strcmp(method_name, "GetDescription") == 0 ||
            strcmp(method_name, "GetName") == 0 ||
            strcmp(method_name, "GetLocalizedName") == 0 ||
            strcmp(method_name, "GetKeyBinding") == 0)
        {
            gint index = -1;
            g_variant_get(parameters, "(i)", &index);
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            bool supported =
                index == 0 && node_action_enabled(
                                  owner, node, UI_ACCESSIBILITY_ACTION_INVOKE);
            pthread_mutex_unlock(&s->lock);
            const char *result =
                supported && (strcmp(method_name, "GetName") == 0 ||
                              strcmp(method_name, "GetLocalizedName") == 0)
                    ? "click"
                    : "";
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(s)", result));
            return;
        }
        if (strcmp(method_name, "GetActions") == 0)
        {
            GVariantBuilder actions;
            g_variant_builder_init(&actions, G_VARIANT_TYPE("a(sss)"));
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            bool supported = node_action_enabled(
                owner, node, UI_ACCESSIBILITY_ACTION_INVOKE);
            pthread_mutex_unlock(&s->lock);
            if (supported)
                g_variant_builder_add(&actions, "(sss)", "click",
                                      "Activate this control", "");
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(@a(sss))", g_variant_builder_end(&actions)));
            return;
        }
        if (strcmp(method_name, "DoAction") == 0)
        {
            gint index = -1;
            g_variant_get(parameters, "(i)", &index);
            bool ok =
                index == 0 && invoke_action(id, UI_ACCESSIBILITY_ACTION_INVOKE);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", ok));
            return;
        }
    }

    if (strcmp(interface_name, ATSPI_COMPONENT_IFACE) == 0)
    {
        if (strcmp(method_name, "GetExtents") == 0 ||
            strcmp(method_name, "GetPosition") == 0 ||
            strcmp(method_name, "GetSize") == 0)
        {
            guint coord = 1;
            if (strcmp(method_name, "GetSize") != 0)
                g_variant_get(parameters, "(u)", &coord);
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            struct rect2i bounds = node && owner
                                       ? node_bounds_locked(node, owner)
                                       : (struct rect2i){0};
            if (coord == 0)
            {
                /* Wayland does not expose a toplevel's global screen origin. */
                bounds.pos = (struct vector2i){0};
            }
            else if (coord == 2 && node && node->parent_id)
            {
                const struct ui_accessibility_node *parent =
                    find_node_locked(node->parent_id, NULL);
                if (parent)
                {
                    struct rect2i parent_bounds =
                        node_bounds_locked(parent, owner);
                    bounds.x -= parent_bounds.x;
                    bounds.y -= parent_bounds.y;
                }
            }
            pthread_mutex_unlock(&s->lock);
            if (strcmp(method_name, "GetExtents") == 0)
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("((iiii))", bounds.x, bounds.y,
                                              bounds.width, bounds.height));
            else
                g_dbus_method_invocation_return_value(
                    invocation,
                    g_variant_new(
                        "(ii)",
                        strcmp(method_name, "GetSize") == 0 ? bounds.width
                                                            : bounds.x,
                        strcmp(method_name, "GetSize") == 0 ? bounds.height
                                                            : bounds.y));
            return;
        }
        if (strcmp(method_name, "Contains") == 0 ||
            strcmp(method_name, "GetAccessibleAtPoint") == 0)
        {
            gint x = 0, y = 0;
            guint coord = 1;
            g_variant_get(parameters, "(iiu)", &x, &y, &coord);
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            const struct ui_accessibility_node *hit = NULL;
            if (node && owner && coord != 0)
            {
                struct rect2i origin = node_bounds_locked(node, owner);
                if (coord == 2 && node->parent_id)
                {
                    const struct ui_accessibility_node *parent =
                        find_node_locked(node->parent_id, NULL);
                    if (parent)
                    {
                        struct rect2i pb = node_bounds_locked(parent, owner);
                        x += pb.x;
                        y += pb.y;
                    }
                }
                if (rect2i_contains(origin, (struct vector2i){.x = x, .y = y}))
                    hit = node;
                if (hit && strcmp(method_name, "GetAccessibleAtPoint") == 0)
                {
                    const struct ui_accessibility_node *deepest =
                        deepest_child_at_point_locked(id, x, y, owner);
                    if (deepest)
                        hit = deepest;
                }
            }
            bool contains = hit != NULL;
            GVariant *ref =
                node_ref(g_dbus_connection_get_unique_name(s->connection), hit);
            pthread_mutex_unlock(&s->lock);
            if (strcmp(method_name, "Contains") == 0)
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(b)", contains));
            else
                return_object_method(invocation, ref);
            return;
        }
        if (strcmp(method_name, "GetLayer") == 0)
        {
            guint layer = 0;
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            if (node && owner)
            {
                if (owner->popup_level)
                    layer = 5; /* POPUP */
                else if (node->role == UI_ACCESSIBILITY_ROLE_WINDOW)
                    layer = 7; /* WINDOW */
                else
                    layer = 3; /* WIDGET */
            }
            pthread_mutex_unlock(&s->lock);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(u)", layer));
            return;
        }
        if (strcmp(method_name, "GetMDIZOrder") == 0)
        {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(n)", (gint16)0));
            return;
        }
        if (strcmp(method_name, "GetAlpha") == 0)
        {
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(d)", 1.0));
            return;
        }
        if (strcmp(method_name, "GrabFocus") == 0)
        {
            bool ok = invoke_action(id, UI_ACCESSIBILITY_ACTION_FOCUS);
            g_dbus_method_invocation_return_value(invocation,
                                                  g_variant_new("(b)", ok));
            return;
        }
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.a11y.atspi.Error.NotImplemented",
        "The requested AT-SPI method is not supported.");
}

static GVariant *atspi_get_property(GDBusConnection *connection,
                                    const gchar *sender,
                                    const gchar *object_path,
                                    const gchar *interface_name,
                                    const gchar *property_name, GError **error,
                                    gpointer user_data)
{
    (void)connection;
    (void)sender;
    struct provider_server *s = user_data;
    bool is_root = strcmp(object_path, ATSPI_ROOT_PATH) == 0;
    uint64_t id = 0;
    (void)path_node_id(object_path, &id);

    if (strcmp(interface_name, ATSPI_APPLICATION_IFACE) == 0 && is_root)
    {
        if (strcmp(property_name, "ToolkitName") == 0)
            return g_variant_new_string("wsap_term");
        if (strcmp(property_name, "ToolkitVersion") == 0)
            return g_variant_new_string("0.1.0");
        if (strcmp(property_name, "AtspiVersion") == 0)
            return g_variant_new_string("2.1");
        if (strcmp(property_name, "InterfaceVersion") == 0)
            return g_variant_new_uint32(1);
        if (strcmp(property_name, "Id") == 0)
        {
            pthread_mutex_lock(&s->lock);
            int id_value = s->registry_id;
            pthread_mutex_unlock(&s->lock);
            return g_variant_new_int32(id_value);
        }
    }
    if (strcmp(interface_name, ATSPI_ACCESSIBLE_IFACE) == 0)
    {
        if (strcmp(property_name, "version") == 0)
            return g_variant_new_uint32(1);
        if (strcmp(property_name, "Locale") == 0)
            return g_variant_new_string(locale_name());
        if (strcmp(property_name, "HelpText") == 0)
            return g_variant_new_string("");
        if (strcmp(property_name, "AccessibleId") == 0)
        {
            if (is_root)
                return g_variant_new_string("wsap_term");
            return g_variant_new_take_string(
                g_strdup_printf("%016" G_GINT64_MODIFIER "x", (guint64)id));
        }
        if (strcmp(property_name, "Name") == 0 ||
            strcmp(property_name, "Description") == 0)
        {
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *value = "";
            if (is_root && strcmp(property_name, "Name") == 0)
            {
                value = g_get_prgname();
                if (!value || !*value)
                    value = "wsap_term";
            }
            else if (node)
            {
                value = strcmp(property_name, "Name") == 0
                            ? (node->name ? node->name : "")
                            : (node->description ? node->description : "");
            }
            GVariant *result = g_variant_new_string(value);
            pthread_mutex_unlock(&s->lock);
            return result;
        }
        if (strcmp(property_name, "Parent") == 0)
        {
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            GVariant *parent;
            if (!node)
                parent = object_ref("", ATSPI_NULL_PATH);
            else if (!node->parent_id)
                parent =
                    object_ref(g_dbus_connection_get_unique_name(s->connection),
                               ATSPI_ROOT_PATH);
            else
            {
                char *path = node_path(node->parent_id);
                parent = object_ref(
                    g_dbus_connection_get_unique_name(s->connection), path);
                g_free(path);
            }
            pthread_mutex_unlock(&s->lock);
            return parent;
        }
        if (strcmp(property_name, "ChildCount") == 0)
        {
            pthread_mutex_lock(&s->lock);
            size_t count = child_count_locked(is_root ? 0 : id);
            pthread_mutex_unlock(&s->lock);
            return g_variant_new_int32(count > INT_MAX ? INT_MAX
                                                       : (gint32)count);
        }
    }
    if (strcmp(interface_name, ATSPI_COMPONENT_IFACE) == 0 &&
        strcmp(property_name, "version") == 0)
        return g_variant_new_uint32(1);
    if ((strcmp(interface_name, ATSPI_TEXT_IFACE) == 0 ||
         strcmp(interface_name, ATSPI_EDITABLE_TEXT_IFACE) == 0) &&
        strcmp(property_name, "version") == 0)
        return g_variant_new_uint32(1);
    if (strcmp(interface_name, ATSPI_TEXT_IFACE) == 0)
    {
        if (strcmp(property_name, "CharacterCount") == 0 ||
            strcmp(property_name, "CaretOffset") == 0)
        {
            pthread_mutex_lock(&s->lock);
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, NULL) : NULL;
            const char *text = node_text(node);
            size_t count = strcmp(property_name, "CharacterCount") == 0
                               ? text_character_count(text)
                           : node ? text_byte_to_char(text, node->caret_offset)
                                  : 0;
            pthread_mutex_unlock(&s->lock);
            return g_variant_new_int32(count > INT_MAX ? INT_MAX
                                                       : (gint32)count);
        }
    }
    if (strcmp(interface_name, ATSPI_ACTION_IFACE) == 0)
    {
        if (strcmp(property_name, "version") == 0)
            return g_variant_new_uint32(1);
        if (strcmp(property_name, "NActions") == 0)
        {
            pthread_mutex_lock(&s->lock);
            struct provider_window *owner = NULL;
            const struct ui_accessibility_node *node =
                id ? find_node_locked(id, &owner) : NULL;
            gint32 count =
                node_action_enabled(owner, node, UI_ACCESSIBILITY_ACTION_INVOKE)
                    ? 1
                    : 0;
            pthread_mutex_unlock(&s->lock);
            return g_variant_new_int32(count);
        }
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "Unknown AT-SPI property %s", property_name);
    return NULL;
}

static gboolean emit_focus_event(gpointer data)
{
    guint64 id = *(guint64 *)data;
    pthread_mutex_lock(&server.lock);
    if (!server.connection || server.stopping || !find_node_locked(id, NULL))
    {
        pthread_mutex_unlock(&server.lock);
        return G_SOURCE_REMOVE;
    }
    char *path = node_path(id);
    GDBusConnection *connection = g_object_ref(server.connection);
    pthread_mutex_unlock(&server.lock);

    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_emit_signal(
        connection, NULL, path, ATSPI_EVENT_FOCUS_IFACE, "Focus",
        g_variant_new("(siiv@a{sv})", "", 0, 0, g_variant_new_string("0"),
                      g_variant_builder_end(&properties)),
        NULL);
    g_object_unref(connection);
    g_free(path);
    return G_SOURCE_REMOVE;
}

static gboolean emit_child_change_event(gpointer data)
{
    const struct child_change *change = data;
    pthread_mutex_lock(&server.lock);
    if (!server.connection)
    {
        pthread_mutex_unlock(&server.lock);
        return G_SOURCE_REMOVE;
    }
    GDBusConnection *connection = g_object_ref(server.connection);
    const char *bus_name = g_dbus_connection_get_unique_name(connection);
    pthread_mutex_unlock(&server.lock);

    char *parent_path = change->parent_id ? node_path(change->parent_id)
                                          : g_strdup(ATSPI_ROOT_PATH);
    char *child_path = node_path(change->child_id);
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_emit_signal(
        connection, NULL, parent_path, ATSPI_EVENT_OBJECT_IFACE,
        "ChildrenChanged",
        g_variant_new("(siiv@a{sv})", change->added ? "add" : "remove",
                      change->index, 0,
                      g_variant_new("(so)", bus_name, child_path),
                      g_variant_builder_end(&properties)),
        NULL);
    g_free(child_path);
    g_free(parent_path);
    g_object_unref(connection);
    return G_SOURCE_REMOVE;
}

static void schedule_child_changes(GMainContext *context, GArray *changes)
{
    if (!context || !changes)
        return;
    for (guint i = 0; i < changes->len; i++)
    {
        struct child_change *copy = g_new(struct child_change, 1);
        *copy = g_array_index(changes, struct child_change, i);
        schedule_source(context, emit_child_change_event, copy, g_free);
    }
}

static void node_event_destroy(gpointer data)
{
    struct node_event *event = data;
    g_free(event->text);
    g_free(event);
}

static gboolean emit_node_event(gpointer data)
{
    const struct node_event *event = data;
    pthread_mutex_lock(&server.lock);
    if (!server.connection ||
        (event->node_id && !find_node_locked(event->node_id, NULL)))
    {
        pthread_mutex_unlock(&server.lock);
        return G_SOURCE_REMOVE;
    }
    GDBusConnection *connection = g_object_ref(server.connection);
    pthread_mutex_unlock(&server.lock);

    const char *member = NULL;
    const char *detail = event->detail ? event->detail : "";
    switch (event->kind)
    {
    case NODE_EVENT_STATE_CHANGED:
        member = "StateChanged";
        break;
    case NODE_EVENT_PROPERTY_CHANGED:
        member = "PropertyChange";
        break;
    case NODE_EVENT_BOUNDS_CHANGED:
        member = "BoundsChanged";
        break;
    case NODE_EVENT_TEXT_CHANGED:
        member = "TextChanged";
        break;
    case NODE_EVENT_TEXT_CARET_MOVED:
        member = "TextCaretMoved";
        break;
    case NODE_EVENT_TEXT_SELECTION_CHANGED:
        member = "TextSelectionChanged";
        break;
    case NODE_EVENT_SELECTION_CHANGED:
        member = "SelectionChanged";
        break;
    }
    char *path =
        event->node_id ? node_path(event->node_id) : g_strdup(ATSPI_ROOT_PATH);
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    GVariant *value =
        event->bounds_value
            ? g_variant_new("(iiii)", event->bounds.x, event->bounds.y,
                            event->bounds.width, event->bounds.height)
        : event->uint32_value
            ? g_variant_new_uint32(event->value_uint32)
            : g_variant_new_string(event->text ? event->text : "");
    g_dbus_connection_emit_signal(
        connection, NULL, path, ATSPI_EVENT_OBJECT_IFACE, member,
        g_variant_new("(siiv@a{sv})", detail, event->detail1, event->detail2,
                      value, g_variant_builder_end(&properties)),
        NULL);
    g_free(path);
    g_object_unref(connection);
    return G_SOURCE_REMOVE;
}

static void append_node_event(GArray *events, uint64_t node_id,
                              enum node_event_kind kind, const char *detail,
                              gint32 detail1, gint32 detail2, bool enabled,
                              const char *text)
{
    struct node_event event = {
        .node_id = node_id,
        .kind = kind,
        .detail = detail,
        .detail1 = detail1,
        .detail2 = detail2,
        .enabled = enabled,
        .text = g_strdup(text ? text : ""),
    };
    g_array_append_val(events, event);
}

static void append_property_change(GArray *events, uint64_t node_id,
                                   const char *property, const char *value)
{
    append_node_event(events, node_id, NODE_EVENT_PROPERTY_CHANGED, property, 0,
                      0, false, value ? value : "");
}

static void append_bounds_change(GArray *events, uint64_t node_id,
                                 struct rect2i bounds)
{
    struct node_event event = {
        .node_id = node_id,
        .kind = NODE_EVENT_BOUNDS_CHANGED,
        .bounds_value = true,
        .bounds = bounds,
        .text = g_strdup(""),
    };
    g_array_append_val(events, event);
}

static void append_role_change(GArray *events,
                               const struct ui_accessibility_node *node)
{
    struct node_event event = {
        .node_id = node->id,
        .kind = NODE_EVENT_PROPERTY_CHANGED,
        .detail = "accessible-role",
        .uint32_value = true,
        .value_uint32 = atspi_role(node->role, node->states),
        .text = g_strdup(""),
    };
    g_array_append_val(events, event);
}

static void schedule_node_events(GMainContext *context, GArray *events)
{
    if (!context || !events)
        return;
    for (guint i = 0; i < events->len; i++)
    {
        const struct node_event *source =
            &g_array_index(events, struct node_event, i);
        struct node_event *copy = g_new(struct node_event, 1);
        *copy = *source;
        copy->text = g_strdup(source->text);
        schedule_source(context, emit_node_event, copy, node_event_destroy);
    }
}

static void append_state_change(GArray *events, uint64_t node_id,
                                const char *state_name, bool enabled)
{
    append_node_event(events, node_id, NODE_EVENT_STATE_CHANGED, state_name,
                      enabled ? 1 : 0, 0, enabled, "");
}

static void append_state_changes(GArray *events,
                                 const struct ui_accessibility_node *old_node,
                                 const struct ui_accessibility_node *new_node)
{
    uint32_t changed = old_node->states ^ new_node->states;
    const struct
    {
        uint32_t bit;
        const char *name;
    } simple_states[] = {
        {UI_ACCESSIBILITY_STATE_FOCUSABLE, "focusable"},
        {UI_ACCESSIBILITY_STATE_FOCUSED, "focused"},
        {UI_ACCESSIBILITY_STATE_SELECTED, "selected"},
        {UI_ACCESSIBILITY_STATE_EXPANDED, "expanded"},
        {UI_ACCESSIBILITY_STATE_CHECKED, "checked"},
        {UI_ACCESSIBILITY_STATE_EDITABLE, "editable"},
        {UI_ACCESSIBILITY_STATE_READ_ONLY, "read-only"},
        {UI_ACCESSIBILITY_STATE_MULTILINE, "multi-line"},
        {UI_ACCESSIBILITY_STATE_BUSY, "busy"},
        {UI_ACCESSIBILITY_STATE_REQUIRED, "required"},
        {UI_ACCESSIBILITY_STATE_INVALID, "invalid-entry"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(simple_states); i++)
    {
        uint32_t bit = simple_states[i].bit;
        if (changed & bit)
            append_state_change(events, new_node->id, simple_states[i].name,
                                (new_node->states & bit) != 0);
    }
    if (changed & UI_ACCESSIBILITY_STATE_DISABLED)
    {
        bool enabled = !(new_node->states & UI_ACCESSIBILITY_STATE_DISABLED);
        append_state_change(events, new_node->id, "enabled", enabled);
        append_state_change(events, new_node->id, "sensitive", enabled);
    }
    if (changed & UI_ACCESSIBILITY_STATE_HIDDEN)
    {
        bool visible = !(new_node->states & UI_ACCESSIBILITY_STATE_HIDDEN);
        append_state_change(events, new_node->id, "showing", visible);
        append_state_change(events, new_node->id, "visible", visible);
    }
    if (changed & UI_ACCESSIBILITY_STATE_SELECTED)
        append_node_event(events, new_node->parent_id,
                          NODE_EVENT_SELECTION_CHANGED, "", 0, 0, false, "");
}

static void
append_property_changes(GArray *events,
                        const struct ui_accessibility_node *old_node,
                        const struct ui_accessibility_node *new_node)
{
    if (g_strcmp0(old_node->name, new_node->name) != 0)
        append_property_change(events, new_node->id, "accessible-name",
                               new_node->name);
    if (g_strcmp0(old_node->description, new_node->description) != 0)
        append_property_change(events, new_node->id, "accessible-description",
                               new_node->description);
    if (atspi_role(old_node->role, old_node->states) !=
        atspi_role(new_node->role, new_node->states))
        append_role_change(events, new_node);
}

static gint32 offset_as_int(size_t offset)
{
    return offset > INT_MAX ? INT_MAX : (gint32)offset;
}

static void append_text_changes(GArray *events,
                                const struct ui_accessibility_node *old_node,
                                const struct ui_accessibility_node *new_node)
{
    if (!old_node->value || !new_node->value ||
        !g_utf8_validate(old_node->value, -1, NULL) ||
        !g_utf8_validate(new_node->value, -1, NULL))
        return;
    const char *old_text = old_node->value;
    const char *new_text = new_node->value;
    if (strcmp(old_text, new_text) != 0)
    {
        size_t old_length = strlen(old_text);
        size_t new_length = strlen(new_text);
        size_t prefix = 0;
        while (prefix < old_length && prefix < new_length &&
               old_text[prefix] == new_text[prefix])
            prefix++;
        while (prefix > 0 &&
               (((unsigned char)old_text[prefix] & 0xc0) == 0x80 ||
                ((unsigned char)new_text[prefix] & 0xc0) == 0x80))
            prefix--;

        size_t suffix = 0;
        while (suffix < old_length - prefix && suffix < new_length - prefix &&
               old_text[old_length - suffix - 1] ==
                   new_text[new_length - suffix - 1])
            suffix++;
        while (suffix > 0 &&
               (((unsigned char)old_text[old_length - suffix] & 0xc0) == 0x80 ||
                ((unsigned char)new_text[new_length - suffix] & 0xc0) == 0x80))
            suffix--;

        size_t start =
            (size_t)g_utf8_pointer_to_offset(old_text, old_text + prefix);
        size_t old_end = (size_t)g_utf8_pointer_to_offset(
            old_text, old_text + old_length - suffix);
        size_t new_end = (size_t)g_utf8_pointer_to_offset(
            new_text, new_text + new_length - suffix);
        size_t old_chars = old_end - start;
        size_t new_chars = new_end - start;
        if (old_chars)
        {
            char *deleted =
                g_strndup(old_text + prefix, old_length - suffix - prefix);
            append_node_event(events, new_node->id, NODE_EVENT_TEXT_CHANGED,
                              "delete", offset_as_int(start),
                              offset_as_int(old_chars), false, deleted);
            g_free(deleted);
        }
        if (new_chars)
        {
            char *inserted =
                g_strndup(new_text + prefix, new_length - suffix - prefix);
            append_node_event(events, new_node->id, NODE_EVENT_TEXT_CHANGED,
                              "insert", offset_as_int(start),
                              offset_as_int(new_chars), false, inserted);
            g_free(inserted);
        }
    }

    if (old_node->caret_offset != new_node->caret_offset)
    {
        append_node_event(
            events, new_node->id, NODE_EVENT_TEXT_CARET_MOVED, "",
            offset_as_int(text_byte_to_char(new_text, new_node->caret_offset)),
            0, false, "");
    }
    if (old_node->selection_start != new_node->selection_start ||
        old_node->selection_end != new_node->selection_end)
    {
        append_node_event(events, new_node->id,
                          NODE_EVENT_TEXT_SELECTION_CHANGED, "", 0, 0, false,
                          "");
    }
}

static void destroy_node_events(GArray *events)
{
    if (!events)
        return;
    for (guint i = 0; i < events->len; i++)
        g_free(g_array_index(events, struct node_event, i).text);
    g_array_unref(events);
}

static gboolean sync_objects(gpointer data)
{
    struct provider_server *s = data;
    if (!s->connection || !s->interfaces)
        return G_SOURCE_REMOVE;

    GArray *desired = g_array_new(FALSE, FALSE, sizeof(struct registered_node));
    pthread_mutex_lock(&s->lock);
    for (struct provider_window *w = s->windows; w; w = w->next)
    {
        if (w->detaching)
            continue;
        for (size_t i = 0; i < w->snapshot.count; i++)
        {
            const struct ui_accessibility_node *node = &w->snapshot.nodes[i];
            struct registered_node want = {
                .id = node->id,
                .action_id =
                    node_action_enabled(w, node, UI_ACCESSIBILITY_ACTION_INVOKE)
                        ? 1
                        : 0,
                .text_id =
                    ((node->states & UI_ACCESSIBILITY_STATE_PASSWORD) ||
                     (node->value && g_utf8_validate(node->value, -1, NULL)))
                        ? 1
                        : 0,
                .editable_text_id =
                    ((node->states & UI_ACCESSIBILITY_STATE_PASSWORD) ||
                     (node->value && g_utf8_validate(node->value, -1, NULL))) &&
                            node_action_enabled(
                                w, node, UI_ACCESSIBILITY_ACTION_SET_VALUE)
                        ? 1
                        : 0,
            };
            g_array_append_val(desired, want);
        }
    }
    pthread_mutex_unlock(&s->lock);

    for (guint i = 0; i < s->registered->len;)
    {
        struct registered_node *current = g_ptr_array_index(s->registered, i);
        bool keep = false;
        struct registered_node want = {0};
        for (guint j = 0; j < desired->len; j++)
        {
            struct registered_node *candidate =
                &g_array_index(desired, struct registered_node, j);
            if (candidate->id == current->id)
            {
                keep = true;
                want = *candidate;
                break;
            }
        }
        char *path = node_path(current->id);
        if (!keep)
        {
            g_dbus_connection_unregister_object(s->connection,
                                                current->accessible_id);
            g_dbus_connection_unregister_object(s->connection,
                                                current->component_id);
            if (current->action_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    current->action_id);
            if (current->text_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    current->text_id);
            if (current->editable_text_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    current->editable_text_id);
            g_free(path);
            g_ptr_array_remove_index_fast(s->registered, i);
            continue;
        }
        const char *optional_ifaces[] = {ATSPI_ACTION_IFACE, ATSPI_TEXT_IFACE,
                                         ATSPI_EDITABLE_TEXT_IFACE};
        guint *registered_ids[] = {&current->action_id, &current->text_id,
                                   &current->editable_text_id};
        const guint wants[] = {want.action_id, want.text_id,
                               want.editable_text_id};
        for (size_t k = 0; k < G_N_ELEMENTS(optional_ifaces); k++)
        {
            if (wants[k] && !*registered_ids[k])
            {
                const GDBusInterfaceInfo *info =
                    g_dbus_node_info_lookup_interface(s->interfaces,
                                                      optional_ifaces[k]);
                *registered_ids[k] = g_dbus_connection_register_object(
                    s->connection, path, (GDBusInterfaceInfo *)info,
                    &atspi_vtable, s, NULL, NULL);
            }
            else if (!wants[k] && *registered_ids[k])
            {
                g_dbus_connection_unregister_object(s->connection,
                                                    *registered_ids[k]);
                *registered_ids[k] = 0;
            }
        }
        g_free(path);
        i++;
    }

    for (guint i = 0; i < desired->len; i++)
    {
        struct registered_node *want =
            &g_array_index(desired, struct registered_node, i);
        bool found = false;
        for (guint j = 0; j < s->registered->len; j++)
        {
            if (((struct registered_node *)g_ptr_array_index(s->registered, j))
                    ->id == want->id)
            {
                found = true;
                break;
            }
        }
        if (found)
            continue;
        char *path = node_path(want->id);
        const GDBusInterfaceInfo *accessible =
            g_dbus_node_info_lookup_interface(s->interfaces,
                                              ATSPI_ACCESSIBLE_IFACE);
        const GDBusInterfaceInfo *component = g_dbus_node_info_lookup_interface(
            s->interfaces, ATSPI_COMPONENT_IFACE);
        const GDBusInterfaceInfo *text =
            g_dbus_node_info_lookup_interface(s->interfaces, ATSPI_TEXT_IFACE);
        const GDBusInterfaceInfo *editable_text =
            g_dbus_node_info_lookup_interface(s->interfaces,
                                              ATSPI_EDITABLE_TEXT_IFACE);
        struct registered_node reg = {.id = want->id};
        reg.accessible_id = g_dbus_connection_register_object(
            s->connection, path, (GDBusInterfaceInfo *)accessible,
            &atspi_vtable, s, NULL, NULL);
        reg.component_id = g_dbus_connection_register_object(
            s->connection, path, (GDBusInterfaceInfo *)component, &atspi_vtable,
            s, NULL, NULL);
        if (want->action_id)
        {
            const GDBusInterfaceInfo *action =
                g_dbus_node_info_lookup_interface(s->interfaces,
                                                  ATSPI_ACTION_IFACE);
            reg.action_id = g_dbus_connection_register_object(
                s->connection, path, (GDBusInterfaceInfo *)action,
                &atspi_vtable, s, NULL, NULL);
        }
        if (want->text_id)
            reg.text_id = g_dbus_connection_register_object(
                s->connection, path, (GDBusInterfaceInfo *)text, &atspi_vtable,
                s, NULL, NULL);
        if (want->editable_text_id)
            reg.editable_text_id = g_dbus_connection_register_object(
                s->connection, path, (GDBusInterfaceInfo *)editable_text,
                &atspi_vtable, s, NULL, NULL);
        if (reg.accessible_id && reg.component_id)
        {
            struct registered_node *stored = g_new(struct registered_node, 1);
            *stored = reg;
            g_ptr_array_add(s->registered, stored);
        }
        else
        {
            if (reg.accessible_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg.accessible_id);
            if (reg.component_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg.component_id);
            if (reg.action_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg.action_id);
            if (reg.text_id)
                g_dbus_connection_unregister_object(s->connection, reg.text_id);
            if (reg.editable_text_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg.editable_text_id);
        }
        g_free(path);
    }
    g_array_unref(desired);
    return G_SOURCE_REMOVE;
}

static void schedule_source(GMainContext *context, GSourceFunc callback,
                            gpointer data, GDestroyNotify destroy)
{
    if (!context)
        return;
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, callback, data, destroy);
    g_source_attach(source, context);
    g_source_unref(source);
}

static GVariant *call_sync(GDBusConnection *connection, const char *name,
                           const char *path, const char *interface,
                           const char *method, GVariant *parameters,
                           const GVariantType *reply_type, GError **error)
{
    return g_dbus_connection_call_sync(
        connection, name, path, interface, method, parameters, reply_type,
        G_DBUS_CALL_FLAGS_NONE, 250, NULL, error);
}

static bool connect_atspi_bus(void)
{
    GError *error = NULL;
    GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!session)
    {
        g_clear_error(&error);
        return false;
    }
    GVariant *address_result =
        call_sync(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                  "GetAddress", NULL, G_VARIANT_TYPE("(s)"), &error);
    if (!address_result)
    {
        g_clear_error(&error);
        g_object_unref(session);
        return false;
    }
    const char *address = NULL;
    g_variant_get(address_result, "(&s)", &address);
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
        address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    g_variant_unref(address_result);
    g_object_unref(session);
    if (!connection)
    {
        g_clear_error(&error);
        return false;
    }

    GDBusNodeInfo *interfaces = g_dbus_node_info_new_for_xml(atspi_xml, &error);
    if (!interfaces)
    {
        g_clear_error(&error);
        g_object_unref(connection);
        return false;
    }
    const GDBusInterfaceInfo *accessible =
        g_dbus_node_info_lookup_interface(interfaces, ATSPI_ACCESSIBLE_IFACE);
    const GDBusInterfaceInfo *application =
        g_dbus_node_info_lookup_interface(interfaces, ATSPI_APPLICATION_IFACE);
    guint accessible_id = g_dbus_connection_register_object(
        connection, ATSPI_ROOT_PATH, (GDBusInterfaceInfo *)accessible,
        &atspi_vtable, &server, NULL, &error);
    guint application_id = accessible_id
                               ? g_dbus_connection_register_object(
                                     connection, ATSPI_ROOT_PATH,
                                     (GDBusInterfaceInfo *)application,
                                     &atspi_vtable, &server, NULL, &error)
                               : 0;
    if (!accessible_id || !application_id)
    {
        if (accessible_id)
            g_dbus_connection_unregister_object(connection, accessible_id);
        if (application_id)
            g_dbus_connection_unregister_object(connection, application_id);
        g_clear_error(&error);
        g_dbus_node_info_unref(interfaces);
        g_object_unref(connection);
        return false;
    }

    const char *unique_name = g_dbus_connection_get_unique_name(connection);
    GVariant *embedded = call_sync(
        connection, ATSPI_REGISTRY_NAME, ATSPI_ROOT_PATH, ATSPI_SOCKET_IFACE,
        "Embed", g_variant_new("((so))", unique_name, ATSPI_ROOT_PATH),
        G_VARIANT_TYPE("((so))"), &error);
    if (!embedded)
    {
        g_clear_error(&error);
        g_dbus_connection_unregister_object(connection, application_id);
        g_dbus_connection_unregister_object(connection, accessible_id);
        g_dbus_node_info_unref(interfaces);
        g_object_unref(connection);
        return false;
    }
    g_variant_unref(embedded);

    pthread_mutex_lock(&server.lock);
    server.connection = connection;
    server.interfaces = interfaces;
    server.registered = g_ptr_array_new_with_free_func(g_free);
    bool stopping = server.stopping;
    pthread_mutex_unlock(&server.lock);
    if (!stopping)
    {
        sync_objects(&server);
        GArray *focus_ids = g_array_new(FALSE, FALSE, sizeof(guint64));
        pthread_mutex_lock(&server.lock);
        for (struct provider_window *w = server.windows; w; w = w->next)
            if (!w->detaching && w->focused_id)
                g_array_append_val(focus_ids, w->focused_id);
        pthread_mutex_unlock(&server.lock);
        for (guint i = 0; i < focus_ids->len; i++)
        {
            guint64 *focus_id = g_new(guint64, 1);
            *focus_id = g_array_index(focus_ids, guint64, i);
            emit_focus_event(focus_id);
            g_free(focus_id);
        }
        g_array_unref(focus_ids);
    }
    return true;
}

static gboolean stop_loop(gpointer data)
{
    GMainLoop *loop = data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gpointer accessibility_thread(gpointer data)
{
    struct provider_server *s = data;
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop *loop = g_main_loop_new(context, FALSE);

    pthread_mutex_lock(&s->lock);
    s->context = context;
    s->loop = loop;
    bool stopping = s->stopping;
    pthread_cond_broadcast(&s->idle);
    pthread_mutex_unlock(&s->lock);

    if (!stopping)
        connect_atspi_bus();

    pthread_mutex_lock(&s->lock);
    stopping = s->stopping;
    pthread_mutex_unlock(&s->lock);
    if (!stopping)
        g_main_loop_run(loop);

    if (s->registered)
    {
        for (guint i = 0; i < s->registered->len; i++)
        {
            struct registered_node *reg = g_ptr_array_index(s->registered, i);
            if (reg->accessible_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg->accessible_id);
            if (reg->component_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg->component_id);
            if (reg->action_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg->action_id);
            if (reg->text_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg->text_id);
            if (reg->editable_text_id)
                g_dbus_connection_unregister_object(s->connection,
                                                    reg->editable_text_id);
        }
        g_ptr_array_unref(s->registered);
        s->registered = NULL;
    }
    pthread_mutex_lock(&s->lock);
    GDBusConnection *connection = s->connection;
    s->connection = NULL;
    pthread_mutex_unlock(&s->lock);
    if (connection)
    {
        g_dbus_connection_close_sync(connection, NULL, NULL);
        g_object_unref(connection);
    }
    if (s->interfaces)
    {
        g_dbus_node_info_unref(s->interfaces);
        s->interfaces = NULL;
    }
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    pthread_mutex_lock(&s->lock);
    s->context = NULL;
    s->loop = NULL;
    pthread_cond_broadcast(&s->idle);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void *provider_attach(void *native_handle,
                             ui_accessibility_action_sink_fn action_sink,
                             void *action_userdata)
{
    if (!native_handle)
        return NULL;
    struct provider_window *window = calloc(1, sizeof(*window));
    if (!window)
        return NULL;
    window->native_handle = native_handle;
    window->action_sink = action_sink;
    window->action_userdata = action_userdata;
    window->scale_120 = surface_scale_120(native_handle);

    pthread_mutex_lock(&server.lock);
    while (server.stopping)
        pthread_cond_wait(&server.idle, &server.lock);
    if (!server.thread)
    {
        GError *error = NULL;
        server.thread = g_thread_try_new("wsap-atspi", accessibility_thread,
                                         &server, &error);
        if (!server.thread)
        {
            g_clear_error(&error);
            pthread_mutex_unlock(&server.lock);
            free(window);
            return NULL;
        }
    }
    struct provider_window **tail = &server.windows;
    while (*tail)
        tail = &(*tail)->next;
    *tail = window;
    GMainContext *context =
        server.context ? g_main_context_ref(server.context) : NULL;
    pthread_mutex_unlock(&server.lock);
    schedule_source(context, sync_objects, &server, NULL);
    if (context)
        g_main_context_unref(context);
    return window;
}

static bool node_spec_matches_node(const struct ui_accessibility_node *node,
                                   const struct ui_accessibility_node *spec)
{
    if (node->id != spec->id || node->parent_id != spec->parent_id ||
        node->order != spec->order || node->role != spec->role ||
        node->states != spec->states || node->actions != spec->actions ||
        !rect2i_eq(node->bounds, spec->bounds) ||
        node->caret_offset != spec->caret_offset ||
        node->selection_start != spec->selection_start ||
        node->selection_end != spec->selection_end)
        return false;
    if ((node->name != NULL) != (spec->name != NULL) ||
        (node->name && strcmp(node->name, spec->name) != 0))
        return false;
    if ((node->description != NULL) != (spec->description != NULL) ||
        (node->description &&
         strcmp(node->description, spec->description) != 0))
        return false;
    if ((node->value != NULL) != (spec->value != NULL) ||
        (node->value && strcmp(node->value, spec->value) != 0))
        return false;
    return true;
}

static bool trees_match(const struct ui_accessibility_tree *a,
                        const struct ui_accessibility_tree *b)
{
    if (a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; i++)
    {
        if (!node_spec_matches_node(&a->nodes[i], &b->nodes[i]))
            return false;
    }
    return true;
}

static bool
provider_publish_snapshot(void *instance,
                          const struct ui_accessibility_tree *snapshot)
{
    struct provider_window *window = instance;
    if (!window || !snapshot)
        return false;

    pthread_mutex_lock(&server.lock);
    if (!server.connection || window->detaching)
    {
        pthread_mutex_unlock(&server.lock);
        return false;
    }
    uint32_t old_scale_120 = window->scale_120;
    uint32_t new_scale_120 = surface_scale_120(window->native_handle);
    if (old_scale_120 == new_scale_120 &&
        trees_match(&window->snapshot, snapshot))
    {
        pthread_mutex_unlock(&server.lock);
        return true;
    }
    pthread_mutex_unlock(&server.lock);

    struct ui_accessibility_tree copy = {0};
    for (size_t i = 0; i < snapshot->count; i++)
    {
        const struct ui_accessibility_node *node = &snapshot->nodes[i];
        if (!ui_accessibility_tree_add(
                &copy, &(struct ui_accessibility_node_spec){
                           .id = node->id,
                           .parent_id = node->parent_id,
                           .order = node->order,
                           .role = node->role,
                           .states = node->states,
                           .actions = node->actions,
                           .bounds = node->bounds,
                           .name = node->name,
                           .description = node->description,
                           .value = node->value,
                           .caret_offset = node->caret_offset,
                           .selection_start = node->selection_start,
                           .selection_end = node->selection_end,
                       }))
        {
            ui_accessibility_tree_destroy(&copy);
            return false;
        }
    }

    uint64_t new_focus = 0;
    for (size_t i = 0; i < copy.count; i++)
    {
        if (copy.nodes[i].states & UI_ACCESSIBILITY_STATE_FOCUSED)
        {
            new_focus = copy.nodes[i].id;
            break;
        }
    }
    pthread_mutex_lock(&server.lock);
    if (window->detaching)
    {
        pthread_mutex_unlock(&server.lock);
        ui_accessibility_tree_destroy(&copy);
        return false;
    }
    old_scale_120 = window->scale_120;
    new_scale_120 = surface_scale_120(window->native_handle);

    GArray *removed_children =
        g_array_new(FALSE, FALSE, sizeof(struct child_change));
    GArray *added_children =
        g_array_new(FALSE, FALSE, sizeof(struct child_change));
    GArray *node_events = g_array_new(FALSE, FALSE, sizeof(struct node_event));
    for (size_t i = 0; i < window->snapshot.count; i++)
    {
        const struct ui_accessibility_node *old_node =
            &window->snapshot.nodes[i];
        const struct ui_accessibility_node *new_node =
            ui_accessibility_tree_find(&copy, old_node->id);
        if (new_node)
        {
            append_state_changes(node_events, old_node, new_node);
            append_property_changes(node_events, old_node, new_node);
            struct rect2i old_bounds =
                rect_to_logical(window->native_handle, old_node->bounds);
            struct rect2i new_bounds =
                rect_to_logical(window->native_handle, new_node->bounds);
            if (!rect2i_eq(old_bounds, new_bounds))
                append_bounds_change(node_events, new_node->id, new_bounds);
            append_text_changes(node_events, old_node, new_node);
        }
        if (new_node && new_node->parent_id == old_node->parent_id &&
            new_node->order == old_node->order)
            continue;
        if (old_node->parent_id &&
            !ui_accessibility_tree_find(&copy, old_node->parent_id))
            continue;
        struct child_change change = {
            .parent_id = old_node->parent_id,
            .child_id = old_node->id,
            .index = index_in_parent_locked(old_node->id),
            .added = false,
        };
        g_array_append_val(removed_children, change);
    }
    struct ui_accessibility_tree old = window->snapshot;
    window->snapshot = copy;
    for (size_t i = 0; i < window->snapshot.count; i++)
    {
        const struct ui_accessibility_node *new_node =
            &window->snapshot.nodes[i];
        const struct ui_accessibility_node *old_node =
            ui_accessibility_tree_find(&old, new_node->id);
        if (old_node && old_node->parent_id == new_node->parent_id &&
            old_node->order == new_node->order)
            continue;
        if (new_node->parent_id && !find_node_locked(new_node->parent_id, NULL))
            continue;
        struct child_change change = {
            .parent_id = new_node->parent_id,
            .child_id = new_node->id,
            .index = index_in_parent_locked(new_node->id),
            .added = true,
        };
        g_array_append_val(added_children, change);
    }
    window->scale_120 = new_scale_120;
    bool focus_changed = window->focused_id != new_focus;
    window->focused_id = new_focus;
    GMainContext *context = server.context && !server.stopping
                                ? g_main_context_ref(server.context)
                                : NULL;
    pthread_mutex_unlock(&server.lock);
    schedule_child_changes(context, removed_children);
    schedule_source(context, sync_objects, &server, NULL);
    schedule_child_changes(context, added_children);
    schedule_node_events(context, node_events);
    if (focus_changed && new_focus && context)
    {
        guint64 *focus_id = g_new(guint64, 1);
        *focus_id = new_focus;
        schedule_source(context, emit_focus_event, focus_id, g_free);
    }
    if (context)
        g_main_context_unref(context);
    g_array_unref(removed_children);
    g_array_unref(added_children);
    destroy_node_events(node_events);
    ui_accessibility_tree_destroy(&old);
    return true;
}

static void provider_detach(void *instance)
{
    struct provider_window *window = instance;
    if (!window)
        return;
    GArray *removed_children =
        g_array_new(FALSE, FALSE, sizeof(struct child_change));
    GThread *thread_to_join = NULL;
    GMainContext *context = NULL;
    GMainLoop *loop = NULL;

    pthread_mutex_lock(&server.lock);
    for (size_t i = 0; i < window->snapshot.count; i++)
    {
        const struct ui_accessibility_node *node = &window->snapshot.nodes[i];
        if (node->parent_id)
            continue;
        struct child_change change = {
            .parent_id = 0,
            .child_id = node->id,
            .index = index_in_parent_locked(node->id),
            .added = false,
        };
        g_array_append_val(removed_children, change);
    }
    window->detaching = true;
    struct provider_window **cursor = &server.windows;
    while (*cursor && *cursor != window)
        cursor = &(*cursor)->next;
    if (*cursor)
        *cursor = window->next;
    while (window->active_sinks)
        pthread_cond_wait(&server.idle, &server.lock);
    if (!server.windows)
    {
        server.stopping = true;
        thread_to_join = server.thread;
        context = server.context ? g_main_context_ref(server.context) : NULL;
        loop = server.loop ? g_main_loop_ref(server.loop) : NULL;
    }
    else if (server.context)
    {
        context = g_main_context_ref(server.context);
    }
    pthread_mutex_unlock(&server.lock);

    schedule_child_changes(context, removed_children);
    if (thread_to_join)
    {
        if (context && loop)
            schedule_source(context, stop_loop, loop,
                            (GDestroyNotify)g_main_loop_unref);
        else if (loop)
            g_main_loop_unref(loop);
        g_thread_join(thread_to_join);
        if (context)
            g_main_context_unref(context);
        pthread_mutex_lock(&server.lock);
        server.thread = NULL;
        server.stopping = false;
        server.registry_id = 0;
        pthread_cond_broadcast(&server.idle);
        pthread_mutex_unlock(&server.lock);
    }
    else
    {
        schedule_source(context, sync_objects, &server, NULL);
        if (context)
            g_main_context_unref(context);
    }
    g_array_unref(removed_children);
    ui_accessibility_tree_destroy(&window->snapshot);
    free(window);
}

static bool provider_is_active(void *instance)
{
    (void)instance;
    pthread_mutex_lock(&server.lock);
    bool active = server.connection != NULL && !server.stopping;
    pthread_mutex_unlock(&server.lock);
    return active;
}

static const struct ui_accessibility_provider_ops provider_ops = {
    .attach = provider_attach,
    .publish_snapshot = provider_publish_snapshot,
    .detach = provider_detach,
    .is_active = provider_is_active,
};

const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void)
{
    return &provider_ops;
}

void *ui_accessibility_native_handle(struct yasl_surface *s)
{
    return s;
}

void ui_accessibility_set_menu_popup_depth(
    const struct ui_accessibility_provider *provider, int depth)
{
    if (!provider || provider->ops != &provider_ops || !provider->instance ||
        depth < 0)
        return;
    struct provider_window *window = provider->instance;
    pthread_mutex_lock(&server.lock);
    window->popup_level = depth + 1;
    pthread_mutex_unlock(&server.lock);
}
