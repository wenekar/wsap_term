#define _GNU_SOURCE

#include "ui/yasl/accessibility/accessibility.h"

#include "ui/widget_impl.h"
#include "util/grow.h"
#include "wsap_vtcore/unicode/utf8.h"
#include "yasl/yasl.h"

#include <accesskit.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unicode/ubrk.h>
#include <unicode/utext.h>

#define RUN_CHARACTERS 255

struct text_run {
    uint64_t id, owner;
    size_t ordinal, offset, length;
    uint8_t lengths[RUN_CHARACTERS];
    uint8_t words[RUN_CHARACTERS];
    size_t characters, word_count;
};

struct text_model {
    struct text_run *runs;
    int count, capacity;
};

struct provider_window {
    struct provider_window *next;
    uintptr_t token;
    struct yasl_surface *surface;
    ui_accessibility_action_sink_fn action_sink;
    void *action_userdata;
    struct ui_accessibility_tree snapshot;
    struct text_model text;
    uint64_t activation_generation, published_generation;
#ifdef __APPLE__
    accesskit_macos_subclassing_adapter *adapter;
#else
    accesskit_unix_adapter *adapter;
#endif
    bool active, eager, popup, focus_known, window_focused;
};

/* Unix adapter removal is asynchronous. Late callbacks use a token, not a
 * pointer to storage that the window can release. */
static pthread_mutex_t providers_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct provider_window *providers;
static uintptr_t next_token;

static struct provider_window *provider_for_token(void *userdata)
{
    uintptr_t token = (uintptr_t)userdata;
    for (struct provider_window *window = providers; window; window = window->next)
        if (window->token == token)
            return window;
    return NULL;
}

static accesskit_role role_for_node(const struct ui_accessibility_node *node)
{
    switch (node->role) {
    case UI_ACCESSIBILITY_ROLE_WINDOW: return ACCESSKIT_ROLE_WINDOW;
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
        if (node->states & UI_ACCESSIBILITY_STATE_PASSWORD)
            return ACCESSKIT_ROLE_PASSWORD_INPUT;
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
    case UI_ACCESSIBILITY_ROLE_COMBO_BOX:
        return node->states & UI_ACCESSIBILITY_STATE_EDITABLE
            ? ACCESSKIT_ROLE_EDITABLE_COMBO_BOX : ACCESSKIT_ROLE_COMBO_BOX;
    default: return ACCESSKIT_ROLE_UNKNOWN;
    }
}

static bool has_text(const struct ui_accessibility_node *node)
{
    return !(node->states & UI_ACCESSIBILITY_STATE_PASSWORD) &&
           (node->role == UI_ACCESSIBILITY_ROLE_TEXT_FIELD ||
            node->role == UI_ACCESSIBILITY_ROLE_SEARCH_FIELD ||
            node->role == UI_ACCESSIBILITY_ROLE_TERMINAL ||
            (node->actions & UI_ACCESSIBILITY_ACTION_SET_SELECTION));
}

static struct text_run *new_run(struct text_model *model,
                                const struct text_model *previous,
                                const struct ui_accessibility_tree *snapshot,
                                uint64_t owner, size_t ordinal, size_t offset)
{
    if (model->count == INT_MAX ||
        !UTIL_GROW(model->runs, model->capacity, model->count + 1))
        return NULL;
    struct text_run *run = &model->runs[model->count++];
    *run = (struct text_run){ .owner = owner, .ordinal = ordinal, .offset = offset };
    for (int i = 0; i < previous->count; i++) {
        if (previous->runs[i].owner == owner && previous->runs[i].ordinal == ordinal) {
            run->id = previous->runs[i].id;
            break;
        }
    }
    if (run->id && ui_accessibility_tree_find(snapshot, run->id))
        run->id = 0;
    while (!run->id) {
        run->id = ui_widget_accessibility_id_allocate();
        if (ui_accessibility_tree_find(snapshot, run->id))
            run->id = 0;
    }
    return run;
}

static const struct text_run *run_for_position(const struct text_model *model,
                                              uint64_t owner, size_t offset)
{
    const struct text_run *last = NULL;
    for (int i = 0; i < model->count; i++) {
        const struct text_run *run = &model->runs[i];
        if (run->owner != owner)
            continue;
        last = run;
        if (offset < run->offset + run->length)
            return run;
    }
    return last;
}

