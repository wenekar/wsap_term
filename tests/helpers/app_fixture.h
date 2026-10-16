#ifndef TEST_APP_FIXTURE_H
#define TEST_APP_FIXTURE_H

#include <assert.h>
#include <stdio.h>

#include "app/app.h"

/* A real app over the built-in defaults, as main() would build it. The
 * app loads its own settings, so the fixture passes them as arguments.
 * --config NONE keeps the developer's config file out of the tests. */
struct test_app {
    struct app *app;
};

static inline void test_app_init(struct test_app *t, const char *command,
                                 struct vector2i size)
{
    char width[16], height[16];
    snprintf(width, sizeof(width), "%d", size.x);
    snprintf(height, sizeof(height), "%d", size.y);
    char *argv[12] = {
        "wsap_term", "--config", "NONE",
        "--width", width, "--height", height,
    };
    int argc = 7;
    if (command) {
        argv[argc++] = "--command";
        argv[argc++] = (char *)command;
    }
    int status = 0;
    t->app = app_create(argc, argv, &status);
    assert(t->app);
}

static inline void test_app_fini(struct test_app *t)
{
    app_destroy(t->app);
}

#endif
