#define _GNU_SOURCE

#include "ui/yasl/accessibility/accessibility.h"

#include "ui/text_break.h"
#include "ui/widget_impl.h"
#include "util/grow.h"
#include "wsap_vtcore/unicode/utf8.h"
#include "yasl/yasl.h"

#include <accesskit.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

struct build_grapheme_ctx {
    struct text_model *model;
    const struct text_model *previous;
    const struct ui_accessibility_tree *snapshot;
    uint64_t node_id;
    const char *value;
    size_t length;
    struct text_run *run;
    size_t ordinal;
    bool ok;
};

static bool build_grapheme_cb(size_t start, size_t end, void *userdata)
{
    struct build_grapheme_ctx *ctx = userdata;
    size_t off = start;
    while (ctx->ok && off < end) {
        size_t bytes = end - off;
        /* AccessKit stores one character's byte count in a uint8_t. */
        if (bytes > UINT8_MAX) {
            uint32_t cp;
            bytes = (size_t)wsap_vtcore_utf8_decode_one(ctx->value + off,
                                                       ctx->length - off, &cp);
        }
        if (ctx->run->characters == RUN_CHARACTERS ||
            (ctx->run->length &&
             ctx->value[ctx->run->offset + ctx->run->length - 1] == '\n'))
            ctx->run = new_run(ctx->model, ctx->previous, ctx->snapshot,
                               ctx->node_id, ctx->ordinal++, off);
        if (!ctx->run) {
            ctx->ok = false;
            return false;
        }
        ctx->run->lengths[ctx->run->characters++] = (uint8_t)bytes;
        ctx->run->length += bytes;
        off += bytes;
    }
    return ctx->ok;
}

struct build_word_ctx {
    struct text_model *model;
    uint64_t node_id;
};

static bool build_word_cb(size_t start, size_t end, bool is_word, void *userdata)
{
    (void)end;
    if (!is_word)
        return true;
    struct build_word_ctx *ctx = userdata;
    accesskit_text_position pos = text_position(ctx->model, ctx->node_id, start);
    for (int i = 0; i < ctx->model->count; i++) {
        struct text_run *run = &ctx->model->runs[i];
        if (run->id == pos.node && pos.character_index < RUN_CHARACTERS &&
            run->word_count < RUN_CHARACTERS)
            run->words[run->word_count++] = (uint8_t)pos.character_index;
    }
    return true;
}

