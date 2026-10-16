#define _GNU_SOURCE

#include <poll.h>

#include "ui/yasl/internal.h"

#include "platform/platform.h"
#include "ui/accessibility.h"
#include "ui/yasl/accessibility/accessibility.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct queued_action {
    struct queued_action *next;
    struct ui_accessibility_action_request request;
    char *value;
};

struct popup_accessibility {
    struct ui_accessibility_provider provider;
    struct ui_accessibility_tree tree;
};

struct window_accessibility {
    struct window *window;
    struct ui_accessibility_provider provider;
    struct ui_accessibility_tree tree;
    struct popup_accessibility popups[UI_MENU_MAX_LEVELS];
    pthread_mutex_t mutex;
    struct queued_action *head;
    struct queued_action *tail;
    int event_fd;
    int event_wr;
    int event_watch;
    bool active;
    bool stopped;
};

static void queued_action_free(struct queued_action *action)
{
    if (!action)
        return;
    free(action->value);
    free(action);
}

static void action_sink(void *userdata,
                        const struct ui_accessibility_action_request *request)
{
    struct window_accessibility *accessibility = userdata;
    if (!accessibility || !request || !request->node_id || !request->action)
        return;

    struct queued_action *action = calloc(1, sizeof(*action));
    if (!action)
        return;
    action->request = *request;
    if (request->value) {
        action->value = strdup(request->value);
        if (!action->value) {
            free(action);
            return;
        }
        action->request.value = action->value;
    }

    int event_wr = -1;
    pthread_mutex_lock(&accessibility->mutex);
    if (!accessibility->active) {
        pthread_mutex_unlock(&accessibility->mutex);
        queued_action_free(action);
        return;
    }
    if (accessibility->tail)
        accessibility->tail->next = action;
    else
        accessibility->head = action;
    accessibility->tail = action;
    event_wr = accessibility->event_wr;
    pthread_mutex_unlock(&accessibility->mutex);

    /* The loop watches the eventfd, so this write wakes it. */
    if (event_wr >= 0) {
        uint64_t wake = 1;
        ssize_t ignored = write(event_wr, &wake, sizeof(wake));
        (void)ignored;
    }
}

static void actions_ready(void *userdata, int fd, short revents)
{
    (void)revents;
    struct window_accessibility *accessibility = userdata;
    uint64_t wake;
    while (read(fd, &wake, sizeof(wake)) > 0) {
    }

    pthread_mutex_lock(&accessibility->mutex);
    struct queued_action *actions = accessibility->head;
    accessibility->head = NULL;
    accessibility->tail = NULL;
    pthread_mutex_unlock(&accessibility->mutex);

    struct window *window = accessibility->window;
    while (actions) {
        struct queued_action *next = actions->next;
        if (!window_closed(window) && !accessibility->stopped) {
            bool handled = ui_accessibility_dispatch_action(
                &ui_of(window)->root, &actions->request);
            for (int i = 0; !handled && i < ui_of(window)->menu_popups; i++)
                handled = ui_accessibility_dispatch_action(
                    &ui_of(window)->menu_popup[i].root, &actions->request);
        }
        queued_action_free(actions);
        actions = next;
        if (window_closed(window) || ui_of(window)->accessibility != accessibility ||
            accessibility->stopped) {
            while (actions) {
                struct queued_action *rem = actions->next;
                queued_action_free(actions);
                actions = rem;
            }
            break;
        }
    }
}

static void accessibility_stop_instance(
    struct window_accessibility *accessibility)
{
    if (!accessibility)
        return;
    if (accessibility->stopped)
        return;
    accessibility->stopped = true;
    ui_accessibility_provider_detach(&accessibility->provider);
    for (int i = 0; i < UI_MENU_MAX_LEVELS; i++) {
        ui_accessibility_provider_detach(&accessibility->popups[i].provider);
        ui_accessibility_tree_clear(&accessibility->popups[i].tree);
    }

    pthread_mutex_lock(&accessibility->mutex);
    accessibility->active = false;
    struct queued_action *actions = accessibility->head;
    accessibility->head = NULL;
    accessibility->tail = NULL;
    int event_watch = accessibility->event_watch;
    int event_fd = accessibility->event_fd;
    int event_wr = accessibility->event_wr;
    accessibility->event_watch = -1;
    accessibility->event_fd = -1;
    accessibility->event_wr = -1;
    pthread_mutex_unlock(&accessibility->mutex);

    while (actions) {
        struct queued_action *next = actions->next;
        queued_action_free(actions);
        actions = next;
    }
    if (event_watch >= 0)
        yasl_loop_del_fd(window_host(accessibility->window)->loop, event_watch);
    if (event_fd >= 0)
        close(event_fd);
    if (event_wr >= 0 && event_wr != event_fd)
        close(event_wr);
}

void window_accessibility_stop(struct window *window)
{
    if (window)
        accessibility_stop_instance(ui_of(window)->accessibility);
}

