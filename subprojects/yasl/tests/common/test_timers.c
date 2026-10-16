#define _POSIX_C_SOURCE 200809L
#include "common/loop.h"
#include "common/types.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct test_ctx {
    int count;
    uint64_t last_time;
    yasl_private_timer_id id;
    yasl_private_timer_id other_id;
    struct yasl_loop *loop;
    struct yasl_surface *surface;
    bool done;
};

/* 1. Single-shot timer: returns 0, fires exactly once. */
static uint64_t oneshot_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    (void)interval_ns;
    ctx->count++;
    return 0; /* Disarm */
}

static uint64_t count_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    (*(int *)ud)++;
    return 0;
}

/* Adds enough timers to move the timer array, then repeats itself. */
static uint64_t grow_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    if (ctx->count++ == 0) {
        for (int i = 0; i < 64; i++)
            assert(yasl_loop_add_timer(ctx->loop, YASL_PRIVATE_MS_TO_NS(60000), count_cb, NULL));
        return YASL_PRIVATE_MS_TO_NS(2);
    }
    assert(interval_ns == YASL_PRIVATE_MS_TO_NS(2));
    return 0;
}

/* 2. Repeating timer: ticks N times, then returns 0. */
static uint64_t repeat_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    (void)interval_ns;
    ctx->count++;
    if (ctx->count >= 5) {
        ctx->done = true;
        return 0; /* Disarm after 5 ticks */
    }
    return interval_ns; /* Repeat with same interval */
}

/* 3. Rescheduling timer: changes interval dynamically. */
static uint64_t reschedule_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    ctx->count++;
    if (ctx->count == 1) {
        assert(interval_ns == 2000000ull);
        return 5000000ull; /* Reschedule to 5ms */
    } else if (ctx->count == 2) {
        assert(interval_ns == 5000000ull);
        ctx->done = true;
        return 0; /* Done */
    }
    return 0;
}

/* 4. Timer adding another timer from inside callback. */
static uint64_t child_timer_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)interval_ns;
    struct test_ctx *ctx = ud;
    assert(id == ctx->other_id);
    ctx->count += 10;
    return 0;
}

static uint64_t parent_timer_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)interval_ns;
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    ctx->count += 1;
    ctx->other_id = yasl_loop_add_timer(ctx->loop, 1000000ull, child_timer_cb, ctx);
    assert(ctx->other_id > 0);
    return 0;
}

/* 5. Timer deleting another timer from inside callback. */
static uint64_t victim_timer_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)ud; (void)id; (void)interval_ns;
    assert(false && "Victim timer should have been cancelled before firing!");
    return 0;
}

static uint64_t killer_timer_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct test_ctx *ctx = ud;
    ctx->count++;
    assert(yasl_loop_del_timer(ctx->loop, ctx->other_id));
    return 0;
}

/* 6. Millisecond convenience conversion uses the same timer callback. */
static uint64_t ms_timer_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    assert(interval_ns == YASL_PRIVATE_MS_TO_NS(5));
    ctx->count++;
    if (ctx->count >= 3) {
        ctx->done = true;
        return 0;
    }
    return interval_ns;
}

