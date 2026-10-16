#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "common/private_loop.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Fail instead of hanging when a wake never arrives. */
static void on_alarm(int signo)
{
    (void)signo;
    static const char msg[] = "loop test timed out\n";
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    _exit(1);
}

struct post_log {
    struct yasl_loop *loop;
    char order[8];
    int count;
};

static void post_a(void *ud);

static void post_c(void *ud)
{
    struct post_log *log = ud;
    log->order[log->count++] = 'c';
}

static void post_a(void *ud)
{
    struct post_log *log = ud;
    log->order[log->count++] = 'a';
    assert(yasl_private_loop_post(log->loop, post_c, log));
}

static void post_b(void *ud)
{
    struct post_log *log = ud;
    log->order[log->count++] = 'b';
}

static void test_post_order(void)
{
    struct yasl_loop *l = yasl_loop_create();
    assert(l);
    assert(!yasl_private_loop_post(NULL, post_b, NULL));
    assert(!yasl_private_loop_post(l, NULL, NULL));

    struct post_log log = {.loop = l};
    assert(yasl_private_loop_post(l, post_a, &log));
    assert(yasl_private_loop_post(l, post_b, &log));
    assert(yasl_loop_step(l, 0) >= 1);
    /* A post made while posts run waits for a later step. */
    assert(log.count == 2 && memcmp(log.order, "ab", 2) == 0);
    assert(yasl_loop_step(l, 0) >= 1);
    assert(log.count == 3 && log.order[2] == 'c');

    /* Destroy drops posts that never ran. */
    assert(yasl_private_loop_post(l, post_b, &log));
    yasl_loop_destroy(l);
    assert(log.count == 3);
}

struct thread_ctx {
    struct yasl_loop *loop;
    bool kick;
    int signo;
    _Atomic bool ran;
};

static void mark_ran(void *ud)
{
    struct thread_ctx *ctx = ud;
    ctx->ran = true;
}

static void *wake_thread(void *ud)
{
    struct thread_ctx *ctx = ud;
    sleep_ms(30);
    if (ctx->signo)
        kill(getpid(), ctx->signo);
    else if (ctx->kick)
        yasl_private_loop_kick(ctx->loop);
    else
        assert(yasl_private_loop_post(ctx->loop, mark_ran, ctx));
    return NULL;
}

static void test_cross_thread(void)
{
    struct yasl_loop *l = yasl_loop_create();
    assert(l);

    struct thread_ctx post = {.loop = l};
    pthread_t t;
    assert(pthread_create(&t, NULL, wake_thread, &post) == 0);
    while (!post.ran)
        assert(yasl_loop_step(l, -1) >= 0);
    pthread_join(t, NULL);

    /* A kick ends an indefinite wait without running anything. */
    struct thread_ctx kick = {.loop = l, .kick = true};
    assert(pthread_create(&t, NULL, wake_thread, &kick) == 0);
    assert(yasl_loop_step(l, -1) >= 1);
    pthread_join(t, NULL);

    yasl_loop_destroy(l);
}

struct signal_log {
    struct yasl_loop *loop;
    int seen[NSIG];
    int total;
    bool unwatch;
};

static void on_signal(void *ud, int signo)
{
    struct signal_log *log = ud;
    assert(signo > 0 && signo < NSIG);
    log->seen[signo]++;
    log->total++;
    if (log->unwatch)
        yasl_loop_unwatch_signals(log->loop);
}

static volatile sig_atomic_t previous_handler_hits;

static void previous_handler(int signo)
{
    (void)signo;
    previous_handler_hits++;
}

static void step_until(struct yasl_loop *l, const int *counter, int want)
{
    while (*counter < want)
        assert(yasl_loop_step(l, -1) >= 0);
}

