#ifndef TEST_TERMINAL_WINDOW_H
#define TEST_TERMINAL_WINDOW_H

#include "terminal/terminal.h"
#include "ui/yasl/window.h"

/* A terminal window with no surface and no tab, for tests. */
static inline struct window *test_terminal_window(struct window_host *host,
                                                  const struct terminal_config *config)
{
    struct window *w = window_create(host, &(struct window_spec){0});
    if (!w)
        return NULL;
    if (!terminal_mount(w, &(struct terminal_spec){ .config = config, .empty = true })) {
        window_close(w);
        return NULL;
    }
    return w;
}

#endif