static void test_loop_timers(void)
{
    struct yasl_loop *l = yasl_loop_create();
    assert(l);

    /* Invalid arguments */
    assert(yasl_loop_add_timer(NULL, 1000, oneshot_cb, NULL) == 0);
    assert(yasl_loop_add_timer(l, 1000, NULL, NULL) == 0);
    assert(!yasl_loop_del_timer(NULL, 1));
    assert(!yasl_loop_del_timer(l, 0));
    assert(!yasl_loop_del_timer(l, 99999));
    assert(yasl_private_loop_next_timeout_ms(l) == -1);

    /* Test 1: Single-shot */
    struct test_ctx ctx1 = { .loop = l };
    ctx1.id = yasl_loop_add_timer(l, 5000000ull, oneshot_cb, &ctx1);
    assert(ctx1.id > 0);
    assert(yasl_private_loop_next_timeout_ms(l) >= 0);

    while (ctx1.count == 0) {
        int handled = yasl_loop_step(l, 50);
        assert(handled >= 0);
    }
    assert(ctx1.count == 1);
    /* Should be disarmed and removed */
    assert(!yasl_loop_del_timer(l, ctx1.id));
    assert(yasl_private_loop_next_timeout_ms(l) == -1);

    /* Test 2: Repeating */
    struct test_ctx ctx2 = { .loop = l };
    ctx2.id = yasl_loop_add_timer(l, 2000000ull, repeat_cb, &ctx2);
    assert(ctx2.id > 0);
    while (!ctx2.done) {
        yasl_loop_step(l, 50);
    }
    assert(ctx2.count == 5);
    assert(!yasl_loop_del_timer(l, ctx2.id));

    /* Test 3: Reschedule dynamically */
    struct test_ctx ctx3 = { .loop = l };
    ctx3.id = yasl_loop_add_timer(l, 2000000ull, reschedule_cb, &ctx3);
    assert(ctx3.id > 0);
    while (!ctx3.done) {
        yasl_loop_step(l, 50);
    }
    assert(ctx3.count == 2);

    /* Test 4: Timer cancellation before firing */
    struct test_ctx ctx4 = { .loop = l };
    yasl_private_timer_id cancel_id = yasl_loop_add_timer(l, 5000000ull, oneshot_cb, &ctx4);
    assert(cancel_id > 0);
    assert(yasl_loop_del_timer(l, cancel_id));
    assert(!yasl_loop_del_timer(l, cancel_id));
    /* Step for 10ms; timer should not fire */
    yasl_loop_step(l, 10);
    assert(ctx4.count == 0);

    /* Test 5: Child timer added inside callback */
    struct test_ctx ctx5 = { .loop = l };
    ctx5.id = yasl_loop_add_timer(l, 2000000ull, parent_timer_cb, &ctx5);
    while (ctx5.count < 11) {
        yasl_loop_step(l, 50);
    }
    assert(ctx5.count == 11);

    /* Test 6: Killer timer deletes victim timer before victim fires */
    struct test_ctx ctx6 = { .loop = l };
    ctx6.id = yasl_loop_add_timer(l, 1000000ull, killer_timer_cb, &ctx6);
    ctx6.other_id = yasl_loop_add_timer(l, 10000000ull, victim_timer_cb, &ctx6);
    while (ctx6.count == 0) {
        yasl_loop_step(l, 50);
    }
    assert(ctx6.count == 1);
    yasl_loop_step(l, 20);

    /* Test 8: Zero delay runs in the next step without waiting, and IDs are
     * never handed out again. */
    int zero_runs = 0;
    yasl_private_timer_id zero = yasl_loop_add_timer(l, 0, count_cb, &zero_runs);
    assert(zero > 0 && yasl_private_loop_next_timeout_ms(l) == 0);
    assert(yasl_loop_step(l, -1) == 1 && zero_runs == 1);
    yasl_private_timer_id next = yasl_loop_add_timer(l, 0, count_cb, &zero_runs);
    assert(next > zero);
    assert(!yasl_loop_del_timer(l, zero) && yasl_loop_del_timer(l, next));

    /* Test 9: A callback that grows the timer array still reschedules itself. */
    struct yasl_loop *grown = yasl_loop_create();
    assert(grown);
    struct test_ctx ctx9 = { .loop = grown };
    ctx9.id = yasl_loop_add_timer(grown, 0, grow_cb, &ctx9);
    while (ctx9.count < 2)
        assert(yasl_loop_step(grown, 50) >= 0);
    assert(!yasl_private_loop_timer_armed(grown, ctx9.id));
    assert(yasl_loop_destroy(grown) == 0);

    /* Test 7: Millisecond conversion */
    struct test_ctx ctx7 = { .loop = l };
    ctx7.id = yasl_loop_add_timer(l, YASL_PRIVATE_MS_TO_NS(5), ms_timer_cb, &ctx7);
    assert(ctx7.id > 0);
    while (!ctx7.done) {
        yasl_loop_step(l, 50);
    }
    assert(ctx7.count == 3);

    yasl_loop_destroy(l);
}