static bool build_text(struct text_model *model,
                         const struct text_model *previous,
                         const struct ui_accessibility_tree *snapshot,
                         const struct ui_accessibility_node *node)
{
    const char *value = node->value ? node->value : "";
    size_t length = strlen(value);
    if (length > INT_MAX)
        return false;
    struct text_run *run = new_run(model, previous, snapshot, node->id, 0, 0);
    if (!run)
        return false;
    struct build_grapheme_ctx gctx = {
        .model = model,
        .previous = previous,
        .snapshot = snapshot,
        .node_id = node->id,
        .value = value,
        .length = length,
        .run = run,
        .ordinal = 1,
        .ok = true,
    };
    ui_text_each_grapheme(value, length, build_grapheme_cb, &gctx);
    if (!gctx.ok)
        return false;
    if (length && value[length - 1] == '\n' &&
        !new_run(model, previous, snapshot, node->id, gctx.ordinal, length))
        return false;

    struct build_word_ctx wctx = {
        .model = model,
        .node_id = node->id,
    };
    ui_text_each_word(value, length, build_word_cb, &wctx);
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

static void update_focus(struct provider_window *window);

#if defined(__linux__)
typedef uint32_t dbus_bool_t;
typedef struct dbus_connection dbus_connection;
typedef struct dbus_message dbus_message;
typedef struct dbus_watch dbus_watch;
typedef struct dbus_timeout dbus_timeout;
typedef struct dbus_pending_call dbus_pending_call;

typedef struct dbus_error {
    const char *name;
    const char *message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void *padding1;
} dbus_error;

typedef struct dbus_message_iter {
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ > 8
    void *dummy[16];
#else
    void *dummy1;
    void *dummy2;
    uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    void *pad2;
    void *pad3;
#endif
} dbus_message_iter;

enum {
    DBUS_WATCH_READABLE = 1 << 0,
    DBUS_WATCH_WRITABLE = 1 << 1,
    DBUS_WATCH_ERROR = 1 << 2,
    DBUS_WATCH_HANGUP = 1 << 3,
    DBUS_DISPATCH_DATA_REMAINS = 0,
    DBUS_TYPE_INVALID = '\0',
    DBUS_TYPE_BOOLEAN = 'b',
    DBUS_TYPE_STRING = 's',
    DBUS_TYPE_ARRAY = 'a',
    DBUS_TYPE_VARIANT = 'v',
    DBUS_TYPE_DICT_ENTRY = 'e',
    DBUS_MESSAGE_TYPE_METHOD_RETURN = 2,
    DBUS_MESSAGE_TYPE_ERROR = 3,
    DBUS_HANDLER_RESULT_HANDLED = 0,
    DBUS_HANDLER_RESULT_NOT_YET_HANDLED = 1,
    DBUS_TIMEOUT_USE_DEFAULT = -1,
};

typedef void (*dbus_free_fn)(void *memory);
typedef dbus_bool_t (*dbus_add_watch_fn)(dbus_watch *watch, void *data);
typedef void (*dbus_remove_watch_fn)(dbus_watch *watch, void *data);
typedef void (*dbus_watch_toggled_fn)(dbus_watch *watch, void *data);
typedef dbus_bool_t (*dbus_add_timeout_fn)(dbus_timeout *timeout, void *data);
typedef void (*dbus_remove_timeout_fn)(dbus_timeout *timeout, void *data);
typedef void (*dbus_timeout_toggled_fn)(dbus_timeout *timeout, void *data);
typedef int (*dbus_filter_fn)(dbus_connection *conn, dbus_message *msg,
                              void *data);
typedef void (*dbus_pending_notify_fn)(dbus_pending_call *pending, void *data);

struct dbus_api {
    void (*error_init)(dbus_error *);
    void (*error_free)(dbus_error *);
    dbus_bool_t (*error_is_set)(const dbus_error *);
    dbus_connection *(*connection_open_private)(const char *, dbus_error *);
    void (*connection_close)(dbus_connection *);
    void (*connection_unref)(dbus_connection *);
    void (*connection_set_exit_on_disconnect)(dbus_connection *, dbus_bool_t);
    dbus_bool_t (*connection_set_watch_functions)(
        dbus_connection *, dbus_add_watch_fn, dbus_remove_watch_fn,
        dbus_watch_toggled_fn, void *, dbus_free_fn);
    dbus_bool_t (*connection_set_timeout_functions)(
        dbus_connection *, dbus_add_timeout_fn, dbus_remove_timeout_fn,
        dbus_timeout_toggled_fn, void *, dbus_free_fn);
    dbus_bool_t (*connection_add_filter)(dbus_connection *, dbus_filter_fn,
                                         void *, dbus_free_fn);
    void (*connection_remove_filter)(dbus_connection *, dbus_filter_fn,
                                     void *);
    int (*connection_get_dispatch_status)(dbus_connection *);
    int (*connection_dispatch)(dbus_connection *);
    dbus_bool_t (*connection_send_with_reply)(
        dbus_connection *, dbus_message *, dbus_pending_call **, int);
    void (*connection_flush)(dbus_connection *);
    int (*watch_get_unix_fd)(dbus_watch *);
    unsigned int (*watch_get_flags)(dbus_watch *);
    dbus_bool_t (*watch_get_enabled)(dbus_watch *);
    void *(*watch_get_data)(dbus_watch *);
    void (*watch_set_data)(dbus_watch *, void *, dbus_free_fn);
    dbus_bool_t (*watch_handle)(dbus_watch *, unsigned int);
    int (*timeout_get_interval)(dbus_timeout *);
    dbus_bool_t (*timeout_get_enabled)(dbus_timeout *);
    void *(*timeout_get_data)(dbus_timeout *);
    void (*timeout_set_data)(dbus_timeout *, void *, dbus_free_fn);
    dbus_bool_t (*timeout_handle)(dbus_timeout *);
    dbus_message *(*message_new_method_call)(const char *, const char *,
                                             const char *, const char *);
    void (*message_unref)(dbus_message *);
    void (*message_set_auto_start)(dbus_message *, dbus_bool_t);
    dbus_bool_t (*message_append_args)(dbus_message *, int, ...);
    int (*message_get_type)(dbus_message *);
    dbus_bool_t (*message_is_signal)(dbus_message *, const char *,
                                     const char *);
    const char *(*message_get_path)(dbus_message *);
    const char *(*message_get_error_name)(dbus_message *);
    dbus_bool_t (*message_iter_init)(dbus_message *, dbus_message_iter *);
    dbus_bool_t (*message_iter_next)(dbus_message_iter *);
    int (*message_iter_get_arg_type)(dbus_message_iter *);
    void (*message_iter_recurse)(dbus_message_iter *, dbus_message_iter *);
    void (*message_iter_get_basic)(dbus_message_iter *, void *);
    dbus_bool_t (*pending_call_set_notify)(
        dbus_pending_call *, dbus_pending_notify_fn, void *, dbus_free_fn);
    dbus_message *(*pending_call_steal_reply)(dbus_pending_call *);
    void (*pending_call_cancel)(dbus_pending_call *);
    void (*pending_call_unref)(dbus_pending_call *);
    dbus_bool_t (*bus_set_unique_name)(dbus_connection *, const char *);
    void (*bus_add_match)(dbus_connection *, const char *, dbus_error *);
};

enum status_prop {
    PROP_IS_ENABLED = 0,
    PROP_SCREEN_READER = 1,
};

struct dbus_watch_item {
    struct dbus_watch_item *next;
    struct a11y_dbus_monitor *m;
    dbus_watch *watch;
    int fd_id;
    short events;
    int refcount;
    bool removed;
};

struct dbus_timeout_item {
    struct dbus_timeout_item *next;
    struct a11y_dbus_monitor *m;
    dbus_timeout *timeout;
    uint64_t timer_id;
    int refcount;
    bool removed;
};

struct a11y_dbus_monitor {
    struct yasl_loop *loop;
    void *lib;
    struct dbus_api api;
    dbus_connection *conn;
    dbus_pending_call *hello_call;
    dbus_pending_call *status_call;
    struct dbus_watch_item *watches;
    struct dbus_timeout_item *timeouts;
    enum status_prop pending_prop;
    bool started;
    bool registered;
    bool filter_added;
    bool enabled;
    bool dispatching;
    bool disconnected;
};

static struct a11y_dbus_monitor a11y_monitor;

static void monitor_stop(void);
static void monitor_query_status(struct a11y_dbus_monitor *m,
                                 enum status_prop prop);

static bool is_dbus_path_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '/' ||
           c == '.';
}

