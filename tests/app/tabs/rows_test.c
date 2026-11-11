/* The tab bar keeps its reserved height until close presentation ends.
 * The content widget gives the session its cell grid.
 *
 * The checks compare the grid of the terminal core with the content box. One
 * check also starts a real shell and reads `stty size`, so the PTY rows are
 * part of the evidence. */

#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/tabs/tabs.h"
#include "font/font.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/debug.h"
#include "ui/surface.h"
#include "ui/widget.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"
#include <wsap_vtcore/wsap_vtcore.h>

static struct vector2i grid_of(struct terminal *content)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(
        terminal_session(content)->term);
    return (struct vector2i){ .x = size.x, .y = size.y };
}

/* The content box that the owner lays out. */
static struct rect2i content_box(struct terminal *content)
{
    return terminal_widget(content)->bounds;
}

/* The grid of a content must fit its content box and its font. */
static void assert_grid_fits(struct terminal *content)
{
    const struct font_metrics *m = font_instance_metrics(terminal_font(content));
    assert(m->cell_w > 0 && m->cell_h > 0);
    struct rect2i box = content_box(content);
    struct vector2i grid = grid_of(content);
    assert(box.width > 0 && box.height > 0);
    assert(grid.x == box.width / m->cell_w);
    assert(grid.y == box.height / m->cell_h);
}

static void layout_window(struct window *w, struct vector2i viewport)
{
    ui_surface_resize(window_ui_surface(w), viewport);
    app_tabs_relayout(w);
}

/* Adds a window with one tab to the app. */
static struct window *add_tabs_window(struct test_app *a,
                                      struct vector2i viewport)
{
    struct window *w = test_terminal_window(a->app);
    assert(w);
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(w), viewport);
    assert(app_tabs_new(w));
    app_tabs_relayout(w);
    return w;
}

static struct window *open_tabs_window(struct test_app *a,
                                       struct vector2i viewport,
                                       const char *command)
{
    test_app_init(a, command, viewport);
    return add_tabs_window(a, viewport);
}

static void close_window(struct window *w, struct test_app *a)
{
    window_close(w);
    test_app_fini(a);
}

/* Open a second tab, then close it. The grid of the first tab returns to its
 * first size, and its content box returns to its first rectangle. */
