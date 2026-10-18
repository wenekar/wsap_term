#define _GNU_SOURCE

#include "ui/yasl/accessibility/accessibility.h"
#include "yasl/yasl.h"

#include <accesskit.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct text_run {
    accesskit_node_id id;
    size_t start, length, characters;
    uint8_t *lengths;
};

struct mapped_node {
    accesskit_node_id id;
    accesskit_node_id text_id;
    size_t first_child, next_sibling;
    struct text_run *runs;
    size_t run_count;
};

struct snapshot {
    struct ui_accessibility_tree tree;
    /* The last entry is the native window root. Child links use index + 1. */
    struct mapped_node *nodes;
};

struct provider {
    struct provider *next;
    uintptr_t token;
    struct yasl_surface *surface;
    ui_accessibility_action_sink_fn sink;
    void *userdata;
    void (*wake)(void *userdata);
    void *wake_userdata;
    bool active;
    struct snapshot snapshot;
    accesskit_node_id next_node_id;
    double scale;
#if defined(ACCESSKIT_MACOS)
    struct accesskit_macos_subclassing_adapter *adapter;
#else
    struct accesskit_unix_adapter *adapter;
#endif
};

/* Unix can call handlers after adapter_free returns. Tokens never name a new
 * provider. Removal under this lock also waits for an active action sink. */
static pthread_mutex_t providers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct provider *providers;
static uintptr_t next_token;

static struct provider *find_provider(void *token)
{
    for (struct provider *p = providers; p; p = p->next)
        if (p->token == (uintptr_t)token)
            return p;
    return NULL;
}

static void snapshot_destroy(struct snapshot *s)
{
    if (s->nodes) {
        for (size_t i = 0; i < s->tree.count; i++) {
            for (size_t j = 0; j < s->nodes[i].run_count; j++)
                free(s->nodes[i].runs[j].lengths);
            free(s->nodes[i].runs);
        }
        free(s->nodes);
    }
    ui_accessibility_tree_destroy(&s->tree);
    *s = (struct snapshot){0};
}

static accesskit_node_id new_node_id(struct provider *p)
{
    return p->next_node_id == UINT64_MAX ? 0 : ++p->next_node_id;
}

static const char *node_text(const struct ui_accessibility_node *node)
{
    if (node->states & UI_ACCESSIBILITY_STATE_PASSWORD)
        return "";
    return node->value;
}

static bool native_text_role(const struct ui_accessibility_node *node)
{
    return (node->states & UI_ACCESSIBILITY_STATE_PASSWORD) ||
        node->role == UI_ACCESSIBILITY_ROLE_LABEL ||
        node->role == UI_ACCESSIBILITY_ROLE_TEXT_FIELD ||
        node->role == UI_ACCESSIBILITY_ROLE_SEARCH_FIELD ||
        node->role == UI_ACCESSIBILITY_ROLE_TERMINAL;
}

static bool map_text(struct provider *p, struct mapped_node *map,
                     const struct mapped_node *old, const char *text)
{
    if (!text)
        return true;
    map->run_count = 1;
    for (const char *c = text; *c; c++)
        if (*c == '\n')
            map->run_count++;
    map->runs = calloc(map->run_count, sizeof(*map->runs));
    if (!map->runs) {
        map->run_count = 0;
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i < map->run_count; i++) {
        struct text_run *run = &map->runs[i];
        const char *end = strchr(text + start, '\n');
        run->start = start;
        run->length = end ? (size_t)(end + 1 - text) - start
                          : strlen(text + start);
        run->id = old && i < old->run_count ? old->runs[i].id : new_node_id(p);
        run->lengths = malloc(run->length ? run->length : 1);
        if (!run->id || !run->lengths)
            return false;
        for (size_t j = 0; j < run->length;) {
            unsigned char c = (unsigned char)text[start + j];
            /* The UI tree supplies valid UTF-8. CRLF is one text character. */
            size_t length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (c == '\r' && j + 1 < run->length && text[start + j + 1] == '\n')
                length = 2;
            run->lengths[run->characters++] = (uint8_t)length;
            j += length;
        }
        start += run->length;
    }
    return true;
}