static void test_signals(void)
{
    struct sigaction prev = {.sa_handler = previous_handler};
    sigemptyset(&prev.sa_mask);
    assert(sigaction(SIGUSR2, &prev, NULL) == 0);

    struct yasl_loop *l = yasl_loop_create();
    struct yasl_loop *other = yasl_loop_create();
    assert(l && other);
    struct signal_log log = {.loop = l};

    const int bad[] = {SIGKILL};
    assert(yasl_loop_watch_signals(l, bad, 1, on_signal, &log) == -EINVAL);
    const int sigs[] = {SIGUSR1, SIGUSR2, SIGCHLD};
    assert(yasl_loop_watch_signals(l, sigs, 0, on_signal, &log) == -EINVAL);
    assert(yasl_loop_watch_signals(l, sigs, 3, NULL, &log) == -EINVAL);
    assert(yasl_loop_watch_signals(l, sigs, 3, on_signal, &log) == 0);

    /* One signal belongs to one loop at a time. */
    assert(yasl_loop_watch_signals(other, sigs, 1, on_signal, &log) == -EBUSY);

    /* Deliveries before the callback runs coalesce per signal. */
    raise(SIGUSR1);
    raise(SIGUSR1);
    raise(SIGUSR2);
    assert(yasl_loop_step(l, 0) >= 1);
    assert(log.seen[SIGUSR1] == 1 && log.seen[SIGUSR2] == 1);
    assert(previous_handler_hits == 0);

    /* A signal sent from another thread wakes an indefinite wait. */
    struct thread_ctx ctx = {.loop = l, .signo = SIGUSR1};
    pthread_t t;
    assert(pthread_create(&t, NULL, wake_thread, &ctx) == 0);
    step_until(l, &log.seen[SIGUSR1], 2);
    pthread_join(t, NULL);

    /* SIGCHLD arrives on the loop thread when a child exits. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) _exit(0);
    step_until(l, &log.seen[SIGCHLD], 1);
    int status;
    assert(waitpid(child, &status, 0) == child);

    /* A loop has one watch, so a second cannot silently drop the first one's
     * signals. Unwatching restores the previous actions. */
    const int only_usr1[] = {SIGUSR1};
    assert(yasl_loop_watch_signals(l, only_usr1, 1, on_signal, &log) == -EBUSY);
    yasl_loop_unwatch_signals(l);
    assert(yasl_loop_watch_signals(l, only_usr1, 1, on_signal, &log) == 0);
    raise(SIGUSR2);
    assert(previous_handler_hits == 1);
    assert(yasl_loop_watch_signals(other, (const int[]){SIGUSR2}, 1, on_signal, &log) == 0);
    yasl_loop_unwatch_signals(other);
    assert(previous_handler_hits == 1);
    raise(SIGUSR2);
    assert(previous_handler_hits == 2);

    /* A callback may unwatch; the previous default action comes back. */
    log.unwatch = true;
    raise(SIGUSR1);
    step_until(l, &log.seen[SIGUSR1], 3);
    struct sigaction now;
    assert(sigaction(SIGUSR1, NULL, &now) == 0);
    assert(now.sa_handler == SIG_DFL);
    yasl_loop_unwatch_signals(l);

    /* Destroy unwatches. */
    log.unwatch = false;
    assert(yasl_loop_watch_signals(l, only_usr1, 1, on_signal, &log) == 0);
    assert(yasl_loop_destroy(l) == 0);
    assert(sigaction(SIGUSR1, NULL, &now) == 0);
    assert(now.sa_handler == SIG_DFL);
    yasl_loop_destroy(other);

    struct sigaction dfl = {.sa_handler = SIG_DFL};
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGUSR2, &dfl, NULL);
}

struct order_log {
    char order[8];
    int count;
};

static void order_signal(void *ud, int signo)
{
    (void)signo;
    struct order_log *log = ud;
    log->order[log->count++] = 's';
}

static void order_fd(void *ud, int fd, short revents)
{
    (void)revents;
    struct order_log *log = ud;
    char b;
    assert(read(fd, &b, 1) == 1);
    log->order[log->count++] = 'f';
}

/* A signal runs after the descriptors of its step, so a child's last output
 * is read before its exit is handled. */
static void test_signal_order(void)
{
    yasl_loop *l = yasl_loop_create();
    assert(l);
    struct order_log log = {0};
    assert(yasl_loop_watch_signals(l, (const int[]){SIGUSR1}, 1, order_signal, &log) == 0);
    int p[2];
    assert(pipe(p) == 0);
    assert(yasl_loop_add_fd(l, p[0], POLLIN, order_fd, &log) > 0);
    assert(write(p[1], "x", 1) == 1);
    raise(SIGUSR1);
    assert(yasl_loop_step(l, 0) >= 1);
    assert(log.count == 2 && memcmp(log.order, "fs", 2) == 0);
    close(p[0]);
    close(p[1]);
    assert(yasl_loop_destroy(l) == 0);
}

struct fd_log {
    int calls;
    short revents;
};

static void on_readable(void *ud, int fd, short revents)
{
    struct fd_log *log = ud;
    char b;
    if (revents & POLLIN)
        assert(read(fd, &b, 1) == 1);
    log->calls++;
    log->revents = revents;
}

