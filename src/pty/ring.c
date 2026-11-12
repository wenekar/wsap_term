#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "platform/platform.h"
#include "pty/ring.h"
#include "pty/sync.h"
#include "util/proc.h"
#include "yasl/yasl.h"

#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

static void event_signal(int fd)
{
    uint64_t one = 1;
    ssize_t n = write(fd, &one, sizeof(one));
    (void)n;
}

static void drain_fd(int fd)
{
    char drain[64];
    while (read(fd, drain, sizeof(drain)) > 0) {}
}

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#endif
}

#define PTY_READ_BATCH_SIZE (256 * 1024)
#define PTY_END_DRAIN_LIMIT (4 * 1024 * 1024)
#define PTY_SPIN_ITERS 128
#define PTY_SYNC_REQUEST_MS 100
#define PTY_POLL_INTERVAL_NS 100000ull
#define PTY_OUTPUT_INTERVAL_NS 1000000ull

enum { PFD_SRC0, PFD_SRC1, PFD_SPACE, PFD_SYNC, PFD_COUNT };

enum pump_result { PUMP_EMPTY, PUMP_FULL, PUMP_EOF };

struct worker {
    struct pty_ring *r;
    struct pollfd pfd[PFD_COUNT];
    size_t len;
    unsigned src;
    bool end_done;
    bool dirty;
    uint64_t notify_after;
};

static enum pump_result pump(struct worker *w, unsigned i)
{
    struct pty_ring *r = w->r;
    int fd = r->src_fd[i];
    enum pump_result result = PUMP_FULL;
#if defined(__APPLE__)
    size_t start = w->len;
    bool short_read = false;
    int spin = 0;
#endif
    for (;;) {
        size_t to_read = PTY_READ_BATCH_SIZE - w->len;
        if (to_read == 0)
            break;

        ssize_t n = read(fd, r->buf + w->len, to_read);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
#if defined(__APPLE__)
                if (w->len != start && !short_read &&
                    ++spin <= PTY_SPIN_ITERS) {
                    cpu_relax();
                    continue;
                }
#endif
                result = PUMP_EMPTY;
                break;
            }
            if (n < 0 && errno == EINTR)
                continue;
            result = PUMP_EOF;
            break;
        }

#if defined(__APPLE__)
        spin = 0;
        short_read = (size_t)n < to_read && n < 1024;
#endif
        w->src = i;
        w->len += (size_t)n;
    }

    return result;
}

static void close_source(struct worker *w, unsigned i)
{
    w->pfd[i].fd = -1;
}

static void drain_batch(struct worker *w);
static void notify_drained(struct worker *w);

static void drain_source(struct worker *w, unsigned i)
{
    while (w->pfd[i].fd >= 0) {
        enum pump_result res = pump(w, i);
        drain_batch(w);
        notify_drained(w);
        if (res == PUMP_EMPTY)
            return;
        if (res == PUMP_EOF) {
            close_source(w, i);
            return;
        }
        if (atomic_load_explicit(&w->r->stop, memory_order_relaxed))
            return;
    }
}

static void drain_now(struct worker *w, unsigned i)
{
    size_t total = 0;
    while (w->pfd[i].fd >= 0 && total < PTY_END_DRAIN_LIMIT) {
        size_t before = w->len;
        enum pump_result res = pump(w, i);
        total += w->len - before;
        drain_batch(w);
        notify_drained(w);
        if (res == PUMP_EMPTY)
            return;
        if (res == PUMP_EOF) {
            close_source(w, i);
            return;
        }
        if (atomic_load_explicit(&w->r->stop, memory_order_relaxed))
            return;
    }
}

static void serve_end(struct worker *w)
{
    if (w->end_done || !atomic_load(&w->r->end_requested))
        return;
    drain_now(w, PFD_SRC0);
    drain_now(w, PFD_SRC1);
    drain_batch(w);
    notify_drained(w);
    w->end_done = true;
    if (w->r->cbs.ended && !w->r->ended) {
        w->r->ended = true;
        w->r->cbs.ended(w->r->ud);
        if (!w->r->cbs.drained && w->r->event_wr >= 0)
            event_signal(w->r->event_wr);
    }
}