static accesskit_text_position text_position(const struct text_model *model,
                                             uint64_t owner, size_t offset)
{
    const struct text_run *run = run_for_position(model, owner, offset);
    if (!run)
        return (accesskit_text_position){0};
    size_t index = 0, byte = run->offset;
    while (index < run->characters && byte + run->lengths[index] <= offset)
        byte += run->lengths[index++];
    return (accesskit_text_position){ .node = run->id, .character_index = index };
}

static bool text_offset(const struct text_model *model, uint64_t owner,
                         accesskit_text_position position, size_t *offset)
{
    for (int i = 0; i < model->count; i++) {
        const struct text_run *run = &model->runs[i];
        if (run->id != position.node || run->owner != owner ||
            position.character_index > run->characters)
            continue;
        *offset = run->offset;
        for (size_t c = 0; c < position.character_index; c++)
            *offset += run->lengths[c];
        return true;
    }
    return false;
}

static UBreakIterator *text_iterator(UText *utext, const char *value,
                                     size_t length, UBreakIteratorType type)
{
    UErrorCode error = U_ZERO_ERROR;
    utext_openUTF8(utext, value, (int64_t)length, &error);
    UBreakIterator *iterator = ubrk_open(type, NULL, NULL, 0, &error);
    if (U_SUCCESS(error))
        ubrk_setUText(iterator, utext, &error);
    if (U_FAILURE(error)) {
        ubrk_close(iterator);
        return NULL;
    }
    return iterator;
}

static bool build_text(struct text_model *model,
                         const struct text_model *previous,
                         const struct ui_accessibility_tree *snapshot,
                         const struct ui_accessibility_node *node)
{
    const char *value = node->value ? node->value : "";
    size_t length = strlen(value), ordinal = 0;
    if (length > INT_MAX)
        return false;
    UText utext = UTEXT_INITIALIZER;
    UBreakIterator *iterator = text_iterator(&utext, value, length, UBRK_CHARACTER);
    if (!iterator) {
        utext_close(&utext);
        return false;
    }
    struct text_run *run = new_run(model, previous, snapshot, node->id, ordinal++, 0);
    bool ok = run != NULL;
    int32_t start = ubrk_first(iterator), end;
    while (ok && (end = ubrk_next(iterator)) != UBRK_DONE) {
        size_t off = (size_t)start;
        while (ok && off < (size_t)end) {
            size_t bytes = (size_t)end - off;
            /* AccessKit stores one character's byte count in a uint8_t. */
            if (bytes > UINT8_MAX) {
                uint32_t cp;
                bytes = (size_t)wsap_vtcore_utf8_decode_one(value + off,
                                                          length - off, &cp);
            }
            if (run->characters == RUN_CHARACTERS ||
                (run->length && value[run->offset + run->length - 1] == '\n'))
                run = new_run(model, previous, snapshot, node->id, ordinal++, off);
            if (!run) {
                ok = false;
                break;
            }
            run->lengths[run->characters++] = (uint8_t)bytes;
            run->length += bytes;
            off += bytes;
        }
        start = end;
    }
    ubrk_close(iterator);
    utext_close(&utext);
    if (!ok)
        return false;
    if (length && value[length - 1] == '\n' &&
        !new_run(model, previous, snapshot, node->id, ordinal, length))
        return false;

    utext = (UText)UTEXT_INITIALIZER;
    iterator = text_iterator(&utext, value, length, UBRK_WORD);
    if (!iterator) {
        utext_close(&utext);
        return false;
    }
    start = ubrk_first(iterator);
    while ((end = ubrk_next(iterator)) != UBRK_DONE) {
        if (ubrk_getRuleStatus(iterator) != UBRK_WORD_NONE) {
            accesskit_text_position pos = text_position(model, node->id, (size_t)start);
            for (int i = 0; i < model->count; i++) {
                run = &model->runs[i];
                if (run->id == pos.node && pos.character_index < RUN_CHARACTERS)
                    run->words[run->word_count++] = (uint8_t)pos.character_index;
            }
        }
        start = end;
    }
    ubrk_close(iterator);
    utext_close(&utext);
    return true;
}

static accesskit_rect node_bounds(const struct ui_accessibility_node *node)
{
    return (accesskit_rect){ .x0 = node->bounds.x, .y0 = node->bounds.y,
        .x1 = (double)node->bounds.x + node->bounds.width,
        .y1 = (double)node->bounds.y + node->bounds.height };
}