static void test_fds(void)
{
    yasl_loop *l = yasl_loop_create();
    assert(l);
    int p[2];
    assert(pipe(p) == 0);
    struct fd_log log = {0};
    assert(yasl_loop_add_fd(l, -1, POLLIN, on_readable, &log) == -EINVAL);
    assert(yasl_loop_add_fd(l, p[0], POLLIN, NULL, &log) == -EINVAL);
    int id = yasl_loop_add_fd(l, p[0], POLLIN, on_readable, &log);
    assert(id > 0);
    assert(yasl_loop_step(l, 0) == 0 && log.calls == 0);
    assert(write(p[1], "x", 1) == 1);
    assert(yasl_loop_step(l, -1) == 1 && log.calls == 1 && (log.revents & POLLIN));
    /* No requested events: a ready descriptor stays quiet. */
    assert(yasl_loop_set_fd_events(l, id, 0) == 0);
    assert(write(p[1], "x", 1) == 1);
    assert(yasl_loop_step(l, 0) == 0 && log.calls == 1);
    assert(yasl_loop_set_fd_events(l, id, POLLIN) == 0);
    assert(yasl_loop_step(l, 0) == 1 && log.calls == 2);
    assert(yasl_loop_del_fd(l, id) == 0);
    assert(yasl_loop_del_fd(l, id) == -ENOENT);
    assert(yasl_loop_set_fd_events(l, id, POLLIN) == -ENOENT);
    assert(write(p[1], "x", 1) == 1);
    assert(yasl_loop_step(l, 0) == 0 && log.calls == 2);
    close(p[0]);
    close(p[1]);
    assert(yasl_loop_destroy(l) == 0);
}

struct reentry {
    yasl_loop *loop;
    int step, run;
    bool quit;
};

static uint64_t reenter(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id; (void)interval_ns;
    struct reentry *r = ud;
    r->step = yasl_loop_step(r->loop, 0);
    r->run = yasl_loop_run(r->loop);
    if (r->quit)
        yasl_loop_quit(r->loop);
    return 0;
}

static void *step_elsewhere(void *ud)
{
    return (void *)(intptr_t)yasl_loop_step(ud, 0);
}

static void test_reentry_and_quit(void)
{
    yasl_loop *l = yasl_loop_create();
    assert(l);
    /* A callback cannot step its own loop; the nested run keeps the outer quit. */
    struct reentry r = {.loop = l, .quit = true};
    assert(yasl_loop_add_timer(l, 0, reenter, &r));
    assert(yasl_loop_run(l) == 0);
    assert(r.step == -EBUSY && r.run == -EBUSY);

    /* A quit before the run ends it at once, and the run clears it. */
    yasl_loop_quit(l);
    assert(yasl_loop_run(l) == 0);
    r = (struct reentry){.loop = l, .quit = true};
    assert(yasl_loop_add_timer(l, YASL_PRIVATE_MS_TO_NS(5), reenter, &r));
    assert(yasl_loop_run(l) == 0 && r.step == -EBUSY);

    /* Only the creating thread may step. */
    pthread_t t;
    void *result;
    assert(pthread_create(&t, NULL, step_elsewhere, l) == 0);
    pthread_join(t, &result);
    assert((intptr_t)result == -EPERM);
    assert(yasl_loop_destroy(l) == 0);
}

static void on_interrupt(int signo)
{
    (void)signo;
}

struct interrupter {
    pthread_t target;
    int delay_ms;
};

static void *interrupt_thread(void *ud)
{
    struct interrupter *i = ud;
    sleep_ms(i->delay_ms);
    pthread_kill(i->target, SIGUSR2);
    return NULL;
}

/* An interrupted wait resumes with the time left, not the whole timeout. */
static void test_interrupted_wait(void)
{
    struct sigaction sa = {.sa_handler = on_interrupt};
    sigemptyset(&sa.sa_mask);
    struct sigaction old;
    assert(sigaction(SIGUSR2, &sa, &old) == 0);
    yasl_loop *l = yasl_loop_create();
    assert(l);
    struct interrupter i = {.target = pthread_self(), .delay_ms = 60};
    pthread_t t;
    uint64_t start = yasl_time_ns();
    assert(pthread_create(&t, NULL, interrupt_thread, &i) == 0);
    assert(yasl_loop_step(l, 100) == 0);
    uint64_t elapsed_ms = (yasl_time_ns() - start) / 1000000u;
    pthread_join(t, NULL);
    /* Lateness under load is not the bug; an early return is. */
    assert(elapsed_ms >= 90 && elapsed_ms < 1000);
    assert(yasl_loop_destroy(l) == 0);
    sigaction(SIGUSR2, &old, NULL);
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(10);
    test_post_order();
    test_cross_thread();
    test_signals();
    test_signal_order();
    test_fds();
    test_reentry_and_quit();
    test_interrupted_wait();
    printf("Loop tests passed.\n");
    return 0;
}