static bool sync_peer_trusted(int fd)
{
#if defined(__linux__)
    struct ucred cred;
    socklen_t len = sizeof(cred);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
           cred.uid == getuid();
#else
    struct xucred cred;
    socklen_t len = sizeof(cred);
    return getsockopt(fd, 0, LOCAL_PEERCRED, &cred, &len) == 0 &&
           cred.cr_uid == getuid();
#endif
}

static void serve_sync(struct worker *w)
{
    for (;;) {
#if defined(__linux__)
        int c = accept4(w->r->sync_fd, NULL, NULL, SOCK_CLOEXEC);
#else
        int c = accept(w->r->sync_fd, NULL, NULL);
        if (c >= 0)
            fcntl(c, F_SETFD, FD_CLOEXEC);
#endif
        if (c < 0)
            return;
        char req = 0;
        struct pollfd pfd = { .fd = c, .events = POLLIN };
        if (sync_peer_trusted(c) &&
            poll(&pfd, 1, PTY_SYNC_REQUEST_MS) == 1 &&
            recv(c, &req, 1, 0) == 1 &&
            (req == PTY_SYNC_LEAVE_PIPE || req == PTY_SYNC_LEAVE_TTY)) {
            drain_source(w, req == PTY_SYNC_LEAVE_PIPE ? 0 : 1);
            drain_batch(w);
            (void)!send(c, &req, 1, PTY_SYNC_SEND_FLAGS);
        }
        close(c);
    }
}

static void drain_batch(struct worker *w)
{
    struct pty_ring *r = w->r;
    size_t len = w->len;
    w->len = 0;
    if (!len || atomic_load_explicit(&r->stop, memory_order_relaxed))
        return;
    r->cbs.data(r->ud, r->src_fd[w->src], r->buf, len, 0);
    w->dirty = true;
}

static void notify_drained(struct worker *w)
{
    struct pty_ring *r = w->r;
    if (!w->dirty)
        return;
    w->dirty = false;
    if (r->cbs.drained)
        r->cbs.drained(r->ud);
    else if (r->event_wr >= 0)
        event_signal(r->event_wr);
    w->notify_after = yasl_time_ns() + PTY_OUTPUT_INTERVAL_NS;
}