static accesskit_node *build_node(const struct provider_window *window,
                                  const struct ui_accessibility_node *source,
                                  const struct text_model *text)
{
    accesskit_node *node = accesskit_node_new(role_for_node(source));
    accesskit_node_set_bounds(node, node_bounds(source));
    if (source->name)
        accesskit_node_set_label(node, source->name);
    if (source->description)
        accesskit_node_set_description(node, source->description);
    if (source->value)
        accesskit_node_set_value(node, source->value);
    if (source->states & UI_ACCESSIBILITY_STATE_HIDDEN)
        accesskit_node_set_hidden(node);
    if (source->states & UI_ACCESSIBILITY_STATE_DISABLED)
        accesskit_node_set_disabled(node);
    if (!window->action_sink || (source->states & UI_ACCESSIBILITY_STATE_READ_ONLY))
        accesskit_node_set_read_only(node);
    if (source->states & UI_ACCESSIBILITY_STATE_REQUIRED)
        accesskit_node_set_required(node);
    if (source->states & UI_ACCESSIBILITY_STATE_BUSY)
        accesskit_node_set_busy(node);
    if (source->states & UI_ACCESSIBILITY_STATE_INVALID)
        accesskit_node_set_invalid(node, ACCESSKIT_INVALID_TRUE);
    if (source->role == UI_ACCESSIBILITY_ROLE_CHECKBOX ||
        source->role == UI_ACCESSIBILITY_ROLE_RADIO_BUTTON ||
        source->role == UI_ACCESSIBILITY_ROLE_TOGGLE_BUTTON)
        accesskit_node_set_toggled(node, source->states & UI_ACCESSIBILITY_STATE_CHECKED
            ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
    if ((source->states & UI_ACCESSIBILITY_STATE_SELECTED) ||
        source->role == UI_ACCESSIBILITY_ROLE_TAB ||
        source->role == UI_ACCESSIBILITY_ROLE_MENU_ITEM ||
        source->role == UI_ACCESSIBILITY_ROLE_LIST_ITEM)
        accesskit_node_set_selected(node, (source->states & UI_ACCESSIBILITY_STATE_SELECTED) != 0);
    if (source->role == UI_ACCESSIBILITY_ROLE_COMBO_BOX ||
        (source->states & UI_ACCESSIBILITY_STATE_EXPANDED))
        accesskit_node_set_expanded(node, (source->states & UI_ACCESSIBILITY_STATE_EXPANDED) != 0);

    if (window->action_sink) {
        static const struct { uint32_t ui; accesskit_action native; } actions[] = {
            { UI_ACCESSIBILITY_ACTION_INVOKE, ACCESSKIT_ACTION_CLICK },
            { UI_ACCESSIBILITY_ACTION_FOCUS, ACCESSKIT_ACTION_FOCUS },
            { UI_ACCESSIBILITY_ACTION_SET_VALUE, ACCESSKIT_ACTION_SET_VALUE },
            { UI_ACCESSIBILITY_ACTION_SET_SELECTION, ACCESSKIT_ACTION_SET_TEXT_SELECTION },
            { UI_ACCESSIBILITY_ACTION_INCREMENT, ACCESSKIT_ACTION_INCREMENT },
            { UI_ACCESSIBILITY_ACTION_DECREMENT, ACCESSKIT_ACTION_DECREMENT },
        };
        for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
            if (source->actions & actions[i].ui)
                accesskit_node_add_action(node, actions[i].native);
    }
    if (has_text(source)) {
        const char *value = source->value ? source->value : "";
        for (int i = 0; i < text->count; i++)
            if (text->runs[i].owner == source->id)
                accesskit_node_push_child(node, text->runs[i].id);
        size_t start = source->selection_start, end = source->selection_end;
        if (start == end)
            start = end = source->caret_offset;
        else if (source->caret_offset == start) {
            size_t swap = start;
            start = end;
            end = swap;
        }
        size_t length = strlen(value);
        accesskit_node_set_text_selection(node, (accesskit_text_selection){
            .anchor = text_position(text, source->id, start < length ? start : length),
            .focus = text_position(text, source->id, end < length ? end : length),
        });
    }
    return node;
}

struct child_ref {
    uint64_t parent, id;
    uint32_t order;
};

static int compare_children(const void *a, const void *b)
{
    const struct child_ref *left = a, *right = b;
    if (left->parent != right->parent)
        return left->parent < right->parent ? -1 : 1;
    if (left->order != right->order)
        return left->order < right->order ? -1 : 1;
    return left->id < right->id ? -1 : left->id > right->id;
}

static accesskit_tree_update *build_update(const struct provider_window *window,
                                           const struct ui_accessibility_tree *snapshot,
                                           const struct text_model *text)
{
    if (!snapshot->count)
        return NULL;
    struct child_ref *children = calloc(snapshot->count, sizeof(*children));
    if (!children)
        return NULL;
    uint64_t root = snapshot->nodes[0].id, focus = root;
    for (size_t i = 0; i < snapshot->count; i++) {
        const struct ui_accessibility_node *node = &snapshot->nodes[i];
        children[i] = (struct child_ref){ .parent = node->parent_id,
                                         .id = node->id, .order = node->order };
        if (node->states & UI_ACCESSIBILITY_STATE_FOCUSED)
            focus = node->id;
    }
    qsort(children, snapshot->count, sizeof(*children), compare_children);
    accesskit_tree_update *update = accesskit_tree_update_with_capacity_and_focus(
        snapshot->count + (size_t)text->count, focus);
    accesskit_tree_info *info = accesskit_tree_info_new(root);
    accesskit_tree_info_set_toolkit_name(info, "wsap-ui");
    accesskit_tree_update_set_tree_info(update, info);
    for (size_t i = 0; i < snapshot->count; i++) {
        const struct ui_accessibility_node *source = &snapshot->nodes[i];
        accesskit_node *node = build_node(window, source, text);
        size_t low = 0, high = snapshot->count;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (children[mid].parent < source->id)
                low = mid + 1;
            else
                high = mid;
        }
        while (low < snapshot->count && children[low].parent == source->id)
            accesskit_node_push_child(node, children[low++].id);
        accesskit_tree_update_push_node(update, source->id, node);
    }
    for (int i = 0; i < text->count; i++) {
        const struct text_run *run = &text->runs[i];
        const struct ui_accessibility_node *source = ui_accessibility_tree_find(snapshot, run->owner);
        accesskit_node *node = accesskit_node_new(ACCESSKIT_ROLE_TEXT_RUN);
        const char *value = source->value ? source->value : "";
        accesskit_node_set_value_with_length(node, value + run->offset, run->length);
        accesskit_node_set_character_lengths(node, run->characters, run->lengths);
        accesskit_node_set_word_starts(node, run->word_count, run->words);
        accesskit_node_set_bounds(node, node_bounds(source));
        accesskit_node_set_text_direction(node, ACCESSKIT_TEXT_DIRECTION_LEFT_TO_RIGHT);
        accesskit_tree_update_push_node(update, run->id, node);
    }
    free(children);
    return update;
}