static bool snapshot_copy(struct provider *p,
                          const struct ui_accessibility_tree *tree,
                          struct snapshot *copy)
{
    if (!ui_accessibility_tree_copy(&copy->tree, tree))
        return false;
    copy->nodes = calloc(tree->count + 1, sizeof(*copy->nodes));
    if (!copy->nodes)
        return false;
    copy->nodes[tree->count].id = 1;
    for (size_t i = 0; i < tree->count; i++) {
        const struct ui_accessibility_node *node = &tree->nodes[i];
        const struct ui_accessibility_node *previous =
            ui_accessibility_tree_find(&p->snapshot.tree, node->id);
        const struct mapped_node *old = previous
            ? &p->snapshot.nodes[previous - p->snapshot.tree.nodes] : NULL;
        struct mapped_node *map = &copy->nodes[i];
        map->id = old ? old->id : new_node_id(p);
        if (!map->id || !map_text(p, map, old, node_text(node)))
            return false;
        if (map->run_count && !native_text_role(node)) {
            map->text_id = old && old->text_id ? old->text_id : new_node_id(p);
            if (!map->text_id)
                return false;
        }
        const struct ui_accessibility_node *parent =
            ui_accessibility_tree_find(tree, node->parent_id);
        size_t parent_index = parent ? (size_t)(parent - tree->nodes) : tree->count;
        size_t *link = &copy->nodes[parent_index].first_child;
        while (*link && tree->nodes[*link - 1].order <= node->order)
            link = &copy->nodes[*link - 1].next_sibling;
        map->next_sibling = *link;
        *link = i + 1;
    }
    return true;
}

static accesskit_role node_role(const struct ui_accessibility_node *node)
{
    if (node->states & UI_ACCESSIBILITY_STATE_PASSWORD)
        return ACCESSKIT_ROLE_PASSWORD_INPUT;
    switch (node->role) {
    case UI_ACCESSIBILITY_ROLE_UNKNOWN: return ACCESSKIT_ROLE_UNKNOWN;
    case UI_ACCESSIBILITY_ROLE_WINDOW:
    case UI_ACCESSIBILITY_ROLE_GROUP: return ACCESSKIT_ROLE_GROUP;
    case UI_ACCESSIBILITY_ROLE_DIALOG: return ACCESSKIT_ROLE_DIALOG;
    case UI_ACCESSIBILITY_ROLE_LABEL: return ACCESSKIT_ROLE_LABEL;
    case UI_ACCESSIBILITY_ROLE_HEADING: return ACCESSKIT_ROLE_HEADING;
    case UI_ACCESSIBILITY_ROLE_BUTTON:
    case UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON: return ACCESSKIT_ROLE_BUTTON;
    case UI_ACCESSIBILITY_ROLE_CHECKBOX: return ACCESSKIT_ROLE_CHECK_BOX;
    case UI_ACCESSIBILITY_ROLE_RADIO_BUTTON: return ACCESSKIT_ROLE_RADIO_BUTTON;
    case UI_ACCESSIBILITY_ROLE_TAB_LIST: return ACCESSKIT_ROLE_TAB_LIST;
    case UI_ACCESSIBILITY_ROLE_TAB: return ACCESSKIT_ROLE_TAB;
    case UI_ACCESSIBILITY_ROLE_TEXT_FIELD:
        return node->states & UI_ACCESSIBILITY_STATE_MULTILINE
            ? ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT : ACCESSKIT_ROLE_TEXT_INPUT;
    case UI_ACCESSIBILITY_ROLE_SEARCH_FIELD: return ACCESSKIT_ROLE_SEARCH_INPUT;
    case UI_ACCESSIBILITY_ROLE_LINK: return ACCESSKIT_ROLE_LINK;
    case UI_ACCESSIBILITY_ROLE_LIST: return ACCESSKIT_ROLE_LIST;
    case UI_ACCESSIBILITY_ROLE_LIST_ITEM: return ACCESSKIT_ROLE_LIST_ITEM;
    case UI_ACCESSIBILITY_ROLE_MENU: return ACCESSKIT_ROLE_MENU;
    case UI_ACCESSIBILITY_ROLE_MENU_ITEM: return ACCESSKIT_ROLE_MENU_ITEM;
    case UI_ACCESSIBILITY_ROLE_SLIDER: return ACCESSKIT_ROLE_SLIDER;
    case UI_ACCESSIBILITY_ROLE_PROGRESS_INDICATOR: return ACCESSKIT_ROLE_PROGRESS_INDICATOR;
    case UI_ACCESSIBILITY_ROLE_SCROLL_BAR: return ACCESSKIT_ROLE_SCROLL_BAR;
    case UI_ACCESSIBILITY_ROLE_IMAGE: return ACCESSKIT_ROLE_IMAGE;
    case UI_ACCESSIBILITY_ROLE_TERMINAL: return ACCESSKIT_ROLE_TERMINAL;
    case UI_ACCESSIBILITY_ROLE_COMBO_BOX: return ACCESSKIT_ROLE_COMBO_BOX;
    }
    return ACCESSKIT_ROLE_UNKNOWN;
}