/* Surface timer tests */
static uint64_t surface_timer_repeat_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)interval_ns;
    struct test_ctx *ctx = ud;
    assert(id == ctx->id);
    ctx->count++;
    return interval_ns;
}

static uint64_t surface_suicide_cb(void *ud, yasl_private_timer_id id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct test_ctx *ctx = ud;
    ctx->count++;
    /* Destroy surface from within the timer callback */
    yasl_private_surface_destroy(ctx->surface);
    ctx->surface = NULL;
    return 1000000ull; /* Even if it returns > 0, surface is destroyed */
}

static void test_surface_timers(void)
{
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);

    struct yasl_private_display_spec spec = { .app_id = "org.yasl.timer_test" };
    struct yasl_private_display *d = yasl_private_display_open(loop, &spec);
    if (!d) {
        /* Headless without display server */
        yasl_loop_destroy(loop);
        return;
    }

    assert(yasl_private_display_loop(d) == loop);

    struct yasl_private_window_spec sspec = {
        .title = "Timer Surface",
        .size = { .x = 160, .y = 100 },
    };
    struct yasl_surface *s = yasl_private_surface_create_toplevel(d, &sspec);
    assert(s);
    assert(yasl_private_display_loop(yasl_private_surface_display(s)) == loop);

    /* Application timers use the display's loop. */
    struct test_ctx dctx = {0};
    dctx.id = yasl_loop_add_timer(yasl_private_display_loop(d), 2000000ull, oneshot_cb, &dctx);
    assert(dctx.id > 0);
    while (dctx.count == 0) {
        yasl_loop_step(loop, 50);
    }
    assert(dctx.count == 1);

    /* Surface timer basic repeat & manual delete */
    struct test_ctx sctx = {0};
    sctx.id = yasl_private_surface_add_timer(s, 2000000ull, surface_timer_repeat_cb, &sctx);
    assert(sctx.id > 0);
    while (sctx.count < 3) {
        yasl_loop_step(loop, 50);
    }
    assert(sctx.count >= 3);
    assert(yasl_private_surface_del_timer(s, sctx.id));
    int count_before = sctx.count;
    yasl_loop_step(loop, 10);
    assert(sctx.count == count_before);

    /* A fired one-shot surface timer is retired, as a loop timer is. Many
     * of them stay retired after the surface's id list compacts. */
    for (int i = 0; i < 64; i++) {
        struct test_ctx octx = {0};
        octx.id = yasl_private_surface_add_timer(s, 1000000ull, oneshot_cb, &octx);
        assert(octx.id > 0);
        while (octx.count == 0)
            yasl_loop_step(loop, 50);
        assert(!yasl_private_surface_del_timer(s, octx.id));
    }

    /* Surface timer auto-cleanup on surface destruction */
    struct test_ctx sctx2 = {0};
    sctx2.id = yasl_private_surface_add_timer(s, 2000000ull, surface_timer_repeat_cb, &sctx2);
    assert(sctx2.id > 0);
    yasl_private_surface_destroy(s);
    /* Step display/loop; timer must not fire on dead surface */
    yasl_loop_step(loop, 10);
    assert(sctx2.count == 0);

    /* Surface suicide from timer callback */
    s = yasl_private_surface_create_toplevel(d, &sspec);
    assert(s);
    struct test_ctx sctx3 = { .surface = s };
    sctx3.id = yasl_private_surface_add_timer(s, 2000000ull, surface_suicide_cb, &sctx3);
    assert(sctx3.id > 0);
    while (sctx3.count == 0) {
        yasl_loop_step(loop, 50);
    }
    assert(sctx3.count == 1);
    assert(sctx3.surface == NULL);

    yasl_private_display_close(d);
    yasl_loop_destroy(loop);
}

int main(void)
{
    /* A wait that never ends fails here, not at the suite timeout. */
    alarm(10);
    test_loop_timers();
    test_surface_timers();
    printf("Timer tests passed successfully.\n");
    return 0;
}
