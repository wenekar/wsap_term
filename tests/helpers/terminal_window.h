#ifndef TEST_TERMINAL_WINDOW_H
#define TEST_TERMINAL_WINDOW_H

#include "app/app.h"
#include "app/tabs/tabs.h"
#include "terminal/terminal.h"
#include "ui/yasl/window.h"

/* A window that shows the tabbed app root. The tab table of the app owns its
 * terminals. It opens no tab, so the test adds the first one. */
static inline struct window *test_terminal_window(struct app *app)
{
    struct window *w = window_create(&app->host, &(struct window_spec){0});
    if (!w)
        return NULL;
    if (!app_tabs_mount(w, &(struct app_tabs_spec){
            .config = &app->config.terminal, .model = &app->tabs,
            .empty = true })) {
        window_close(w);
        return NULL;
    }
    return w;
}

#endif
