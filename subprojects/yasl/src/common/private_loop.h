/**
 * @file
 * The internal parts of the loop. The loop itself is public: see yasl_loop_* in
 * yasl.h. Use the loop's owning thread for all calls and callbacks, except
 * yasl_private_loop_now_ns(), yasl_private_loop_kick(), yasl_private_loop_wake()
 * and yasl_private_loop_post(), which any thread may call.
 */
#ifndef YASL_PRIVATE_LOOP_H
#define YASL_PRIVATE_LOOP_H

#include "common/types.h"
#include "yasl/yasl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Timer identity scoped to its loop. Zero is invalid; IDs are never reissued. */
typedef uint64_t yasl_private_timer_id;
/** Timer callback. interval_ns is the last scheduled delay; return the next delay or zero to stop. */
typedef uint64_t (*yasl_private_timer_cb)(void *ud, yasl_private_timer_id id, uint64_t interval_ns);

/** Convert a nonnegative integer millisecond value to nanoseconds; avoid overflow. */
#define YASL_PRIVATE_MS_TO_NS(ms) ((uint64_t)(ms) * 1000000ull)

/**
 * How a display drives the loop it runs on. Each step runs prepare, the wait,
 * check, deliver, then descriptor callbacks and timers.
 * prepare: dispatch what the backend already holds, draw due surfaces, flush
 *   requests, and arm the read. True when progress is possible now, so the
 *   wait must not block.
 * wait: replaces poll(2) for platforms whose events do not arrive on a
 *   descriptor; NULL uses poll. The loop reads descriptor readiness after it.
 * check: runs after every wait, failed or not, before any callback; it closes
 *   what prepare opened.
 * deliver: optional; hands queued events to the application.
 */
struct yasl_private_loop_driver {
    bool (*prepare)(void *ud);
    int (*wait)(void *ud, int timeout_ms);
    void (*check)(void *ud);
    void (*deliver)(void *ud);
    void *ud;
};
/** Install a driver, or remove it with NULL. False with EBUSY when one is set. */
bool yasl_private_loop_set_driver(yasl_loop *l, const struct yasl_private_loop_driver *driver);
/** True on the thread that created the loop. */
bool yasl_private_loop_owned(const yasl_loop *l);
/** True while a step runs. */
bool yasl_private_loop_stepping(const yasl_loop *l);
/** The revents the last wait reported for watch id, until its callback runs. */
short yasl_private_loop_revents(const yasl_loop *l, int id);

/** Wake a waiting loop from any thread or a signal handler. Kicks coalesce. */
void yasl_private_loop_kick(yasl_loop *l);
/** Kick, but only from another thread: the owner's next step sees its own work. */
void yasl_private_loop_wake(yasl_loop *l);
/**
 * Run fn on the loop's owning thread during a later step. Any thread, not a
 * signal handler. False for NULL arguments or allocation failure. Functions run
 * in posting order; one posted while posts run waits for a later step.
 */
bool yasl_private_loop_post(yasl_loop *l, void (*fn)(void *ud), void *ud);
/** CLOCK_MONOTONIC nanoseconds. Any thread. */
uint64_t yasl_private_loop_now_ns(void);

/**
 * Get the loop used for a display's file watches and application timers.
 * @param d Display, or NULL.
 * @return Borrowed loop, or NULL. Do not destroy a display-owned loop yourself.
 */
yasl_loop *yasl_private_display_loop(struct yasl_private_display *d);

/** Whether id is armed: added, not deleted, and not retired by its callback. */
bool yasl_private_loop_timer_armed(const yasl_loop *l, yasl_private_timer_id id);
/**
 * Schedule a timer canceled automatically on surface destruction.
 * @param s Surface whose display loop will run the timer.
 * @param interval_ns Initial delay in nanoseconds.
 * @param cb Required timer callback, with the same rules as yasl_loop_add_timer().
 * @param ud Borrowed userdata until the timer ends or the surface is destroyed.
 * @return Nonzero timer ID, or zero on failure.
 * The surface does not own userdata. Cancel with yasl_private_surface_del_timer().
 */
yasl_private_timer_id yasl_private_surface_add_timer(struct yasl_surface *s, uint64_t interval_ns,
                                             yasl_private_timer_cb cb, void *ud);
/**
 * Cancel a timer registered through yasl_private_surface_add_timer().
 * @param s Owning surface.
 * @param id Timer ID.
 * @return true if still armed and removed; false if unknown or already retired.
 */
bool yasl_private_surface_del_timer(struct yasl_surface *s, yasl_private_timer_id id);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_LOOP_H */