static const struct {
    enum ui_accessibility_action ui;
    accesskit_action native;
} actions[] = {
    {UI_ACCESSIBILITY_ACTION_INVOKE, ACCESSKIT_ACTION_CLICK},
    {UI_ACCESSIBILITY_ACTION_FOCUS, ACCESSKIT_ACTION_FOCUS},
    {UI_ACCESSIBILITY_ACTION_SET_VALUE, ACCESSKIT_ACTION_SET_VALUE},
    {UI_ACCESSIBILITY_ACTION_SET_SELECTION, ACCESSKIT_ACTION_SET_TEXT_SELECTION},
    {UI_ACCESSIBILITY_ACTION_INCREMENT, ACCESSKIT_ACTION_INCREMENT},
    {UI_ACCESSIBILITY_ACTION_DECREMENT, ACCESSKIT_ACTION_DECREMENT},
};

static struct accesskit_text_position text_position(const struct mapped_node *map,
                                                    size_t offset)
{
    size_t i = 0;
    while (i + 1 < map->run_count &&
           offset >= map->runs[i + 1].start)
        i++;
    const struct text_run *run = &map->runs[i];
    size_t character = 0, byte = run->start;
    while (character < run->characters &&
           byte + run->lengths[character] <= offset)
        byte += run->lengths[character++];
    return (struct accesskit_text_position){run->id, character};
}

static bool text_offset(const struct mapped_node *map,
                        struct accesskit_text_position position, size_t *offset)
{
    for (size_t i = 0; i < map->run_count; i++) {
        const struct text_run *run = &map->runs[i];
        if (run->id != position.node || position.character_index > run->characters)
            continue;
        *offset = run->start;
        for (size_t j = 0; j < position.character_index; j++)
            *offset += run->lengths[j];
        return true;
    }
    return false;
}

static struct accesskit_rect node_bounds(struct rect2i bounds, double scale)
{
    return (struct accesskit_rect){
        .x0 = bounds.x / scale, .y0 = bounds.y / scale,
        .x1 = ((double)bounds.x + bounds.width) / scale,
        .y1 = ((double)bounds.y + bounds.height) / scale,
    };
}

static void set_selection(struct accesskit_node *out,
                          const struct ui_accessibility_node *node,
                          const struct mapped_node *map)
{
    size_t anchor = node->selection_start, focus = node->selection_end;
    if (anchor == focus)
        anchor = focus = node->caret_offset;
    else if (node->caret_offset == anchor) {
        anchor = node->selection_end;
        focus = node->selection_start;
    }
    accesskit_node_set_text_selection(out, (struct accesskit_text_selection){
        .anchor = text_position(map, anchor), .focus = text_position(map, focus),
    });
}