static char *escape_dbus_path(const char *path)
{
    static const char hex[] = "0123456789abcdef";
    size_t len = 0;
    for (const char *p = path; *p; p++)
        len += is_dbus_path_byte((unsigned char)*p) ? 1 : 3;
    char *out = malloc(sizeof("unix:path=") + len);
    if (!out)
        return NULL;
    memcpy(out, "unix:path=", sizeof("unix:path=") - 1);
    char *dst = out + sizeof("unix:path=") - 1;
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (is_dbus_path_byte(c)) {
            *dst++ = (char)c;
        } else {
            *dst++ = '%';
            *dst++ = hex[c >> 4];
            *dst++ = hex[c & 0x0f];
        }
    }
    *dst = '\0';
    return out;
}

static char *resolve_bus_address(void)
{
    const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env)
        return env[0] ? strdup(env) : NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/')
        return NULL;
    size_t len = strlen(runtime) + sizeof("/bus");
    char *sock = malloc(len);
    if (!sock)
        return NULL;
    snprintf(sock, len, "%s/bus", runtime);
    struct stat st;
    bool ok = stat(sock, &st) == 0 && S_ISSOCK(st.st_mode);
    char *addr = ok ? escape_dbus_path(sock) : NULL;
    free(sock);
    return addr;
}

static bool load_dbus_api(struct a11y_dbus_monitor *m)
{
#define DBUS_SYM(field) \
    { .name = "dbus_" #field, .offset = offsetof(struct dbus_api, field) }
    static const struct {
        const char *name;
        size_t offset;
    } syms[] = {
        DBUS_SYM(error_init),
        DBUS_SYM(error_free),
        DBUS_SYM(error_is_set),
        DBUS_SYM(connection_open_private),
        DBUS_SYM(connection_close),
        DBUS_SYM(connection_unref),
        DBUS_SYM(connection_set_exit_on_disconnect),
        DBUS_SYM(connection_set_watch_functions),
        DBUS_SYM(connection_set_timeout_functions),
        DBUS_SYM(connection_add_filter),
        DBUS_SYM(connection_remove_filter),
        DBUS_SYM(connection_get_dispatch_status),
        DBUS_SYM(connection_dispatch),
        DBUS_SYM(connection_send_with_reply),
        DBUS_SYM(connection_flush),
        DBUS_SYM(watch_get_unix_fd),
        DBUS_SYM(watch_get_flags),
        DBUS_SYM(watch_get_enabled),
        DBUS_SYM(watch_get_data),
        DBUS_SYM(watch_set_data),
        DBUS_SYM(watch_handle),
        DBUS_SYM(timeout_get_interval),
        DBUS_SYM(timeout_get_enabled),
        DBUS_SYM(timeout_get_data),
        DBUS_SYM(timeout_set_data),
        DBUS_SYM(timeout_handle),
        DBUS_SYM(message_new_method_call),
        DBUS_SYM(message_unref),
        DBUS_SYM(message_set_auto_start),
        DBUS_SYM(message_append_args),
        DBUS_SYM(message_get_type),
        DBUS_SYM(message_is_signal),
        DBUS_SYM(message_get_path),
        DBUS_SYM(message_get_error_name),
        DBUS_SYM(message_iter_init),
        DBUS_SYM(message_iter_next),
        DBUS_SYM(message_iter_get_arg_type),
        DBUS_SYM(message_iter_recurse),
        DBUS_SYM(message_iter_get_basic),
        DBUS_SYM(pending_call_set_notify),
        DBUS_SYM(pending_call_steal_reply),
        DBUS_SYM(pending_call_cancel),
        DBUS_SYM(pending_call_unref),
        DBUS_SYM(bus_set_unique_name),
        DBUS_SYM(bus_add_match),
    };
#undef DBUS_SYM

    void *lib = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        return false;
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        void *fn = dlsym(lib, syms[i].name);
        if (!fn) {
            dlclose(lib);
            return false;
        }
        memcpy((char *)&m->api + syms[i].offset, &fn, sizeof(fn));
    }
    m->lib = lib;
    return true;
}