static accesskit_tree_update *activate(void *userdata)
{
    pthread_mutex_lock(&providers_mutex);
    struct provider_window *window = provider_for_token(userdata);
    if (window) {
        window->active = true;
        window->activation_generation++;
        if (window->action_sink)
            window->action_sink(window->action_userdata,
                                 &(struct ui_accessibility_action_request){0});
    }
    pthread_mutex_unlock(&providers_mutex);
    return NULL;
}

#ifndef __APPLE__
static void deactivate(void *userdata)
{
    pthread_mutex_lock(&providers_mutex);
    struct provider_window *window = provider_for_token(userdata);
    if (window)
        window->active = false;
    pthread_mutex_unlock(&providers_mutex);
}
#endif

static void perform_action(accesskit_action_request *native, void *userdata)
{
    pthread_mutex_lock(&providers_mutex);
    struct provider_window *window = provider_for_token(userdata);
    if (!window || !window->action_sink)
        goto done;
    const struct ui_accessibility_node *node =
        ui_accessibility_tree_find(&window->snapshot, native->target_node);
    if (!node || (node->states & (UI_ACCESSIBILITY_STATE_DISABLED | UI_ACCESSIBILITY_STATE_HIDDEN)))
        goto done;
    struct ui_accessibility_action_request request = { .node_id = node->id };
    switch (native->action) {
    case ACCESSKIT_ACTION_CLICK: request.action = UI_ACCESSIBILITY_ACTION_INVOKE; break;
    case ACCESSKIT_ACTION_FOCUS: request.action = UI_ACCESSIBILITY_ACTION_FOCUS; break;
    case ACCESSKIT_ACTION_INCREMENT: request.action = UI_ACCESSIBILITY_ACTION_INCREMENT; break;
    case ACCESSKIT_ACTION_DECREMENT: request.action = UI_ACCESSIBILITY_ACTION_DECREMENT; break;
    case ACCESSKIT_ACTION_SET_VALUE:
        if (!native->data.has_value || native->data.value.tag != ACCESSKIT_ACTION_DATA_VALUE)
            goto done;
        request.action = UI_ACCESSIBILITY_ACTION_SET_VALUE;
        request.value = native->data.value.value;
        request.caret_offset = SIZE_MAX;
        break;
    case ACCESSKIT_ACTION_SET_TEXT_SELECTION: {
        if (!native->data.has_value || native->data.value.tag != ACCESSKIT_ACTION_DATA_SET_TEXT_SELECTION)
            goto done;
        accesskit_text_selection selection = native->data.value.set_text_selection;
        size_t anchor, focus;
        if (!text_offset(&window->text, node->id, selection.anchor, &anchor) ||
            !text_offset(&window->text, node->id, selection.focus, &focus))
            goto done;
        request.action = UI_ACCESSIBILITY_ACTION_SET_SELECTION;
        request.selection_start = anchor < focus ? anchor : focus;
        request.selection_end = anchor < focus ? focus : anchor;
        request.caret_offset = focus;
        break;
    }
    default: goto done;
    }
    if (node->actions & request.action)
        window->action_sink(window->action_userdata, &request);
done:
    pthread_mutex_unlock(&providers_mutex);
    accesskit_action_request_free(native);
}

