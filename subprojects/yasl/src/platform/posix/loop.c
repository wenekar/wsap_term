#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "common/loop.h"
#include "platform/sync.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define YASL_PRIVATE_LOOP_INIT_CAP 32

typedef void (*fd_fn)(void *ud, int fd, short revents);

struct fd_source {
    fd_fn fn;
    void *ud;
    int id;
    int fd;
    short events;
};

struct timer_source {
    yasl_private_timer_cb fn;
    void *ud;
    uint64_t interval_ns;
    uint64_t deadline_ns;
    uint64_t id;
    bool deleted;
};

struct post {
    void (*fn)(void *ud);
    void *ud;
    struct post *next;
};

/* Signals this loop watches; the previous actions are restored on unwatch. */
struct signal_watch {
    void (*cb)(void *ud, int signo);
    void *ud;
    int count;
    int signals[NSIG];
    struct sigaction old[NSIG];
};

struct yasl_loop {
    struct fd_source *fds;
    struct pollfd *pollfds;
    int count;
    int capacity;
    int next_id;
    pthread_t owner;
    /* Set by yasl_loop_quit; run clears it when it returns. */
    bool quit;
    bool stepping;
    int wake_rfd;
    int wake_wfd;
    int wake_watch;

    struct timer_source *timers;
    int timer_count;
    int timer_capacity;
    uint64_t next_timer_id;
    int timer_firing_depth;

    yasl_private_mutex post_mutex;
    struct post *post_head;
    struct post *post_tail;

    struct signal_watch *signals;

    struct yasl_private_loop_driver driver;
    bool has_driver;
};

/* Handlers run on any thread, so they only touch lock-free atomics: they mark
 * the signal pending and write to the owning loop's wake pipe. The pipe is
 * stored as fd + 1 so that the zero initializer means no pipe. */
static atomic_int signal_wake_fd[NSIG];
static atomic_bool signal_pending[NSIG];
static struct yasl_loop *signal_owner[NSIG];

uint64_t yasl_private_loop_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void run_posts(struct yasl_loop *l)
{
    yasl_private_mutex_lock(&l->post_mutex);
    struct post *p = l->post_head;
    l->post_head = l->post_tail = NULL;
    yasl_private_mutex_unlock(&l->post_mutex);
    while (p) {
        struct post *next = p->next;
        p->fn(p->ud);
        free(p);
        p = next;
    }
}

static void run_signals(struct yasl_loop *l)
{
    /* A callback may unwatch, so re-check the watch before each delivery. */
    for (int signo = 1; signo < NSIG; signo++) {
        struct signal_watch *w = l->signals;
        if (!w) return;
        if (signal_owner[signo] != l || !atomic_exchange(&signal_pending[signo], false))
            continue;
        w->cb(w->ud, signo);
    }
}

/* Only wakes the wait; the step runs posts and signals last. */
static void loop_wake_cb(void *ud, int fd, short revents)
{
    (void)ud; (void)revents;
    char buf[128];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR)) {}
}