static void cancel_pending(struct a11y_dbus_monitor *m,
                           dbus_pending_call **slot)
{
    if (!*slot)
        return;
    dbus_pending_call *call = *slot;
    *slot = NULL;
    m->api.pending_call_cancel(call);
    m->api.pending_call_unref(call);
}

static void activate_live_providers(void)
{
    pthread_mutex_lock(&providers_mutex);
    size_t count = 0;
    for (struct provider_window *w = providers; w; w = w->next)
        count++;
    uintptr_t *tokens = count ? calloc(count, sizeof(*tokens)) : NULL;
    if (tokens) {
        size_t i = 0;
        for (struct provider_window *w = providers; w; w = w->next)
            tokens[i++] = w->token;
    }
    pthread_mutex_unlock(&providers_mutex);
    if (!tokens)
        return;

    for (size_t i = 0; i < count; i++) {
        void *token = (void *)tokens[i];
        pthread_mutex_lock(&providers_mutex);
        struct provider_window *w = provider_for_token(token);
        bool needs_adapter = w && !w->adapter;
        pthread_mutex_unlock(&providers_mutex);
        if (!w)
            continue;

        if (needs_adapter) {
            accesskit_unix_adapter *adapter = accesskit_unix_adapter_new(
                activate, token, perform_action, token, deactivate, token);
            if (!adapter)
                continue;
            pthread_mutex_lock(&providers_mutex);
            w = provider_for_token(token);
            if (w && !w->adapter) {
                w->adapter = adapter;
                adapter = NULL;
            }
            pthread_mutex_unlock(&providers_mutex);
            if (adapter) {
                accesskit_unix_adapter_free(adapter);
                continue;
            }
        }

        pthread_mutex_lock(&providers_mutex);
        w = provider_for_token(token);
        ui_accessibility_action_sink_fn sink = NULL;
        void *userdata = NULL;
        bool ready = w && w->adapter;
        if (ready) {
            w->active = true;
            w->activation_generation++;
            sink = w->action_sink;
            userdata = w->action_userdata;
        }
        pthread_mutex_unlock(&providers_mutex);
        if (!ready)
            continue;

        update_focus(w);
        if (sink)
            sink(userdata, &(struct ui_accessibility_action_request){0});
    }
    free(tokens);
}

static void monitor_set_enabled(struct a11y_dbus_monitor *m, bool enabled)
{
    bool was_enabled = m->enabled;
    m->enabled = enabled;
    if (enabled)
        cancel_pending(m, &m->status_call);
    if (enabled && !was_enabled)
        activate_live_providers();
}

static void monitor_pump(struct a11y_dbus_monitor *m)
{
    if (m->dispatching || !m->conn)
        return;
    m->dispatching = true;
    while (m->conn && !m->disconnected &&
           m->api.connection_get_dispatch_status(m->conn) ==
               DBUS_DISPATCH_DATA_REMAINS)
        m->api.connection_dispatch(m->conn);
    if (m->conn && !m->disconnected)
        m->api.connection_flush(m->conn);
    m->dispatching = false;
    /* Stop only after dispatch unwinds. */
    if (m->disconnected)
        monitor_stop();
}

static void watch_item_unref(struct dbus_watch_item *item)
{
    if (item && --item->refcount == 0)
        free(item);
}

static void watch_free_data(void *data)
{
    watch_item_unref(data);
}

static void watch_ready(void *userdata, int fd, short revents)
{
    (void)fd;
    struct dbus_watch_item *item = userdata;
    if (!item || item->removed || !item->m || !item->watch)
        return;
    struct a11y_dbus_monitor *m = item->m;
    unsigned int flags = 0;
    if (revents & POLLIN)
        flags |= DBUS_WATCH_READABLE;
    if (revents & POLLOUT)
        flags |= DBUS_WATCH_WRITABLE;
    if (revents & POLLERR)
        flags |= DBUS_WATCH_ERROR;
    if (revents & POLLHUP)
        flags |= DBUS_WATCH_HANGUP;
    item->refcount++;
    m->api.watch_handle(item->watch, flags);
    watch_item_unref(item);
    monitor_pump(m);
}

