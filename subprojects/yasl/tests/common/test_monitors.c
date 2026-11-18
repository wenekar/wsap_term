#include <assert.h>
#include <errno.h>
#include <string.h>

#include "yasl/yasl.h"

/*
 * Tests of the monitor queries (common/controls.c).
 *
 * A headless machine or a compositor with no monitor reports no monitor, and
 * that is not an error. The test checks the replies that are valid without a
 * display, then checks the entries when a display opens.
 */

int main(void)
{
    /* Before initialization there is no display, so there is no monitor. */
    assert(yasl_get_monitor_count() == 0);
    struct yasl_monitor_info info;
    memset(&info, 0, sizeof(info));
    errno = 0;
    assert(yasl_get_monitor(0, &info) < 0);
    errno = 0;
    assert(yasl_get_primary_monitor(&info) < 0);

    /* A running display may have no monitor: a headless compositor has none. */
    if (yasl_init(NULL) != 0)
        return 0;
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