static void set_properties(struct accesskit_node *out,
                           const struct ui_accessibility_node *node,
                           const struct mapped_node *map, double scale,
                           bool interactive)
{
    if (node->name) accesskit_node_set_label(out, node->name);
    if (node->description) accesskit_node_set_description(out, node->description);
    if (node_text(node)) accesskit_node_set_value(out, node_text(node));
    else if (node->role == UI_ACCESSIBILITY_ROLE_LABEL && node->name)
        accesskit_node_set_value(out, node->name);
    accesskit_node_set_bounds(out, node_bounds(node->bounds, scale));
    uint32_t states = node->states;
    if (states & UI_ACCESSIBILITY_STATE_HIDDEN) accesskit_node_set_hidden(out);
    if (states & UI_ACCESSIBILITY_STATE_DISABLED) accesskit_node_set_disabled(out);
    if (!interactive || (states & UI_ACCESSIBILITY_STATE_READ_ONLY))
        accesskit_node_set_read_only(out);
    if (states & UI_ACCESSIBILITY_STATE_BUSY) accesskit_node_set_busy(out);
    if (states & UI_ACCESSIBILITY_STATE_REQUIRED) accesskit_node_set_required(out);
    if (states & UI_ACCESSIBILITY_STATE_INVALID)
        accesskit_node_set_invalid(out, ACCESSKIT_INVALID_TRUE);
    if ((states & UI_ACCESSIBILITY_STATE_CHECKED) ||
        node->role == UI_ACCESSIBILITY_ROLE_CHECKBOX ||
        node->role == UI_ACCESSIBILITY_ROLE_RADIO_BUTTON ||
        node->role == UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON)
        accesskit_node_set_toggled(out, states & UI_ACCESSIBILITY_STATE_CHECKED
            ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
    if ((states & UI_ACCESSIBILITY_STATE_SELECTED) ||
        node->role == UI_ACCESSIBILITY_ROLE_TAB ||
        node->role == UI_ACCESSIBILITY_ROLE_LIST_ITEM)
        accesskit_node_set_selected(out, !!(states & UI_ACCESSIBILITY_STATE_SELECTED));
    if (node->role == UI_ACCESSIBILITY_ROLE_COMBO_BOX ||
        (states & UI_ACCESSIBILITY_STATE_EXPANDED))
        accesskit_node_set_expanded(out, !!(states & UI_ACCESSIBILITY_STATE_EXPANDED));
    if (interactive &&
        !(states & (UI_ACCESSIBILITY_STATE_DISABLED | UI_ACCESSIBILITY_STATE_HIDDEN)))
        for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
            if (node->actions & actions[i].ui)
                accesskit_node_add_action(out, actions[i].native);
    if (map->run_count && !map->text_id)
        set_selection(out, node, map);
}

static struct accesskit_tree_update *build_update(struct provider *p)
{
    const struct snapshot *s = &p->snapshot;
    accesskit_node_id focus = 1;
    for (size_t i = 0; i < s->tree.count; i++)
        if (s->tree.nodes[i].states & UI_ACCESSIBILITY_STATE_FOCUSED)
            focus = s->nodes[i].id;
    struct accesskit_tree_update *update =
        accesskit_tree_update_with_capacity_and_focus(s->tree.count + 1, focus);
    struct accesskit_tree_info *info = accesskit_tree_info_new(1);
    accesskit_tree_info_set_toolkit_name(info, "wsap-ui");
    accesskit_tree_update_set_tree_info(update, info);
    for (size_t i = 0; i <= s->tree.count; i++) {
        const struct mapped_node *map = &s->nodes[i];
        const struct ui_accessibility_node *node =
            i < s->tree.count ? &s->tree.nodes[i] : NULL;
        struct accesskit_node *out = accesskit_node_new(
            node ? node_role(node) : ACCESSKIT_ROLE_WINDOW);
        if (node)
            set_properties(out, node, map, p->scale, p->sink != NULL);
        else if (s->tree.count) {
            const struct ui_accessibility_node *root = &s->tree.nodes[0];
            if (root->name) accesskit_node_set_label(out, root->name);
            accesskit_node_set_bounds(out, node_bounds(root->bounds, p->scale));
        }
        for (size_t child = map->first_child; child;
             child = s->nodes[child - 1].next_sibling)
            accesskit_node_push_child(out, s->nodes[child - 1].id);
        /* AccessKit exposes text ranges on labels, inputs, and terminals.
         * Other roles, such as a selectable link, need a text container. */
        struct accesskit_node *container = out;
        if (map->text_id) {
            container = accesskit_node_new(ACCESSKIT_ROLE_LABEL);
            set_properties(container, node, map, p->scale, p->sink != NULL);
            accesskit_node_clear_actions(container);
            if (p->sink && (node->actions & UI_ACCESSIBILITY_ACTION_SET_SELECTION) &&
                !(node->states & (UI_ACCESSIBILITY_STATE_DISABLED | UI_ACCESSIBILITY_STATE_HIDDEN)))
                accesskit_node_add_action(container, ACCESSKIT_ACTION_SET_TEXT_SELECTION);
            set_selection(container, node, map);
            accesskit_node_push_child(out, map->text_id);
        }
        for (size_t j = 0; j < map->run_count; j++) {
            const struct text_run *run = &map->runs[j];
            struct accesskit_node *text = accesskit_node_new(ACCESSKIT_ROLE_TEXT_RUN);
            accesskit_node_set_value_with_length(text, node_text(node) + run->start, run->length);
            accesskit_node_set_character_lengths(text, run->characters, run->lengths);
            accesskit_node_push_child(container, run->id);
            accesskit_tree_update_push_node(update, run->id, text);
        }
        if (map->text_id)
            accesskit_tree_update_push_node(update, map->text_id, container);
        accesskit_tree_update_push_node(update, map->id, out);
    }
    return update;
}

static struct accesskit_tree_update *make_update(void *token)
{
    pthread_mutex_lock(&providers_lock);
    struct provider *p = find_provider(token);
    struct accesskit_tree_update *update = p ? build_update(p) : NULL;
    pthread_mutex_unlock(&providers_lock);
    return update;
}

static struct accesskit_tree_update *activate(void *token)
{
    pthread_mutex_lock(&providers_lock);
    struct provider *p = find_provider(token);
    if (p) {
        p->active = true;
        if (p->wake) p->wake(p->wake_userdata);
    }
    pthread_mutex_unlock(&providers_lock);
    /* Request a fresh snapshot on the UI thread, also for an idle window. */
    return NULL;
}

static void handle_action(struct accesskit_action_request *native, void *token)
{
    pthread_mutex_lock(&providers_lock);
    struct provider *p = find_provider(token);
    if (!p || !p->sink)
        goto done;
    const struct snapshot *s = &p->snapshot;
    for (size_t i = 0; i < s->tree.count; i++) {
        const struct mapped_node *map = &s->nodes[i];
        const struct ui_accessibility_node *node = &s->tree.nodes[i];
        if ((map->id != native->target_node &&
             !(map->text_id == native->target_node &&
               native->action == ACCESSKIT_ACTION_SET_TEXT_SELECTION)) ||
            (node->states & (UI_ACCESSIBILITY_STATE_DISABLED | UI_ACCESSIBILITY_STATE_HIDDEN)))
            continue;
        struct ui_accessibility_action_request request = {.node_id = node->id};
        for (size_t j = 0; j < sizeof(actions) / sizeof(actions[0]); j++)
            if (actions[j].native == native->action)
                request.action = actions[j].ui;
        if (!request.action || !(node->actions & request.action))
            break;
        if (request.action == UI_ACCESSIBILITY_ACTION_SET_VALUE) {
            if (!native->data.has_value || native->data.value.tag != ACCESSKIT_ACTION_DATA_VALUE)
                break;
            request.value = native->data.value.value;
            request.caret_offset = request.selection_start = request.selection_end =
                request.value ? strlen(request.value) : 0;
        } else if (request.action == UI_ACCESSIBILITY_ACTION_SET_SELECTION) {
            if (!native->data.has_value ||
                native->data.value.tag != ACCESSKIT_ACTION_DATA_SET_TEXT_SELECTION)
                break;
            struct accesskit_text_selection selection = native->data.value.set_text_selection;
            if (!text_offset(map, selection.anchor, &request.selection_start) ||
                !text_offset(map, selection.focus, &request.selection_end))
                break;
            request.caret_offset = request.selection_end;
        }
        p->sink(p->userdata, &request);
        break;
    }
done:
    pthread_mutex_unlock(&providers_lock);
    /* AccessKit C 0.23.1 does not drop the optional action data. Release the
     * string and mark the data absent before freeing its request. */
    if (native->data.has_value && native->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE) {
        accesskit_string_free(native->data.value.value);
        native->data.has_value = false;
    }
    accesskit_action_request_free(native);
}

#if !defined(ACCESSKIT_MACOS)
static void deactivate(void *token)
{
    pthread_mutex_lock(&providers_lock);
    struct provider *p = find_provider(token);
    if (p) p->active = false;
    pthread_mutex_unlock(&providers_lock);
}
#endif

static void provider_detach(void *instance)
{
    struct provider *p = instance;
    pthread_mutex_lock(&providers_lock);
    struct provider **link = &providers;
    while (*link != p)
        link = &(*link)->next;
    *link = p->next;
    pthread_mutex_unlock(&providers_lock);
    /* Never hold providers_lock while entering the platform adapter. */
#if defined(ACCESSKIT_MACOS)
    if (p->adapter) accesskit_macos_subclassing_adapter_free(p->adapter);
#else
    if (p->adapter) accesskit_unix_adapter_free(p->adapter);
#endif
    snapshot_destroy(&p->snapshot);
    free(p);
}

static void *provider_attach(void *surface, ui_accessibility_action_sink_fn sink,
                             void *userdata)
{
    struct provider *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->surface = surface;
    p->sink = sink;
    p->userdata = userdata;
    p->scale = 1;
    p->next_node_id = 1;
    if (!snapshot_copy(p, &(struct ui_accessibility_tree){0}, &p->snapshot)) {
        snapshot_destroy(&p->snapshot);
        free(p);
        return NULL;
    }
    pthread_mutex_lock(&providers_lock);
    if (next_token == UINTPTR_MAX) {
        pthread_mutex_unlock(&providers_lock);
        snapshot_destroy(&p->snapshot);
        free(p);
        return NULL;
    }
    p->token = ++next_token;
    p->next = providers;
    providers = p;
    pthread_mutex_unlock(&providers_lock);
    void *token = (void *)p->token;
#if defined(ACCESSKIT_MACOS)
    struct yasl_native native = {0};
    if (!yasl_surface_get_native(surface, &native) && native.ns_view)
        p->adapter = accesskit_macos_subclassing_adapter_new(
            native.ns_view, activate, token, handle_action, token);
#else
    p->adapter = accesskit_unix_adapter_new(
        activate, token, handle_action, token, deactivate, token);
#endif
    if (!p->adapter) {
        provider_detach(p);
        return NULL;
    }
    return p;
}

static bool provider_publish(void *instance,
                             const struct ui_accessibility_tree *tree)
{
    struct provider *p = instance;
    struct yasl_surface_info info = yasl_surface_get_info(p->surface);
    struct snapshot copy = {0};
    pthread_mutex_lock(&providers_lock);
    bool copied = snapshot_copy(p, tree, &copy);
    if (copied) {
        snapshot_destroy(&p->snapshot);
        p->snapshot = copy;
        p->scale = info.scale > 0 ? info.scale : 1;
    }
    pthread_mutex_unlock(&providers_lock);
    if (!copied) {
        snapshot_destroy(&copy);
        return false;
    }
    void *token = (void *)p->token;
    /* A native menu popup has no toplevel activation state. Its grab keeps
     * keyboard focus until it closes. */
    bool focused = (info.state & YASL_STATE_ACTIVATED) ||
        (tree->count && tree->nodes[0].role == UI_ACCESSIBILITY_ROLE_MENU);
#if defined(ACCESSKIT_MACOS)
    struct accesskit_macos_queued_events *events =
        accesskit_macos_subclassing_adapter_update_if_active(p->adapter, make_update, token);
    if (events) accesskit_macos_queued_events_raise(events);
    events = accesskit_macos_subclassing_adapter_update_view_focus_state(p->adapter, focused);
    if (events) accesskit_macos_queued_events_raise(events);
#else
    accesskit_unix_adapter_update_if_active(p->adapter, make_update, token);
    accesskit_unix_adapter_update_window_focus_state(p->adapter, focused);
#endif
    return true;
}

static bool provider_is_active(void *instance)
{
    struct provider *p = instance;
    pthread_mutex_lock(&providers_lock);
    bool active = p->active;
    pthread_mutex_unlock(&providers_lock);
    return active;
}

const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void)
{
    static const struct ui_accessibility_provider_ops ops = {
        .attach = provider_attach,
        .publish_snapshot = provider_publish,
        .detach = provider_detach,
        .is_active = provider_is_active,
    };
    return &ops;
}

void ui_accessibility_set_wake_callback(
    const struct ui_accessibility_provider *provider,
    void (*wake)(void *userdata), void *userdata)
{
    if (!provider || provider->ops != ui_accessibility_provider_ops() || !provider->instance)
        return;
    pthread_mutex_lock(&providers_lock);
    struct provider *p = provider->instance;
    p->wake = wake;
    p->wake_userdata = userdata;
    if (p->active && wake) wake(userdata);
    pthread_mutex_unlock(&providers_lock);
}