static void *provider_attach(void *native_handle,
                             ui_accessibility_action_sink_fn sink, void *userdata)
{
    if (!native_handle)
        return NULL;
    struct provider_window *window = calloc(1, sizeof(*window));
    if (!window)
        return NULL;
    window->surface = native_handle;
    window->action_sink = sink;
    window->action_userdata = userdata;
    const char *eager = getenv("WSAP_ACCESSIBILITY");
    window->eager = eager && strcmp(eager, "1") == 0;
    pthread_mutex_lock(&providers_mutex);
    do { window->token = ++next_token; } while (!window->token);
    window->next = providers;
    providers = window;
    pthread_mutex_unlock(&providers_mutex);
#ifdef __APPLE__
    struct yasl_native native = {0};
    if (yasl_surface_get_native(window->surface, &native) == 0 && native.ns_view)
        window->adapter = accesskit_macos_subclassing_adapter_new(native.ns_view,
            activate, (void *)window->token, perform_action, (void *)window->token);
#else
    window->adapter = accesskit_unix_adapter_new(activate, (void *)window->token,
        perform_action, (void *)window->token, deactivate, (void *)window->token);
#endif
    if (!window->adapter) {
        pthread_mutex_lock(&providers_mutex);
        struct provider_window **link = &providers;
        while (*link != window)
            link = &(*link)->next;
        *link = window->next;
        pthread_mutex_unlock(&providers_mutex);
        free(window);
        return NULL;
    }
    return window;
}

static bool copy_snapshot(struct ui_accessibility_tree *copy,
                           const struct ui_accessibility_tree *snapshot)
{
    for (size_t i = 0; i < snapshot->count; i++) {
        const struct ui_accessibility_node *node = &snapshot->nodes[i];
        if (!ui_accessibility_tree_add(copy, &(struct ui_accessibility_node_spec){
                .id = node->id, .parent_id = node->parent_id, .order = node->order,
                .role = node->role, .states = node->states, .actions = node->actions,
                .bounds = node->bounds, .name = node->name, .description = node->description,
                .value = node->value, .caret_offset = node->caret_offset,
                .selection_start = node->selection_start, .selection_end = node->selection_end }))
            return false;
    }
    return true;
}

static accesskit_tree_update *take_update(void *userdata)
{
    accesskit_tree_update **update = userdata;
    accesskit_tree_update *result = *update;
    *update = NULL;
    return result;
}