static void *pty_ring_worker(void *arg)
{
    struct pty_ring *r = arg;
    platform_thread_interactive();
    struct worker w = {
        .r = r,
        .pfd = {
            [PFD_SRC0] = { .fd = r->src_fd[0], .events = POLLIN },
            [PFD_SRC1] = { .fd = r->src_fd[1], .events = POLLIN },
            [PFD_SPACE] = { .fd = r->space_fd, .events = POLLIN },
            [PFD_SYNC] = { .fd = r->sync_fd, .events = POLLIN },
        },
    };
    /* Retry full batches until the other sources need a poll. */
    bool active = false;
    uint64_t next_poll = 0;

    while (!atomic_load_explicit(&r->stop, memory_order_relaxed) &&
           (w.pfd[PFD_SRC0].fd >= 0 || w.pfd[PFD_SRC1].fd >= 0)) {
        bool polled = !active || yasl_time_ns() >= next_poll;
        if (polled) {
            if (!active || yasl_time_ns() >= w.notify_after)
                notify_drained(&w);
            int pr = poll(w.pfd, PFD_COUNT, active ? 0 : -1);
            if (pr < 0 && errno != EINTR)
                break;
            active = false;
            if (pr <= 0)
                continue;
            next_poll = yasl_time_ns() + PTY_POLL_INTERVAL_NS;
            if (w.pfd[PFD_SPACE].revents & POLLIN)
                drain_fd(r->space_fd);
        }
        if (atomic_load_explicit(&r->stop, memory_order_relaxed))
            break;
        serve_end(&w);
        active = false;
        for (unsigned i = PFD_SRC0; i <= PFD_SRC1; i++) {
            if (w.pfd[i].fd >= 0 &&
                (w.pfd[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                enum pump_result res = pump(&w, i);
                drain_batch(&w);
                if (res == PUMP_EOF)
                    close_source(&w, i);
                if (res == PUMP_FULL)
                    active = true;
                else
                    w.pfd[i].revents = 0;
            }
        }
        if (polled && (w.pfd[PFD_SYNC].revents & POLLIN))
            serve_sync(&w);
    }
    drain_batch(&w);
    notify_drained(&w);
    if (!w.end_done && w.r->cbs.ended && !w.r->ended) {
        w.r->ended = true;
        w.r->cbs.ended(w.r->ud);
        if (!w.r->cbs.drained && w.r->event_wr >= 0)
            event_signal(w.r->event_wr);
    }
    atomic_store(&r->worker_done, 1);
    return NULL;
}

static void on_ring_event(void *ud, int fd, short revents)
{
    (void)revents;
    (void)ud;
    drain_fd(fd);
}

static void ring_release(struct pty_ring *r)
{
    if (r->event_watch > 0)
        yasl_loop_del_fd(r->loop, r->event_watch);
    r->event_watch = 0;
    proc_eventfd_close(r->event_fd, r->event_wr);
    r->event_fd = r->event_wr = -1;
    proc_eventfd_close(r->space_fd, r->space_wr);
    r->space_fd = r->space_wr = -1;
    if (r->buf) {
        munmap(r->buf, PTY_READ_BATCH_SIZE);
        r->buf = NULL;
    }
    r->active = false;
}

int pty_ring_start(struct pty_ring *r, struct yasl_loop *loop, const struct pty *p,
                   const struct pty_ring_cbs *cbs, void *ud)
{
    int pipe_fd = pty_pipe_fd(p);
    int master = pty_master_fd(p);
    *r = (struct pty_ring){
        .loop = loop,
        .src_fd = { pipe_fd >= 0 ? pipe_fd : master,
                    pipe_fd >= 0 ? master : -1 },
        .sync_fd = pty_sync_fd(p),
        .event_fd = -1,
        .event_wr = -1,
        .space_fd = -1,
        .space_wr = -1,
        .cbs = *cbs,
        .ud = ud,
    };
    if (r->src_fd[0] < 0)
        return -1;
    r->buf = mmap(NULL, PTY_READ_BATCH_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!r->buf || r->buf == MAP_FAILED) {
        r->buf = NULL;
        return -1;
    }
    platform_madv_dontfork(r->buf, PTY_READ_BATCH_SIZE);
    platform_madv_nohugepage(r->buf, PTY_READ_BATCH_SIZE);
    r->space_fd = platform_eventfd(&r->space_wr);
    if (r->space_fd < 0)
        goto fail;
    if (!cbs->drained) {
        r->event_fd = platform_eventfd(&r->event_wr);
        if (r->event_fd < 0)
            goto fail;
        r->event_watch = yasl_loop_add_fd(loop, r->event_fd, POLLIN, on_ring_event, r);
        if (r->event_watch <= 0)
            goto fail;
    }
    if (pthread_create(&r->thread, NULL, pty_ring_worker, r) != 0)
        goto fail;
    r->active = true;
    return 0;

fail:
    ring_release(r);
    return -1;
}

void pty_ring_stop(struct pty_ring *r)
{
    if (!r->active)
        return;
    atomic_store(&r->stop, 1);
    if (r->event_wr >= 0)
        event_signal(r->event_wr);
    if (r->space_wr >= 0)
        event_signal(r->space_wr);
    pthread_join(r->thread, NULL);
    ring_release(r);
}

void pty_ring_end(struct pty_ring *r)
{
    if (!r->active)
        return;
    atomic_store(&r->end_requested, 1);
    if (r->space_wr >= 0)
        event_signal(r->space_wr);
    if (atomic_load(&r->worker_done) && r->cbs.ended && !r->ended) {
        r->ended = true;
        r->cbs.ended(r->ud);
        if (r->event_wr >= 0)
            event_signal(r->event_wr);
    }
}

bool pty_ring_owns_fd(const struct pty_ring *r, int fd)
{
    return r->active && fd >= 0 && (r->src_fd[0] == fd || r->src_fd[1] == fd);
}