static bool watch_update(struct dbus_watch_item *item)
{
    struct a11y_dbus_monitor *m = item->m;
    if (!m || !m->loop || item->removed || !item->watch)
        return true;
    short events = 0;
    if (m->api.watch_get_enabled(item->watch)) {
        unsigned int flags = m->api.watch_get_flags(item->watch);
        if (flags & DBUS_WATCH_READABLE)
            events |= POLLIN;
        if (flags & DBUS_WATCH_WRITABLE)
            events |= POLLOUT;
    }
    if (!events) {
        if (item->fd_id > 0) {
            yasl_loop_del_fd(m->loop, item->fd_id);
            item->fd_id = 0;
            item->events = 0;
        }
        return true;
    }
    if (item->fd_id <= 0) {
        int fd = m->api.watch_get_unix_fd(item->watch);
        if (fd < 0)
            return false;
        int id = yasl_loop_add_fd(m->loop, fd, events, watch_ready, item);
        if (id <= 0)
            return false;
        item->fd_id = id;
        item->events = events;
        return true;
    }
    if (item->events != events) {
        yasl_loop_set_fd_events(m->loop, item->fd_id, events);
        item->events = events;
    }
    return true;
}

static dbus_bool_t watch_add(dbus_watch *watch, void *data)
{
    struct a11y_dbus_monitor *m = data;
    struct dbus_watch_item *item = calloc(1, sizeof(*item));
    if (!item)
        return 0;
    item->m = m;
    item->watch = watch;
    item->refcount = 1;
    item->next = m->watches;
    m->watches = item;
    m->api.watch_set_data(watch, item, watch_free_data);
    if (!watch_update(item)) {
        m->watches = item->next;
        item->removed = true;
        item->m = NULL;
        item->watch = NULL;
        m->api.watch_set_data(watch, NULL, NULL);
        return 0;
    }
    return 1;
}

static void watch_remove(dbus_watch *watch, void *data)
{
    struct a11y_dbus_monitor *m = data;
    struct dbus_watch_item *item = m->api.watch_get_data(watch);
    if (!item)
        return;
    item->removed = true;
    if (item->fd_id > 0 && m->loop) {
        yasl_loop_del_fd(m->loop, item->fd_id);
        item->fd_id = 0;
        item->events = 0;
    }
    struct dbus_watch_item **link = &m->watches;
    while (*link && *link != item)
        link = &(*link)->next;
    if (*link)
        *link = item->next;
    item->m = NULL;
    item->watch = NULL;
}

static void watch_toggled(dbus_watch *watch, void *data)
{
    struct a11y_dbus_monitor *m = data;
    struct dbus_watch_item *item = m->api.watch_get_data(watch);
    if (item)
        watch_update(item);
}

static void timeout_item_unref(struct dbus_timeout_item *item)
{
    if (item && --item->refcount == 0)
        free(item);
}

static void timeout_free_data(void *data)
{
    timeout_item_unref(data);
}

static bool timeout_update(struct dbus_timeout_item *item);

static uint64_t timeout_ready(void *userdata, uint64_t id, uint64_t delay_ns)
{
    (void)delay_ns;
    struct dbus_timeout_item *item = userdata;
    if (!item || item->removed || item->timer_id != id || !item->m ||
        !item->timeout)
        return 0;
    struct a11y_dbus_monitor *m = item->m;
    item->timer_id = 0;
    item->refcount++;
    m->api.timeout_handle(item->timeout);
    if (!item->removed && item->m == m && item->timeout)
        timeout_update(item);
    timeout_item_unref(item);
    monitor_pump(m);
    return 0;
}

static bool timeout_update(struct dbus_timeout_item *item)
{
    struct a11y_dbus_monitor *m = item->m;
    if (!m || !m->loop || item->removed || !item->timeout)
        return true;
    if (!m->api.timeout_get_enabled(item->timeout)) {
        if (item->timer_id) {
            yasl_loop_del_timer(m->loop, item->timer_id);
            item->timer_id = 0;
        }
        return true;
    }
    int ms = m->api.timeout_get_interval(item->timeout);
    uint64_t delay_ns = (uint64_t)(ms > 0 ? ms : 1) * 1000000ULL;
    if (item->timer_id)
        yasl_loop_del_timer(m->loop, item->timer_id);
    item->timer_id =
        yasl_loop_add_timer(m->loop, delay_ns, timeout_ready, item);
    return item->timer_id != 0;
}

static dbus_bool_t timeout_add(dbus_timeout *timeout, void *data)
{
    struct a11y_dbus_monitor *m = data;
    struct dbus_timeout_item *item = calloc(1, sizeof(*item));
    if (!item)
        return 0;
    item->m = m;
    item->timeout = timeout;
    item->refcount = 1;
    item->next = m->timeouts;
    m->timeouts = item;
    m->api.timeout_set_data(timeout, item, timeout_free_data);
    if (!timeout_update(item)) {
        m->timeouts = item->next;
        item->removed = true;
        item->m = NULL;
        item->timeout = NULL;
        m->api.timeout_set_data(timeout, NULL, NULL);
        return 0;
    }
    return 1;
}

