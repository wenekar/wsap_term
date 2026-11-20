/* The tab model holds contents of more than one kind. It finds each tab by
 * the content identity, and it calls the content ops of that tab. This test
 * puts a terminal and a dummy content in the same table, switches between
 * them, moves one to another window, and closes both. */

#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "app/tabs/tabs.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "terminal/terminal.h"
#include "ui/yasl/window.h"

/* A content that has a title and a window, and no widget. */
struct dummy_content {
    const char *title;
};

static int dummy_destroys;
static const struct window *dummy_window;

static void dummy_destroy(void *content)
{
    (void)content;
    dummy_destroys++;
}

static void dummy_set_window(void *content, struct window *window)
{
    (void)content;
    dummy_window = window;
}

static const char *dummy_title(const void *content)
{
    const struct dummy_content *dummy = content;
    return dummy->title;
}

static int dummy_font_size(const void *content)
{
    (void)content;
    return 7;
}

static bool dummy_exited(const void *content)
{
    (void)content;
    return false;
}

static const struct app_tab_content_ops DUMMY_OPS = {
    .destroy = dummy_destroy,
    .set_window = dummy_set_window,
    .title = dummy_title,
    .font_size = dummy_font_size,
    .exited = dummy_exited,
};

static void mixed_contents_test(void)
{
    struct test_app a;
    test_app_init(&a, "/bin/cat", (struct vector2i){ .x = 800, .y = 600 });
    struct window *source = test_terminal_window(a.app);
    struct window *target = test_terminal_window(a.app);
    assert(source && target);

    /* One tab of the dummy kind. It has no widget, so the root mounts no
     * content for it. */
    struct dummy_content dummy = { .title = "dummy" };
    app_tab_id id = app_tab_model_add(&a.app->tabs, &dummy, &DUMMY_OPS);
    assert(id);
    assert(app_tabs_bind(source, id, -1));
    assert(app_tabs_count(source) == 1);
    assert(app_tabs_active(source) == &dummy);
    assert(dummy_window == source);

    /* A terminal tab joins it in the same table. */
    assert(app_tabs_new(source));
    struct terminal *terminal_content = app_tabs_active(source);
    assert(terminal_content && (void *)terminal_content != (void *)&dummy);
    assert(app_tabs_count(source) == 2);

    /* Switch between the two kinds. */
    app_tabs_select(source, 0);
    assert(app_tabs_active(source) == &dummy);
    const char *title = window_title(source);
    assert(title && !strcmp(title, "dummy"));
    app_tabs_select(source, 1);
    assert(app_tabs_active(source) == terminal_content);

    /* The dummy content moves to the other window. */
    assert(app_tabs_move(source, 0, target, -1));
    assert(app_tabs_count(source) == 1);
    assert(app_tabs_active(source) == terminal_content);
    assert(app_tabs_count(target) == 1);
    assert(app_tabs_active(target) == &dummy);
    assert(dummy_window == target);

    /* Close both tabs. Each window then closes and frees its tabs. */
    app_tabs_close(target, 0);
    assert(app_tabs_count(target) == 0);
    app_tabs_close(source, 0);
    assert(app_tabs_count(source) == 0);
    window_host_reap(&a.app->host);
    assert(dummy_destroys == 1);
    assert(a.app->tabs.count == 0);
    test_app_fini(&a);
}

int main(void)
{
    mixed_contents_test();
    puts("PASS: the tab model holds a terminal and a dummy content");
    return 0;
}