static void close_second_tab_restores_grid_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_tabs_window(&a, viewport, "/bin/cat");
    struct terminal *first = app_tabs_active(w);
    struct vector2i first_grid = grid_of(first);
    struct rect2i first_box = content_box(first);
    assert_grid_fits(first);
    assert(app_tabs_bar_height(w) == 0);

    assert(app_tabs_new(w));
    struct terminal *second = app_tabs_active(w);
    ui_surface_tick(window_ui_surface(w),
                    window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    layout_window(w, viewport);
    assert(second != first);
    assert(app_tabs_bar_height(w) > 0);
    assert(grid_of(second).y < first_grid.y);
    assert_grid_fits(second);
    struct vector2i closing_grid = grid_of(second);
    struct rect2i closing_box = content_box(second);

    app_tabs_close(w, app_tabs_active_index(w));
    layout_window(w, viewport);
    assert(app_tabs_active(w) == first);
    assert(app_tabs_count(w) == 1);
    assert(vector2i_eq(grid_of(first), closing_grid));
    assert(rect2i_eq(content_box(first), closing_box));
    assert_grid_fits(first);

    uint64_t middle = window_frame_time_ms(w) + APP_TABS_CLOSE_MS / 2;
    app_tabs_service(&a.app->tabs, middle);
    ui_surface_tick(window_ui_surface(w), middle);
    layout_window(w, viewport);
    assert(vector2i_eq(grid_of(first), closing_grid));
    struct rect2i sliding_box = content_box(first);
    assert(vector2i_eq(sliding_box.size, closing_box.size));
    assert(sliding_box.y > first_box.y && sliding_box.y < closing_box.y);

    const struct ui_widget *bar = ui_debug_find(window_ui_root(w), "tabbar");
    assert(bar);
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(bar, &parts);
    bool found = false;
    for (int i = 0; i < parts.n; i++) {
        if (!strcmp(parts.items[i].name, "bar")) {
            assert(sliding_box.y == rect2i_end(parts.items[i].rect).y);
            found = true;
        }
    }
    ui_debug_parts_free(&parts);
    assert(found);

    terminal_prepare(first, middle);
    struct terminal_frame_source source;
    assert(terminal_frame_source(first, middle, &source));
    assert(rect2i_eq(source.rect, sliding_box));
    assert_grid_fits(first);

    /* The content grows only after the close presentation ends. */
    app_tabs_service(&a.app->tabs, window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    layout_window(w, viewport);
    assert(app_tabs_count(w) == 1);
    assert(grid_of(first).y == first_grid.y);
    assert(rect2i_eq(content_box(first), first_box));
    assert_grid_fits(first);

    close_window(w, &a);
}

/* Close several tabs at once. Keep the grid size until they leave. */
static void close_many_tabs_before_animation_end_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_tabs_window(&a, viewport, "/bin/cat");
    struct terminal *first = app_tabs_active(w);
    struct vector2i first_grid = grid_of(first);
    assert(app_tabs_new(w));
    assert(app_tabs_new(w));
    assert(app_tabs_new(w));
    layout_window(w, viewport);
    assert(app_tabs_count(w) == 4);
    struct vector2i closing_grid = grid_of(app_tabs_active(w));
    struct rect2i closing_box = content_box(app_tabs_active(w));

    for (int i = 0; i < 3; i++)
        app_tabs_close(w, app_tabs_active_index(w));
    assert(app_tabs_count(w) == 1);
    assert(app_tabs_active(w) == first);
    assert(app_tabs_bar_height(w) > 0);

    layout_window(w, viewport);
    assert(rect2i_eq(content_box(first), closing_box));
    assert(vector2i_eq(grid_of(first), closing_grid));
    assert_grid_fits(first);

    app_tabs_service(&a.app->tabs, window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    layout_window(w, viewport);
    assert(app_tabs_count(w) == 1);
    assert(app_tabs_bar_height(w) == 0);
    assert(content_box(first).y == 0);
    assert(content_box(first).height == viewport.y);
    assert(grid_of(first).y == first_grid.y);
    assert_grid_fits(first);

    close_window(w, &a);
}

/* Close a tab and open another before the animation ends. Keep the same
 * grid size through both changes. */
static void close_and_reopen_during_animation_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_tabs_window(&a, viewport, "/bin/cat");
    struct terminal *first = app_tabs_active(w);
    assert(app_tabs_new(w));
    layout_window(w, viewport);
    struct terminal *second = app_tabs_active(w);
    struct vector2i two_tab_grid = grid_of(second);
    struct rect2i two_tab_box = content_box(second);
    assert_grid_fits(second);

    app_tabs_close(w, app_tabs_active_index(w));
    assert(app_tabs_count(w) == 1);
    assert(app_tabs_bar_height(w) > 0);
    assert(app_tabs_active(w) == first);
    assert(vector2i_eq(grid_of(first), two_tab_grid));
    assert(rect2i_eq(content_box(first), two_tab_box));

    assert(app_tabs_new(w));
    struct terminal *third = app_tabs_active(w);
    assert(third != first && third != second);
    assert(app_tabs_count(w) == 2);
    assert(app_tabs_bar_height(w) > 0);
    layout_window(w, viewport);
    assert(grid_of(third).y == two_tab_grid.y);
    assert(rect2i_eq(content_box(third), two_tab_box));
    assert_grid_fits(third);

    app_tabs_service(&a.app->tabs, window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    layout_window(w, viewport);
    assert(app_tabs_count(w) == 2);
    assert(app_tabs_active(w) == third);
    assert(app_tabs_bar_height(w) > 0);
    assert(grid_of(third).y == two_tab_grid.y);
    assert(rect2i_eq(content_box(third), two_tab_box));
    assert_grid_fits(third);

    close_window(w, &a);
}

/* Closing an inactive tab keeps the visible grid until presentation ends. */
static void close_inactive_tab_keeps_grid_until_presentation_ends_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_tabs_window(&a, viewport, "/bin/cat");
    struct terminal *first = app_tabs_active(w);
    struct vector2i full_grid = grid_of(first);
    assert(app_tabs_new(w));
    app_tabs_select(w, 0);
    layout_window(w, viewport);
    struct vector2i closing_grid = grid_of(first);
    struct rect2i closing_box = content_box(first);

    app_tabs_close(w, 1);
    app_tabs_service(&a.app->tabs,
                     window_frame_time_ms(w) + APP_TABS_CLOSE_MS / 2);
    layout_window(w, viewport);
    assert(app_tabs_active(w) == first);
    assert(app_tabs_count(w) == 1);
    assert(vector2i_eq(grid_of(first), closing_grid));
    assert(rect2i_eq(content_box(first), closing_box));

    app_tabs_service(&a.app->tabs, window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    layout_window(w, viewport);
    assert(vector2i_eq(grid_of(first), full_grid));
    assert(content_box(first).y == 0);
    close_window(w, &a);
}

/* Select an inactive tab after a resize or a zoom. Its grid fits its own
 * content box. */
static void select_inactive_tab_after_resize_test(void)
{
    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_tabs_window(&a, viewport, "/bin/cat");
    struct terminal *first = app_tabs_active(w);
    assert(app_tabs_new(w));
    layout_window(w, viewport);
    struct terminal *second = app_tabs_active(w);

    app_tabs_select(w, 0);
    layout_window(w, viewport);
    assert(app_tabs_active(w) == first);
    struct vector2i wide = grid_of(first);
    assert_grid_fits(first);

    struct vector2i small = { .x = 640, .y = 400 };
    layout_window(w, small);
    struct vector2i narrow = grid_of(first);
    assert(narrow.y < wide.y);
    assert_grid_fits(first);

    app_tabs_select(w, 1);
    layout_window(w, small);
    assert(app_tabs_active(w) == second);
    assert(grid_of(second).y == narrow.y);
    assert(grid_of(second).x == narrow.x);
    assert(content_box(second).height == content_box(first).height);
    assert_grid_fits(second);

    terminal_adjust_font_size(second, 2);
    app_tabs_relayout(w);
    assert_grid_fits(second);
    app_tabs_select(w, 0);
    layout_window(w, small);
    assert_grid_fits(first);

    close_window(w, &a);
}

/* Transfer a tab across windows. The content takes the grid of the
 * destination, and a move to the window that holds the tab keeps it there. */
static void transfer_tab_fits_destination_test(void)
{
    struct test_app a;
    struct vector2i source_size = { .x = 800, .y = 600 };
    struct vector2i target_size = { .x = 640, .y = 400 };
    struct window *source = open_tabs_window(&a, source_size, "/bin/cat");
    struct window *target = add_tabs_window(&a, target_size);
    assert(app_tabs_new(source));
    layout_window(source, source_size);
    struct terminal *resident = app_tabs_active(target);
    assert(app_tabs_active_index(source) == 1);
    struct terminal *moved = app_tabs_active(source);

    assert(app_tabs_move(source, 1, target, -1));
    assert(app_tabs_count(source) == 1);
    assert(app_tabs_active(source) != moved);
    assert(app_tabs_active(target) == moved);
    layout_window(target, target_size);
    assert_grid_fits(moved);

    /* The destination gives the same grid to its own content at the same
     * content size. */
    app_tabs_select(target, app_tabs_active_index(target) == 0 ? 1 : 0);
    layout_window(target, target_size);
    struct terminal *other = app_tabs_active(target);
    assert(other == moved || other == resident);
    struct vector2i other_grid = grid_of(other);
    app_tabs_select(target, app_tabs_active_index(target) == 0 ? 1 : 0);
    layout_window(target, target_size);
    assert(app_tabs_active(target) != other);
    assert(grid_of(app_tabs_active(target)).y == other_grid.y);
    assert(grid_of(app_tabs_active(target)).x == other_grid.x);
    assert_grid_fits(app_tabs_active(target));

    /* The tab is already in this window. The move fails and leaves the tab
     * where it is. */
    assert(app_tabs_active(target) == moved);
    assert(!app_tabs_move(target, app_tabs_active_index(target), target, 0));
    assert(app_tabs_count(target) == 2);
    assert(app_tabs_active(target) == moved);
    assert_grid_fits(moved);

    window_close(target);
    close_window(source, &a);
}

/* A real shell reports the rows and the columns of its PTY. The values must
 * follow the content box across 1 -> 2 -> 1 live tabs. */
#define SHELL_WAIT_MS 8000

/* Runs `stty size` in the shell of `content` and reads the result file. */
static bool shell_size(struct window *w, struct terminal *content,
                       const char *path, int *rows, int *cols)
{
    /* Remove the old result first, so a stale line can not pass. */
    unlink(path);
    struct session *session = terminal_session(content);
    char command[256];
    int n = snprintf(command, sizeof(command), "stty size > '%s'\n", path);
    assert(n > 0 && (size_t)n < sizeof(command));
    assert(session_write_bytes(session, command, (size_t)n));

    struct yasl_loop *loop = window_host(w)->loop;
    uint64_t end = yasl_time_ns() + (uint64_t)SHELL_WAIT_MS * 1000000ull;
    while (yasl_time_ns() < end) {
        assert(yasl_loop_step(loop, 20) >= 0);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char line[64];
        char *got = fgets(line, sizeof(line), f);
        fclose(f);
        if (!got || !strchr(got, '\n'))
            continue;
        int r = 0, c = 0;
        if (sscanf(got, "%d %d", &r, &c) != 2)
            continue;
        *rows = r;
        *cols = c;
        return true;
    }
    return false;
}

static void live_shell_rows_follow_the_content_test(void)
{
    char path[] = "/tmp/wsap-tab-rows-XXXXXX";
    int probe = mkstemp(path);
    assert(probe >= 0);
    close(probe);
    unlink(path);

    struct test_app a;
    struct vector2i viewport = { .x = 800, .y = 600 };
    struct window *w = open_tabs_window(&a, viewport, "/bin/sh");
    struct terminal *first = app_tabs_active(w);
    struct vector2i first_grid = grid_of(first);
    int rows = 0, cols = 0;
    assert(shell_size(w, first, path, &rows, &cols));
    assert(rows == first_grid.y);
    assert(cols == first_grid.x);

    assert(app_tabs_new(w));
    layout_window(w, viewport);
    struct terminal *second = app_tabs_active(w);
    struct vector2i second_grid = grid_of(second);
    assert(second_grid.y < first_grid.y);
    assert(shell_size(w, second, path, &rows, &cols));
    assert(rows == second_grid.y);
    assert(cols == second_grid.x);

    app_tabs_close(w, app_tabs_active_index(w));
    layout_window(w, viewport);
    assert(app_tabs_active(w) == first);
    assert(shell_size(w, first, path, &rows, &cols));
    assert(rows == second_grid.y);
    assert(cols == second_grid.x);

    app_tabs_service(&a.app->tabs, window_frame_time_ms(w) + APP_TABS_CLOSE_MS);
    layout_window(w, viewport);
    uint64_t end = yasl_time_ns() + (uint64_t)SHELL_WAIT_MS * 1000000ull;
    do {
        assert(shell_size(w, first, path, &rows, &cols));
        if (rows == first_grid.y && cols == first_grid.x)
            break;
        assert(yasl_loop_step(window_host(w)->loop, 20) >= 0);
    } while (yasl_time_ns() < end);
    assert(rows == first_grid.y);
    assert(cols == first_grid.x);

    unlink(path);
    close_window(w, &a);
}

int main(void)
{
    close_second_tab_restores_grid_test();
    close_many_tabs_before_animation_end_test();
    close_and_reopen_during_animation_test();
    close_inactive_tab_keeps_grid_until_presentation_ends_test();
    select_inactive_tab_after_resize_test();
    transfer_tab_fits_destination_test();
    live_shell_rows_follow_the_content_test();
    puts("PASS: the tab grid follows the content box after close presentation");
    return 0;
}
