#include "common/timing.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/*
 * Tests of the frame estimate that paces a surface (common/timing.c).
 *
 * A display link reports the frame that it fired for. The estimate must follow
 * that frame, not the distance to the next callback: a link that skips frames
 * still reports one display frame.
 */

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

static void link_period_test(void)
{
    const double hz240 = 1.0 / 240.0;
    const double hz60 = 1.0 / 60.0;

    /* A steady display: the frame duration and the distance to the target
     * agree. */
    double period = yasl_private_clock_link_period(10.0, hz240, 10.0 + hz240);
    assert(near(period, hz240));
    period = yasl_private_clock_link_period(10.0, hz60, 10.0 + hz60);
    assert(near(period, hz60));

    /* The link skipped a frame: one display frame is still the frame that it
     * fired for, so the estimate must not double. */
    period = yasl_private_clock_link_period(10.0, hz240, 10.0 + 2.0 * hz240);
    assert(near(period, hz240));

    /* A variable refresh display follows each frame that the link reports. */
    const double vrr[] = {hz60, 1.0 / 120.0, hz240, 1.0 / 100.0, hz60};
    for (size_t i = 0; i < sizeof vrr / sizeof vrr[0]; i++) {
        period = yasl_private_clock_link_period(1.0, vrr[i], 1.0 + vrr[i]);
        assert(near(period, vrr[i]));
    }

    /* Without a duration the distance to the target answers. */
    period = yasl_private_clock_link_period(10.0, 0.0, 10.0 + hz60);
    assert(near(period, hz60));
    period = yasl_private_clock_link_period(10.0, -hz60, 10.0 + hz60);
    assert(near(period, hz60));

    /* No usable value keeps the earlier estimate. */
    assert(yasl_private_clock_link_period(10.0, 0.0, 10.0) == 0.0);
    assert(yasl_private_clock_link_period(10.0, 0.5, 10.5) == 0.0);
    assert(yasl_private_clock_link_period(10.0, 0.0001, 10.0001) == 0.0);
    assert(yasl_private_clock_link_period(NAN, NAN, NAN) == 0.0);
    assert(yasl_private_clock_link_period(0.0, 0.0, INFINITY) == 0.0);
}

static void refresh_ns_test(void)
{
    /* A rate the platform measured and the default of a 60 Hz display. */
    assert(yasl_private_clock_refresh_ns(4166666.0) == 4166666u);
    assert(yasl_private_clock_refresh_ns(16666666.0) == 16666666u);
    assert(yasl_private_clock_refresh_ns(1000000.0) == 1000000u);
    assert(yasl_private_clock_refresh_ns(1000000000.0) == 1000000000u);

    /* A missing or impossible interval paces at 60 Hz. */
    assert(yasl_private_clock_refresh_ns(0.0) == 16666667u);
    assert(yasl_private_clock_refresh_ns(-1.0) == 16666667u);
    assert(yasl_private_clock_refresh_ns(999999.0) == 16666667u);
    assert(yasl_private_clock_refresh_ns(1000000001.0) == 16666667u);
}

int main(void)
{
    link_period_test();
    refresh_ns_test();
    puts("frame estimate tests passed");
    return 0;
}