static void timeout_remove(dbus_timeout *timeout, void *data)
{
    struct a11y_dbus_monitor *m = data;
    struct dbus_timeout_item *item = m->api.timeout_get_data(timeout);
    if (!item)
        return;
    item->removed = true;
    if (item->timer_id && m->loop) {
        yasl_loop_del_timer(m->loop, item->timer_id);
        item->timer_id = 0;
    }
    struct dbus_timeout_item **link = &m->timeouts;
    while (*link && *link != item)
        link = &(*link)->next;
    if (*link)
        *link = item->next;
    item->m = NULL;
    item->timeout = NULL;
}

static void timeout_toggled(dbus_timeout *timeout, void *data)
{
    struct a11y_dbus_monitor *m = data;
    struct dbus_timeout_item *item = m->api.timeout_get_data(timeout);
    if (item)
        timeout_update(item);
}

static bool iter_read_string(const struct dbus_api *api, dbus_message_iter *iter,
                             const char **out)
{
    if (api->message_iter_get_arg_type(iter) != DBUS_TYPE_STRING)
        return false;
    api->message_iter_get_basic(iter, out);
    return *out != NULL;
}

static bool iter_read_variant_bool(const struct dbus_api *api,
                                   dbus_message_iter *iter, bool *out)
{
    if (api->message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT)
        return false;
    dbus_message_iter sub;
    api->message_iter_recurse(iter, &sub);
    if (api->message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
        return false;
    dbus_bool_t value = 0;
    api->message_iter_get_basic(&sub, &value);
    *out = value != 0;
    return true;
}

static bool is_a11y_prop_name(const char *key)
{
    return key && (strcmp(key, "IsEnabled") == 0 ||
                   strcmp(key, "ScreenReaderEnabled") == 0);
}

static bool is_missing_owner_error(const char *name)
{
    return name &&
           (strcmp(name, "org.freedesktop.DBus.Error.NameHasNoOwner") == 0 ||
            strcmp(name, "org.freedesktop.DBus.Error.ServiceUnknown") == 0);
}

static void status_notify(dbus_pending_call *call, void *userdata)
{
    struct a11y_dbus_monitor *m = userdata;
    if (m->status_call != call)
        return;
    dbus_message *reply = m->api.pending_call_steal_reply(call);
    enum status_prop prop = m->pending_prop;
    m->status_call = NULL;
    m->api.pending_call_unref(call);
    if (!reply)
        return;

    int type = m->api.message_get_type(reply);
    bool enabled = false;
    bool got_value = false;
    bool missing_owner = false;
    if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        dbus_message_iter iter;
        if (m->api.message_iter_init(reply, &iter))
            got_value = iter_read_variant_bool(&m->api, &iter, &enabled);
    } else if (type == DBUS_MESSAGE_TYPE_ERROR) {
        missing_owner =
            is_missing_owner_error(m->api.message_get_error_name(reply));
    }
    m->api.message_unref(reply);

    if (got_value && enabled) {
        monitor_set_enabled(m, true);
        return;
    }
    if (!missing_owner && prop == PROP_IS_ENABLED) {
        monitor_query_status(m, PROP_SCREEN_READER);
        return;
    }
    monitor_set_enabled(m, false);
}

static void monitor_query_status(struct a11y_dbus_monitor *m,
                                 enum status_prop prop)
{
    if (!m->conn || m->disconnected || !m->registered)
        return;
    cancel_pending(m, &m->status_call);
    static const char *const props[] = { "IsEnabled", "ScreenReaderEnabled" };
    const char *iface = "org.a11y.Status";
    const char *prop_name = props[prop];
    dbus_message *msg = m->api.message_new_method_call(
        "org.a11y.Bus", "/org/a11y/bus",
        "org.freedesktop.DBus.Properties", "Get");
    if (!msg)
        return;
    /* Do not launch at-spi-bus-launcher when idle. */
    m->api.message_set_auto_start(msg, 0);
    if (!m->api.message_append_args(msg, DBUS_TYPE_STRING, &iface,
                                    DBUS_TYPE_STRING, &prop_name,
                                    DBUS_TYPE_INVALID)) {
        m->api.message_unref(msg);
        return;
    }
    dbus_pending_call *call = NULL;
    dbus_bool_t sent = m->api.connection_send_with_reply(
        m->conn, msg, &call, DBUS_TIMEOUT_USE_DEFAULT);
    m->api.message_unref(msg);
    if (!sent || !call)
        return;
    m->pending_prop = prop;
    m->status_call = call;
    if (!m->api.pending_call_set_notify(call, status_notify, m, NULL)) {
        cancel_pending(m, &m->status_call);
        return;
    }
    m->api.connection_flush(m->conn);
}

