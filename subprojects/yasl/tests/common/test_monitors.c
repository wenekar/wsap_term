#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "yasl/yasl.h"

/*
 * Tests of the monitor queries (common/controls.c).
 *
 * A headless machine or a compositor with no monitor reports no monitor, and
 * that is not an error. The test checks the replies that are valid without a
 * display, then checks the entries when a display opens. It reports a skip,
 * exit code 77, when no display opens.
 */

/* The empty answers are the same before a display opens and without one. */
static void check_no_monitor(void)
{
    struct yasl_monitor_info info;
    memset(&info, 0, sizeof(info));
    assert(yasl_get_monitor_count() == 0);
    errno = 0;
    assert(yasl_get_monitor(0, &info) == -ENOTSUP);
    errno = 0;
    assert(yasl_get_primary_monitor(&info) == -ENOTSUP);
}

int main(void)
{
    /* Before initialization there is no display, so there is no monitor. */
    check_no_monitor();

    /* A running display may have no monitor: a headless compositor has none. */
    if (yasl_init(NULL) != 0) {
        int error = errno;
        check_no_monitor();
        fprintf(stderr, "SKIP: no display server (YASL error %d)\n", error);
        return 77;
    }
    struct yasl_monitor_info info;
    memset(&info, 0, sizeof(info));
    int count = yasl_get_monitor_count();
    assert(count >= 0);
    for (int i = 0; i < count; i++) {
        assert(yasl_get_monitor(i, &info) == 0);
        assert(info.width > 0 && info.height > 0);
        assert(info.name[YASL_MONITOR_NAME_MAX - 1] == '\0');
        assert(info.work_width >= 0 && info.work_height >= 0);
        assert(info.work_x >= info.x && info.work_y >= info.y);
    }
    errno = 0;
    assert(yasl_get_monitor(count, &info) < 0);
    if (count > 0) {
        struct yasl_monitor_info primary;
        int status = yasl_get_primary_monitor(&primary);
        assert(status == 0 || errno == ENOENT);
    }
    yasl_shutdown();
    return 0;
}