static bool same_string(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

static bool same_snapshot(const struct ui_accessibility_tree *a,
                           const struct ui_accessibility_tree *b)
{
    if (a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; i++) {
        const struct ui_accessibility_node *left = &a->nodes[i], *right = &b->nodes[i];
        if (left->id != right->id || left->parent_id != right->parent_id ||
            left->order != right->order || left->role != right->role ||
            left->states != right->states || left->actions != right->actions ||
            !rect2i_eq(left->bounds, right->bounds) ||
            left->caret_offset != right->caret_offset ||
            left->selection_start != right->selection_start ||
            left->selection_end != right->selection_end ||
            !same_string(left->name, right->name) ||
            !same_string(left->description, right->description) ||
            !same_string(left->value, right->value))
            return false;
    }
    return true;
}

static void update_focus(struct provider_window *window)
{
    struct yasl_surface_info info = yasl_surface_get_info(window->surface);
    bool focused = window->popup || (info.state & YASL_STATE_ACTIVATED);
    if (window->focus_known && window->window_focused == focused)
        return;
    window->focus_known = true;
    window->window_focused = focused;
#ifdef __APPLE__
    accesskit_macos_queued_events *events =
        accesskit_macos_subclassing_adapter_update_view_focus_state(window->adapter, focused);
    if (events)
        accesskit_macos_queued_events_raise(events);
#else
    accesskit_unix_adapter_update_window_focus_state(window->adapter, focused);
#endif
}

static bool provider_publish(void *instance,
                              const struct ui_accessibility_tree *snapshot)
{
    struct provider_window *window = instance;
    if (!window || !snapshot || !snapshot->count)
        return false;
    pthread_mutex_lock(&providers_mutex);
    uint64_t generation = window->activation_generation;
    bool unchanged = generation == window->published_generation &&
                     same_snapshot(&window->snapshot, snapshot);
    pthread_mutex_unlock(&providers_mutex);
    if (unchanged) {
        update_focus(window);
        return true;
    }
    struct ui_accessibility_tree copy = {0};
    struct text_model text = {0};
    if (!copy_snapshot(&copy, snapshot))
        goto fail;
    for (size_t i = 0; i < copy.count; i++)
        if (has_text(&copy.nodes[i]) &&
            !build_text(&text, &window->text, &copy, &copy.nodes[i]))
            goto fail;
    accesskit_tree_update *update = build_update(window, &copy, &text);
    if (!update)
        goto fail;
    pthread_mutex_lock(&providers_mutex);
    struct ui_accessibility_tree old_snapshot = window->snapshot;
    struct text_model old_text = window->text;
    window->snapshot = copy;
    window->text = text;
    pthread_mutex_unlock(&providers_mutex);
    ui_accessibility_tree_destroy(&old_snapshot);
    free(old_text.runs);
#ifdef __APPLE__
    accesskit_macos_queued_events *events =
        accesskit_macos_subclassing_adapter_update_if_active(window->adapter, take_update, &update);
    if (events)
        accesskit_macos_queued_events_raise(events);
#else
    accesskit_unix_adapter_update_if_active(window->adapter, take_update, &update);
#endif
    if (!update) {
        pthread_mutex_lock(&providers_mutex);
        window->published_generation = generation;
        pthread_mutex_unlock(&providers_mutex);
    }
    if (update)
        accesskit_tree_update_free(update);
    update_focus(window);
    return true;
fail:
    ui_accessibility_tree_destroy(&copy);
    free(text.runs);
    return false;
}

static void provider_detach(void *instance)
{
    struct provider_window *window = instance;
    if (!window)
        return;
    pthread_mutex_lock(&providers_mutex);
    struct provider_window **link = &providers;
    while (*link && *link != window)
        link = &(*link)->next;
    if (*link)
        *link = window->next;
    pthread_mutex_unlock(&providers_mutex);
#ifdef __APPLE__
    accesskit_macos_subclassing_adapter_free(window->adapter);
#else
    accesskit_unix_adapter_free(window->adapter);
#endif
    ui_accessibility_tree_destroy(&window->snapshot);
    free(window->text.runs);
    free(window);
}

static bool provider_is_active(void *instance)
{
    struct provider_window *window = instance;
    pthread_mutex_lock(&providers_mutex);
    bool active = window && (window->active || window->eager);
    pthread_mutex_unlock(&providers_mutex);
    return active;
}

static const struct ui_accessibility_provider_ops provider_ops = {
    .attach = provider_attach,
    .publish_snapshot = provider_publish,
    .detach = provider_detach,
    .is_active = provider_is_active,
};

const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void)
{
    return &provider_ops;
}

void *ui_accessibility_native_handle(struct yasl_surface *surface)
{
    return surface;
}

void ui_accessibility_set_menu_popup_depth(
    const struct ui_accessibility_provider *provider, int depth)
{
    if (provider && provider->ops == &provider_ops && provider->instance)
        ((struct provider_window *)provider->instance)->popup = depth >= 0;
}