static void hello_notify(dbus_pending_call *call, void *userdata)
{
    struct a11y_dbus_monitor *m = userdata;
    if (m->hello_call != call)
        return;
    dbus_message *reply = m->api.pending_call_steal_reply(call);
    m->hello_call = NULL;
    m->api.pending_call_unref(call);
    if (!reply ||
        m->api.message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        if (reply)
            m->api.message_unref(reply);
        m->disconnected = true;
        return;
    }
    dbus_message_iter iter;
    const char *unique = NULL;
    if (m->api.message_iter_init(reply, &iter) &&
        iter_read_string(&m->api, &iter, &unique))
        m->api.bus_set_unique_name(m->conn, unique);
    m->api.message_unref(reply);
    if (!unique) {
        m->disconnected = true;
        return;
    }
    m->registered = true;

    static const char *const rules[] = {
        "type='signal',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path='/org/a11y/bus',arg0='org.a11y.Status'",
        "type='signal',sender='org.freedesktop.DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "path='/org/freedesktop/DBus',arg0='org.a11y.Bus'",
    };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++)
        m->api.bus_add_match(m->conn, rules[i], NULL);
    monitor_query_status(m, PROP_IS_ENABLED);
}

static void handle_properties_changed(struct a11y_dbus_monitor *m,
                                      dbus_message *msg)
{
    const char *path = m->api.message_get_path(msg);
    if (!path || strcmp(path, "/org/a11y/bus") != 0)
        return;
    dbus_message_iter iter;
    const char *iface = NULL;
    if (!m->api.message_iter_init(msg, &iter) ||
        !iter_read_string(&m->api, &iter, &iface) ||
        strcmp(iface, "org.a11y.Status") != 0)
        return;

    bool saw_true = false;
    bool saw_false = false;
    if (m->api.message_iter_next(&iter) &&
        m->api.message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter dict;
        m->api.message_iter_recurse(&iter, &dict);
        while (m->api.message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter entry;
            const char *key = NULL;
            m->api.message_iter_recurse(&dict, &entry);
            if (iter_read_string(&m->api, &entry, &key) &&
                is_a11y_prop_name(key) && m->api.message_iter_next(&entry)) {
                bool val = false;
                if (iter_read_variant_bool(&m->api, &entry, &val)) {
                    if (val)
                        saw_true = true;
                    else
                        saw_false = true;
                }
            }
            m->api.message_iter_next(&dict);
        }
    }

    bool invalidated = false;
    if (m->api.message_iter_next(&iter) &&
        m->api.message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter arr;
        m->api.message_iter_recurse(&iter, &arr);
        while (m->api.message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            const char *key = NULL;
            if (iter_read_string(&m->api, &arr, &key) &&
                is_a11y_prop_name(key))
                invalidated = true;
            m->api.message_iter_next(&arr);
        }
    }

    if (saw_true)
        monitor_set_enabled(m, true);
    else if (saw_false || invalidated)
        monitor_query_status(m, PROP_IS_ENABLED);
}

static void handle_name_owner_changed(struct a11y_dbus_monitor *m,
                                      dbus_message *msg)
{
    dbus_message_iter iter;
    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    if (!m->api.message_iter_init(msg, &iter) ||
        !iter_read_string(&m->api, &iter, &name) ||
        !m->api.message_iter_next(&iter) ||
        !iter_read_string(&m->api, &iter, &old_owner) ||
        !m->api.message_iter_next(&iter) ||
        !iter_read_string(&m->api, &iter, &new_owner))
        return;
    if (strcmp(name, "org.a11y.Bus") != 0)
        return;
    if (new_owner[0])
        monitor_query_status(m, PROP_IS_ENABLED);
    else
        monitor_set_enabled(m, false);
}