static bool open_wake_pipe(int p[2])
{
#if defined(__linux__) && defined(O_CLOEXEC)
    return pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0;
#else
    if (pipe(p) != 0) return false;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(p[i], F_GETFL, 0);
        if (flags < 0 || fcntl(p[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(p[i], F_SETFD, FD_CLOEXEC) < 0) {
            int err = errno;
            close(p[0]);
            close(p[1]);
            errno = err;
            return false;
        }
    }
    return true;
#endif
}

yasl_loop *yasl_loop_create(void)
{
    struct yasl_loop *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->capacity = YASL_PRIVATE_LOOP_INIT_CAP;
    l->fds = malloc((size_t)l->capacity * sizeof(*l->fds));
    l->pollfds = malloc((size_t)l->capacity * sizeof(*l->pollfds));
    int p[2];
    int err = 0;
    if (!l->fds || !l->pollfds)
        err = ENOMEM;
    else if ((err = yasl_private_mutex_init(&l->post_mutex)) == 0 && !open_wake_pipe(p)) {
        err = errno;
        yasl_private_mutex_destroy(&l->post_mutex);
    }
    if (err) {
        free(l->fds);
        free(l->pollfds);
        free(l);
        errno = err;
        return NULL;
    }
    l->owner = pthread_self();
    l->next_id = 1;
    l->next_timer_id = 1;
    l->wake_rfd = p[0];
    l->wake_wfd = p[1];
    /* The first watch fits the initial capacity, so it cannot fail. */
    l->wake_watch = yasl_loop_add_fd(l, l->wake_rfd, POLLIN, loop_wake_cb, l);
    return l;
}

int yasl_loop_destroy(yasl_loop *l)
{
    if (!l) return 0;
    if (l->stepping || l->has_driver) return -EBUSY;
    yasl_loop_unwatch_signals(l);
    for (struct post *p = l->post_head; p;) {
        struct post *next = p->next;
        free(p);
        p = next;
    }
    yasl_private_mutex_destroy(&l->post_mutex);
    close(l->wake_rfd);
    close(l->wake_wfd);
    free(l->fds);
    free(l->pollfds);
    free(l->timers);
    free(l);
    return 0;
}

int yasl_loop_add_fd(yasl_loop *l, int fd, short events,
                     void (*fn)(void *ud, int fd, short revents), void *ud)
{
    if (!l || fd < 0 || !fn) return -EINVAL;
    if (l->count >= l->capacity) {
        int new_cap = l->capacity * 2;
        struct fd_source *new_fds = realloc(l->fds, (size_t)new_cap * sizeof(*new_fds));
        if (!new_fds) return -ENOMEM;
        l->fds = new_fds;
        struct pollfd *new_pollfds = realloc(l->pollfds, (size_t)new_cap * sizeof(*new_pollfds));
        if (!new_pollfds) return -ENOMEM;
        l->pollfds = new_pollfds;
        l->capacity = new_cap;
    }
    int id = l->next_id++;
    if (l->next_id <= 0) l->next_id = 1;
    l->fds[l->count] = (struct fd_source){.fn = fn, .ud = ud, .id = id, .fd = fd, .events = events};
    l->pollfds[l->count] = (struct pollfd){.fd = fd, .events = events};
    l->count++;
    return id;
}

static int fd_index(const struct yasl_loop *l, int id)
{
    for (int i = 0; l && id > 0 && i < l->count; i++)
        if (l->fds[i].id == id)
            return i;
    return -1;
}

int yasl_loop_del_fd(yasl_loop *l, int id)
{
    int i = fd_index(l, id);
    if (i < 0) return -ENOENT;
    l->count--;
    l->fds[i] = l->fds[l->count];
    l->pollfds[i] = l->pollfds[l->count];
    return 0;
}

int yasl_loop_set_fd_events(yasl_loop *l, int id, short events)
{
    int i = fd_index(l, id);
    if (i < 0) return -ENOENT;
    l->fds[i].events = events;
    l->pollfds[i].events = events;
    return 0;
}

short yasl_private_loop_revents(const yasl_loop *l, int id)
{
    int i = fd_index(l, id);
    return i < 0 ? 0 : l->pollfds[i].revents;
}

static void wake_write(int fd)
{
    /* A full pipe already wakes the loop, so EAGAIN needs no retry. */
    int saved = errno;
    char b = 1;
    ssize_t n;
    do {
        n = write(fd, &b, 1);
    } while (n < 0 && errno == EINTR);
    errno = saved;
}

void yasl_private_loop_kick(yasl_loop *l)
{
    if (l) wake_write(l->wake_wfd);
}

void yasl_private_loop_wake(yasl_loop *l)
{
    if (l && !pthread_equal(l->owner, pthread_self()))
        wake_write(l->wake_wfd);
}

bool yasl_private_loop_post(yasl_loop *l, void (*fn)(void *ud), void *ud)
{
    if (!l || !fn) return false;
    struct post *p = malloc(sizeof(*p));
    if (!p) return false;
    *p = (struct post){.fn = fn, .ud = ud};
    yasl_private_mutex_lock(&l->post_mutex);
    if (l->post_tail)
        l->post_tail->next = p;
    else
        l->post_head = p;
    l->post_tail = p;
    yasl_private_mutex_unlock(&l->post_mutex);
    wake_write(l->wake_wfd);
    return true;
}

static void signal_handler(int signo)
{
    atomic_store(&signal_pending[signo], true);
    int fd = atomic_load(&signal_wake_fd[signo]) - 1;
    if (fd >= 0) wake_write(fd);
}

static bool signal_valid(int signo)
{
    return signo > 0 && signo < NSIG && signo != SIGKILL && signo != SIGSTOP;
}

int yasl_loop_watch_signals(yasl_loop *l, const int *signals, int n,
                            void (*cb)(void *ud, int signo), void *ud)
{
    if (!l || !signals || n <= 0 || !cb) return -EINVAL;
    if (l->signals) return -EBUSY;
    bool wanted[NSIG] = {0};
    for (int i = 0; i < n; i++) {
        if (!signal_valid(signals[i])) return -EINVAL;
        if (signal_owner[signals[i]]) return -EBUSY;
        wanted[signals[i]] = true;
    }
    struct signal_watch *w = calloc(1, sizeof(*w));
    if (!w) return -ENOMEM;
    w->cb = cb;
    w->ud = ud;
    struct sigaction sa = {.sa_handler = signal_handler, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    for (int signo = 1; signo < NSIG; signo++) {
        if (!wanted[signo]) continue;
        atomic_store(&signal_pending[signo], false);
        atomic_store(&signal_wake_fd[signo], l->wake_wfd + 1);
        if (sigaction(signo, &sa, &w->old[w->count]) != 0) {
            int err = errno;
            atomic_store(&signal_wake_fd[signo], 0);
            for (int i = 0; i < w->count; i++) {
                sigaction(w->signals[i], &w->old[i], NULL);
                atomic_store(&signal_wake_fd[w->signals[i]], 0);
            }
            free(w);
            return -err;
        }
        w->signals[w->count++] = signo;
    }
    for (int i = 0; i < w->count; i++)
        signal_owner[w->signals[i]] = l;
    l->signals = w;
    return 0;
}

void yasl_loop_unwatch_signals(yasl_loop *l)
{
    if (!l || !l->signals) return;
    struct signal_watch *w = l->signals;
    l->signals = NULL;
    for (int i = 0; i < w->count; i++) {
        int signo = w->signals[i];
        sigaction(signo, &w->old[i], NULL);
        atomic_store(&signal_wake_fd[signo], 0);
        atomic_store(&signal_pending[signo], false);
        signal_owner[signo] = NULL;
    }
    free(w);
}

static void timer_cleanup_deleted(struct yasl_loop *l)
{
    if (l->timer_firing_depth > 0) return;
    int dest = 0;
    for (int src = 0; src < l->timer_count; src++) {
        struct timer_source t = l->timers[src];
        l->timers[dest] = t;
        dest += !t.deleted;
    }
    l->timer_count = dest;
}

bool yasl_private_loop_timer_armed(const yasl_loop *l, yasl_private_timer_id id)
{
    if (!l || id == 0) return false;
    for (int i = 0; i < l->timer_count; i++)
        if (l->timers[i].id == id)
            return !l->timers[i].deleted;
    return false;
}

int yasl_private_loop_poll_set(const yasl_loop *l, const struct pollfd **fds)
{
    if (!l) {
        *fds = NULL;
        return 0;
    }
    *fds = l->pollfds;
    return l->count;
}

int yasl_private_loop_next_timeout_ms(const yasl_loop *l)
{
    if (!l) return -1;
    uint64_t now = yasl_private_loop_now_ns();
    uint64_t min_deadline = UINT64_MAX;
    for (int i = 0; i < l->timer_count; i++)
        if (!l->timers[i].deleted && l->timers[i].deadline_ns < min_deadline)
            min_deadline = l->timers[i].deadline_ns;
    if (min_deadline == UINT64_MAX) return -1;
    if (min_deadline <= now) return 0;
    uint64_t ms = (min_deadline - now + 999999ull) / 1000000ull;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

uint64_t yasl_loop_add_timer(yasl_loop *l, uint64_t interval_ns,
                             yasl_private_timer_cb cb, void *ud)
{
    if (!l || !cb) {
        errno = EINVAL;
        return 0;
    }
    if (l->timer_count >= l->timer_capacity) {
        int new_cap = l->timer_capacity ? l->timer_capacity * 2 : 16;
        struct timer_source *new_timers = realloc(l->timers, (size_t)new_cap * sizeof(*new_timers));
        if (!new_timers) {
            errno = ENOMEM;
            return 0;
        }
        l->timers = new_timers;
        l->timer_capacity = new_cap;
    }
    /* 64 bits never wrap in practice, so an ID never names a newer timer. */
    uint64_t id = l->next_timer_id++;
    l->timers[l->timer_count++] = (struct timer_source){
        .fn = cb,
        .ud = ud,
        .id = id,
        .interval_ns = interval_ns,
        .deadline_ns = yasl_private_loop_now_ns() + interval_ns,
    };
    /* Owner-thread only, and the next wait computes its timeout from the
     * timers, so no wake is needed. */
    return id;
}

bool yasl_loop_del_timer(yasl_loop *l, uint64_t id)
{
    if (!l || id == 0) return false;
    for (int i = l->timer_count - 1; i >= 0; i--) {
        if (l->timers[i].id == id && !l->timers[i].deleted) {
            l->timers[i].deleted = true;
            timer_cleanup_deleted(l);
            return true;
        }
    }
    return false;
}

bool yasl_private_loop_set_driver(yasl_loop *l, const struct yasl_private_loop_driver *driver)
{
    if (!l) return false;
    if (driver && l->has_driver) {
        errno = EBUSY;
        return false;
    }
    l->driver = driver ? *driver : (struct yasl_private_loop_driver){0};
    l->has_driver = driver != NULL;
    return true;
}

bool yasl_private_loop_owned(const yasl_loop *l)
{
    return l && pthread_equal(l->owner, pthread_self());
}

bool yasl_private_loop_stepping(const yasl_loop *l)
{
    return l && l->stepping;
}

/* poll with an EINTR retry that keeps the deadline. */
static int poll_all(struct yasl_loop *l, int timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? yasl_private_loop_now_ns() + YASL_PRIVATE_MS_TO_NS(timeout_ms) : 0;
    for (;;) {
        if (poll(l->pollfds, (nfds_t)l->count, timeout_ms) >= 0) return 0;
        if (errno != EINTR) return -errno;
        if (timeout_ms > 0) {
            uint64_t now = yasl_private_loop_now_ns();
            timeout_ms = now >= deadline ? 0 : (int)((deadline - now + 999999ull) / 1000000ull);
        }
    }
}

static int dispatch_fds(struct yasl_loop *l)
{
    int handled = 0;
    int count = l->count;
    struct fd_source sstack[32];
    struct pollfd pstack[32];
    struct fd_source *scopy = count <= 32 ? sstack : malloc((size_t)count * sizeof(*scopy));
    struct pollfd *pcopy = count <= 32 ? pstack : malloc((size_t)count * sizeof(*pcopy));
    if (!scopy || !pcopy) {
        if (scopy != sstack) free(scopy);
        if (pcopy != pstack) free(pcopy);
        return -ENOMEM;
    }
    memcpy(scopy, l->fds, (size_t)count * sizeof(*scopy));
    memcpy(pcopy, l->pollfds, (size_t)count * sizeof(*pcopy));
    for (int i = 0; i < count; i++) {
        if (!pcopy[i].revents) continue;
        /* A callback may have removed or moved this watch. */
        int j = i < l->count && l->fds[i].id == scopy[i].id ? i : fd_index(l, scopy[i].id);
        if (j < 0) continue;
        l->pollfds[j].revents = 0;
        scopy[i].fn(scopy[i].ud, scopy[i].fd, pcopy[i].revents);
        handled++;
    }
    if (scopy != sstack) free(scopy);
    if (pcopy != pstack) free(pcopy);
    return handled;
}

static void reschedule(struct timer_source *t, uint64_t next, uint64_t now)
{
    if (next == 0 || t->deleted) {
        t->deleted = true;
        return;
    }
    t->interval_ns = next;
    t->deadline_ns = t->deadline_ns + next > now ? t->deadline_ns + next : now + next;
}

static int fire_timers(struct yasl_loop *l)
{
    int handled = 0;
    uint64_t now = yasl_private_loop_now_ns();
    l->timer_firing_depth++;
    /* Timers added by a callback wait for the next step. */
    int initial_count = l->timer_count;
    for (int i = 0; i < initial_count && i < l->timer_count; i++) {
        if (l->timers[i].deleted || l->timers[i].deadline_ns > now)
            continue;
        struct timer_source t = l->timers[i];
        uint64_t next = t.fn(t.ud, t.id, t.interval_ns);
        handled++;
        /* Compaction waits for depth zero, so index i still holds this timer. */
        reschedule(&l->timers[i], next, now);
    }
    l->timer_firing_depth--;
    timer_cleanup_deleted(l);
    return handled;
}

static int step(struct yasl_loop *l, int timeout_ms)
{
    /* A driver cannot change during a step: its display cannot close then. */
    const struct yasl_private_loop_driver *d = l->has_driver ? &l->driver : NULL;
    if (d && d->prepare(d->ud))
        timeout_ms = 0;
    int timer_to = yasl_private_loop_next_timeout_ms(l);
    if (timer_to >= 0 && (timeout_ms < 0 || timer_to < timeout_ms))
        timeout_ms = timer_to;
    int r;
    if (d && d->wait) {
        r = d->wait(d->ud, timeout_ms);
        /* The platform wait does not fill revents; read them now. */
        if (r >= 0) r = poll_all(l, 0);
    } else {
        r = poll_all(l, timeout_ms);
    }
    /* check closes what prepare opened, whatever the wait returned. */
    if (d) d->check(d->ud);
    if (r < 0) return r;
    if (d && d->deliver) d->deliver(d->ud);
    int fds = dispatch_fds(l);
    if (fds < 0) return fds;
    int timers = fire_timers(l);
    /* Last, so a signal sees what came with it: a child's final output is
     * read before its exit is handled. */
    run_posts(l);
    run_signals(l);
    return fds + timers;
}

int yasl_loop_step(yasl_loop *l, int timeout_ms)
{
    if (!l) return -EINVAL;
    if (!pthread_equal(l->owner, pthread_self())) return -EPERM;
    if (l->stepping) return -EBUSY;
    l->stepping = true;
    int r = step(l, timeout_ms);
    l->stepping = false;
    return r;
}

int yasl_loop_run(yasl_loop *l)
{
    if (!l) return -EINVAL;
    if (!pthread_equal(l->owner, pthread_self())) return -EPERM;
    /* A nested run must not consume the outer run's quit. */
    if (l->stepping) return -EBUSY;
    int r = 0;
    while (!l->quit) {
        r = yasl_loop_step(l, -1);
        if (r < 0) break;
    }
    l->quit = false;
    return r < 0 ? r : 0;
}

void yasl_loop_quit(yasl_loop *l)
{
    if (l) l->quit = true;
}