static void accessibility_free(struct window_accessibility *accessibility)
{
    if (!accessibility)
        return;
    accessibility_stop_instance(accessibility);
    ui_accessibility_tree_destroy(&accessibility->tree);
    for (int i = 0; i < UI_MENU_MAX_LEVELS; i++)
        ui_accessibility_tree_destroy(&accessibility->popups[i].tree);
    pthread_mutex_destroy(&accessibility->mutex);
    free(accessibility);
}

void window_accessibility_create(struct window *window)
{
    struct yasl_surface *surface = window_surface(window);
    if (!window || !surface || ui_of(window)->accessibility)
        return;
    const struct ui_accessibility_provider_ops *ops =
        ui_accessibility_provider_ops();
    if (!ops)
        return;

    struct window_accessibility *accessibility =
        calloc(1, sizeof(*accessibility));
    if (!accessibility)
        return;
    accessibility->window = window;
    accessibility->event_fd = -1;
    accessibility->event_wr = -1;
    accessibility->event_watch = -1;
    if (pthread_mutex_init(&accessibility->mutex, NULL) != 0) {
        free(accessibility);
        return;
    }

    accessibility->event_fd = platform_eventfd(&accessibility->event_wr);
    if (accessibility->event_fd < 0)
        goto fail;
    accessibility->event_watch = yasl_loop_add_fd(
        window_host(window)->loop, accessibility->event_fd, POLLIN,
        actions_ready, accessibility);
    if (accessibility->event_watch < 0)
        goto fail;

    accessibility->active = true;
    if (!ui_accessibility_provider_attach(
            &accessibility->provider, ops,
            ui_accessibility_native_handle(surface),
            action_sink, accessibility))
        goto fail;
    ui_of(window)->accessibility = accessibility;
    return;

fail:
    accessibility_free(accessibility);
}

void window_accessibility_destroy(struct window *window)
{
    if (!window || !ui_of(window)->accessibility)
        return;
    struct window_accessibility *accessibility = ui_of(window)->accessibility;
    ui_of(window)->accessibility = NULL;
    accessibility_free(accessibility);
}

void window_accessibility_publish(struct window *window)
{
    if (!window || !ui_of(window)->accessibility || window_closed(window))
        return;
    struct window_accessibility *accessibility = ui_of(window)->accessibility;
    if (!ui_accessibility_provider_is_active(&accessibility->provider))
        return;
    if (!ui_accessibility_tree_build(&ui_of(window)->root, &accessibility->tree))
        return;
    ui_accessibility_provider_publish_snapshot(&accessibility->provider,
                                               &accessibility->tree);
    for (int i = 0; i < ui_of(window)->menu_popups; i++) {
        struct window_ui_menu_popup *popup = &ui_of(window)->menu_popup[i];
        struct popup_accessibility *popup_accessibility =
            &accessibility->popups[i];
        if (!popup->surface || !popup_accessibility->provider.instance)
            continue;
        if (!ui_accessibility_provider_is_active(&popup_accessibility->provider))
            continue;
        ui_surface_prepare(&popup->ui);
        if (!ui_accessibility_tree_build(&popup->root,
                                         &popup_accessibility->tree))
            continue;
        ui_accessibility_provider_publish_snapshot(
            &popup_accessibility->provider, &popup_accessibility->tree);
    }
}

void window_accessibility_menu_popup_open(struct window *window, int depth)
{
    if (!window || depth < 0 || depth >= UI_MENU_MAX_LEVELS ||
        !ui_of(window)->accessibility || ui_of(window)->accessibility->stopped)
        return;
    struct window_accessibility *accessibility = ui_of(window)->accessibility;
    struct window_ui_menu_popup *popup = &ui_of(window)->menu_popup[depth];
    struct popup_accessibility *popup_accessibility =
        &accessibility->popups[depth];
    const struct ui_accessibility_provider_ops *ops =
        ui_accessibility_provider_ops();
    if (!popup->surface || !ops ||
        !ui_accessibility_provider_attach(
            &popup_accessibility->provider, ops,
            ui_accessibility_native_handle(popup->surface), action_sink,
            accessibility))
        return;
    ui_accessibility_set_menu_popup_depth(&popup_accessibility->provider,
                                          depth);
    ui_surface_prepare(&popup->ui);
    if (ui_accessibility_tree_build(&popup->root,
                                    &popup_accessibility->tree))
        ui_accessibility_provider_publish_snapshot(
            &popup_accessibility->provider, &popup_accessibility->tree);
}

void window_accessibility_menu_popup_close(struct window *window, int depth)
{
    if (!window || !ui_of(window)->accessibility || depth < 0 ||
        depth >= UI_MENU_MAX_LEVELS)
        return;
    struct popup_accessibility *popup =
        &ui_of(window)->accessibility->popups[depth];
    ui_accessibility_provider_detach(&popup->provider);
    ui_accessibility_tree_clear(&popup->tree);
}