static int dbus_filter(dbus_connection *conn, dbus_message *msg, void *data)
{
    (void)conn;
    struct a11y_dbus_monitor *m = data;
    if (m->api.message_is_signal(msg, "org.freedesktop.DBus.Local",
                                 "Disconnected")) {
        m->disconnected = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (m->api.message_is_signal(msg, "org.freedesktop.DBus.Properties",
                                 "PropertiesChanged")) {
        handle_properties_changed(m, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (m->api.message_is_signal(msg, "org.freedesktop.DBus",
                                 "NameOwnerChanged")) {
        handle_name_owner_changed(m, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void monitor_stop(void)
{
    struct a11y_dbus_monitor *m = &a11y_monitor;
    struct yasl_loop *loop = m->loop;
    cancel_pending(m, &m->hello_call);
    cancel_pending(m, &m->status_call);
    if (m->conn) {
        if (m->filter_added) {
            m->api.connection_remove_filter(m->conn, dbus_filter, m);
            m->filter_added = false;
        }
        m->api.connection_close(m->conn);
        m->api.connection_unref(m->conn);
        m->conn = NULL;
    }
    while (m->watches) {
        struct dbus_watch_item *item = m->watches;
        m->watches = item->next;
        item->removed = true;
        item->m = NULL;
        item->watch = NULL;
        if (item->fd_id > 0 && loop) {
            yasl_loop_del_fd(loop, item->fd_id);
            item->fd_id = 0;
        }
    }
    while (m->timeouts) {
        struct dbus_timeout_item *item = m->timeouts;
        m->timeouts = item->next;
        item->removed = true;
        item->m = NULL;
        item->timeout = NULL;
        if (item->timer_id && loop) {
            yasl_loop_del_timer(loop, item->timer_id);
            item->timer_id = 0;
        }
    }
    if (m->lib) {
        dlclose(m->lib);
        m->lib = NULL;
    }
    *m = (struct a11y_dbus_monitor){ .loop = loop };
}

static bool monitor_send_hello(struct a11y_dbus_monitor *m)
{
    dbus_message *msg = m->api.message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "Hello");
    if (!msg)
        return false;
    dbus_pending_call *call = NULL;
    dbus_bool_t sent = m->api.connection_send_with_reply(
        m->conn, msg, &call, DBUS_TIMEOUT_USE_DEFAULT);
    m->api.message_unref(msg);
    if (!sent || !call)
        return false;
    m->hello_call = call;
    if (!m->api.pending_call_set_notify(call, hello_notify, m, NULL)) {
        cancel_pending(m, &m->hello_call);
        return false;
    }
    return true;
}

static void monitor_start(void)
{
    struct a11y_dbus_monitor *m = &a11y_monitor;
    if (m->started || !m->loop)
        return;
    m->started = true;

    char *addr = resolve_bus_address();
    if (!addr)
        return;
    if (!load_dbus_api(m)) {
        free(addr);
        return;
    }

    dbus_error err;
    m->api.error_init(&err);
    m->conn = m->api.connection_open_private(addr, &err);
    free(addr);
    if (!m->conn || m->api.error_is_set(&err)) {
        m->api.error_free(&err);
        monitor_stop();
        return;
    }
    m->api.error_free(&err);
    m->api.connection_set_exit_on_disconnect(m->conn, 0);

    if (!m->api.connection_add_filter(m->conn, dbus_filter, m, NULL)) {
        monitor_stop();
        return;
    }
    m->filter_added = true;

    if (!m->api.connection_set_watch_functions(m->conn, watch_add, watch_remove,
                                               watch_toggled, m, NULL) ||
        !m->api.connection_set_timeout_functions(
            m->conn, timeout_add, timeout_remove, timeout_toggled, m, NULL) ||
        !monitor_send_hello(m)) {
        monitor_stop();
        return;
    }
    m->api.connection_flush(m->conn);
    monitor_pump(m);
}
#endif

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
        window->adapter = accesskit_macos_subclassing_adapter_new(
            native.ns_view, activate, (void *)window->token, perform_action,
            (void *)window->token);
    if (!window->adapter)
        goto fail;
#elif defined(__linux__)
    if (!window->eager && !a11y_monitor.enabled) {
        monitor_start();
        return window;
    }
    window->adapter = accesskit_unix_adapter_new(
        activate, (void *)window->token, perform_action, (void *)window->token,
        deactivate, (void *)window->token);
    if (!window->adapter)
        goto fail;
    if (a11y_monitor.enabled) {
        pthread_mutex_lock(&providers_mutex);
        window->active = true;
        window->activation_generation++;
        pthread_mutex_unlock(&providers_mutex);
        update_focus(window);
    }
#else
    window->adapter = accesskit_unix_adapter_new(
        activate, (void *)window->token, perform_action, (void *)window->token,
        deactivate, (void *)window->token);
    if (!window->adapter)
        goto fail;
#endif
    return window;

fail:
    pthread_mutex_lock(&providers_mutex);
    struct provider_window **link = &providers;
    while (*link != window)
        link = &(*link)->next;
    *link = window->next;
    pthread_mutex_unlock(&providers_mutex);
    free(window);
    return NULL;
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
    if (!window->adapter)
        return;
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
    if (!window || !window->adapter || !snapshot || !snapshot->count)
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
    bool empty = providers == NULL;
    pthread_mutex_unlock(&providers_mutex);
    if (window->adapter) {
#ifdef __APPLE__
        accesskit_macos_subclassing_adapter_free(window->adapter);
#else
        accesskit_unix_adapter_free(window->adapter);
#endif
    }
#if defined(__linux__)
    if (empty) {
        monitor_stop();
        a11y_monitor.loop = NULL;
    }
#else
    (void)empty;
#endif
    ui_accessibility_tree_destroy(&window->snapshot);
    free(window->text.runs);
    free(window);
}

static bool provider_is_active(void *instance)
{
    struct provider_window *window = instance;
    pthread_mutex_lock(&providers_mutex);
    bool active = window && window->adapter && (window->active || window->eager);
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

void ui_accessibility_set_loop(struct yasl_loop *loop)
{
#if defined(__linux__)
    if (a11y_monitor.loop == loop)
        return;
    if (a11y_monitor.started)
        monitor_stop();
    a11y_monitor.loop = loop;
#else
    (void)loop;
#endif
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
