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

#define PTY_RING_MAX_CHUNK (256 * 1024)
#define PTY_RING_MIN_FREE (64 * 1024)
#define PTY_SIGNAL_INTERVAL_NS 1000000ull
#define PTY_SIGNAL_BYTES 131072ull
#define PTY_SPIN_ITERS 128
#define PTY_SYNC_REQUEST_MS 100

enum { PFD_SRC0, PFD_SRC1, PFD_SPACE, PFD_SYNC, PFD_COUNT };

enum pump_result { PUMP_EMPTY, PUMP_FULL, PUMP_EOF };

struct worker {
    struct pty_ring *r;
    struct pollfd pfd[PFD_COUNT];
    size_t head;
    size_t switch_head;
    uint64_t signal_after_ns;
    size_t pending_bytes;
    unsigned src;
    bool signal_deferred;
    bool end_done;
};

static bool ring_low(size_t head, size_t tail, size_t switch_head,
                     size_t switch_tail)
{
    return PTY_RING_SIZE - (head - tail) < PTY_RING_MIN_FREE ||
           switch_head - switch_tail >= PTY_RING_SWITCHES;
}

static void signal_consumer(struct worker *w, uint64_t now)
{
    w->signal_deferred = false;
    w->signal_after_ns = now + PTY_SIGNAL_INTERVAL_NS;
    w->pending_bytes = 0;
    event_signal(w->r->event_wr);
}

static void flush_deferred(struct worker *w)
{
    if (!w->signal_deferred)
        return;
    w->signal_deferred = false;
    w->pending_bytes = 0;
    if (!atomic_load_explicit(&w->r->awake, memory_order_acquire))
        event_signal(w->r->event_wr);
}

static void notify(struct worker *w, size_t bytes, bool urgent)
{
    struct pty_ring *r = w->r;
    w->pending_bytes += bytes;
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&r->awake, memory_order_acquire))
        return;
    uint64_t now = yasl_time_ns();
    if (urgent || w->pending_bytes >= PTY_SIGNAL_BYTES ||
        now >= w->signal_after_ns)
        signal_consumer(w, now);
    else
        w->signal_deferred = true;
}

static bool wait_for_space(struct worker *w)
{
    struct pty_ring *r = w->r;
    for (;;) {
        if (atomic_load_explicit(&r->stop, memory_order_relaxed))
            return false;
        atomic_store_explicit(&r->need_space, 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        size_t switch_tail =
            atomic_load_explicit(&r->switch_tail, memory_order_acquire);
        if (!ring_low(w->head, tail, w->switch_head, switch_tail)) {
            atomic_store_explicit(&r->need_space, 0, memory_order_release);
            return true;
        }
        flush_deferred(w);
        if (poll(&w->pfd[PFD_SPACE], 1, -1) < 0 && errno != EINTR)
            return false;
        drain_fd(r->space_fd);
    }
}

static enum pump_result pump(struct worker *w, unsigned i)
{
    struct pty_ring *r = w->r;
    int fd = r->src_fd[i];
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t start = w->head;
    enum pump_result result = PUMP_FULL;
    bool short_read = false;
#if defined(__APPLE__)
    int spin = 0;
#endif
    for (;;) {
        if (i != w->src &&
            w->switch_head - atomic_load_explicit(&r->switch_tail,
                                                  memory_order_acquire) >=
                PTY_RING_SWITCHES)
            break;
        size_t off = w->head & PTY_RING_MASK;
        size_t contig = PTY_RING_SIZE - off;
        size_t free_bytes = PTY_RING_SIZE - (w->head - tail);
        size_t to_read = free_bytes < contig ? free_bytes : contig;
        if (to_read > PTY_RING_MAX_CHUNK) to_read = PTY_RING_MAX_CHUNK;
        if (to_read == 0)
            break;

        ssize_t n = read(fd, r->buf + off, to_read);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
#if defined(__APPLE__)
                if (w->head != start && !short_read &&
                    ++spin <= PTY_SPIN_ITERS) {
                    cpu_relax();
                    tail = atomic_load_explicit(&r->tail,
                            memory_order_acquire);
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
#endif
        if (i != w->src) {
            r->switch_at[w->switch_head % PTY_RING_SWITCHES] = w->head;
            atomic_store_explicit(&r->switch_head, ++w->switch_head,
                                  memory_order_release);
            w->src = i;
        }
        short_read = (size_t)n < to_read && n < 1024;
        w->head += (size_t)n;
        atomic_store_explicit(&r->head, w->head, memory_order_release);
    }

    if (w->head != start || result == PUMP_EOF)
        notify(w, w->head - start, result == PUMP_EOF || short_read);
    return result;
}

static void close_source(struct worker *w, unsigned i)
{
    w->pfd[i].fd = -1;
}

static void drain_source(struct worker *w, unsigned i)
{
    while (w->pfd[i].fd >= 0) {
        enum pump_result res = pump(w, i);
        if (res == PUMP_EMPTY)
            return;
        if (res == PUMP_EOF) {
            close_source(w, i);
            return;
        }
        if (!wait_for_space(w))
            return;
    }
}

static void drain_now(struct worker *w, unsigned i)
{
    size_t start = w->head;
    while (w->pfd[i].fd >= 0 && w->head - start < PTY_RING_SIZE) {
        enum pump_result res = pump(w, i);
        if (res == PUMP_EMPTY)
            return;
        if (res == PUMP_EOF) {
            close_source(w, i);
            return;
        }
        if (!wait_for_space(w))
            return;
    }
}

static void publish_end(struct pty_ring *r, size_t head)
{
    size_t none = 0;
    atomic_compare_exchange_strong(&r->end_at, &none, head + 1);
    event_signal(r->event_wr);
}

static void serve_end(struct worker *w)
{
    if (w->end_done || !atomic_load(&w->r->end_requested))
        return;
    drain_now(w, PFD_SRC0);
    drain_now(w, PFD_SRC1);
    w->end_done = true;
    flush_deferred(w);
    publish_end(w->r, w->head);
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
            (void)!send(c, &req, 1, PTY_SYNC_SEND_FLAGS);
        }
        close(c);
    }
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

    while (!atomic_load_explicit(&r->stop, memory_order_relaxed) &&
           (w.pfd[PFD_SRC0].fd >= 0 || w.pfd[PFD_SRC1].fd >= 0)) {
        if (w.signal_deferred) {
            uint64_t now = yasl_time_ns();
            if (now >= w.signal_after_ns) {
                w.signal_deferred = false;
                if (!atomic_load_explicit(&r->awake, memory_order_acquire))
                    signal_consumer(&w, now);
            }
        }

        size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        size_t switch_tail =
            atomic_load_explicit(&r->switch_tail, memory_order_acquire);
        if (ring_low(w.head, tail, w.switch_head, switch_tail)) {
            if (!wait_for_space(&w))
                break;
            continue;
        }

        int pr = poll(w.pfd, PFD_COUNT, w.signal_deferred ? 1 : -1);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        if (w.pfd[PFD_SPACE].revents & POLLIN)
            drain_fd(r->space_fd);
        if (atomic_load_explicit(&r->stop, memory_order_relaxed))
            break;
        serve_end(&w);
        for (unsigned i = PFD_SRC0; i <= PFD_SRC1; i++) {
            if (w.pfd[i].fd >= 0 &&
                (w.pfd[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) &&
                pump(&w, i) == PUMP_EOF)
                close_source(&w, i);
        }
        if (w.pfd[PFD_SYNC].revents & POLLIN)
            serve_sync(&w);
    }
    flush_deferred(&w);
    atomic_store(&r->worker_done, 1);
    if (!atomic_load_explicit(&r->stop, memory_order_relaxed) &&
        atomic_load(&r->end_requested) && !w.end_done)
        publish_end(r, w.head);
    return NULL;
}

static void on_ring_event(void *ud, int fd, short revents)
{
    (void)revents;
    struct pty_ring *r = ud;

    drain_fd(fd);

    atomic_store_explicit(&r->awake, 1, memory_order_seq_cst);
    bool any = false;
    uint64_t deadline = yasl_time_ns() + PTY_DRAIN_BUDGET_NS;
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

    size_t head;
    size_t switch_tail =
        atomic_load_explicit(&r->switch_tail, memory_order_relaxed);
    for (;;) {
        head = atomic_load_explicit(&r->head, memory_order_acquire);
        size_t switch_head =
            atomic_load_explicit(&r->switch_head, memory_order_acquire);
        while (switch_tail != switch_head &&
               r->switch_at[switch_tail % PTY_RING_SWITCHES] == tail) {
            r->tail_src ^= 1;
            switch_tail++;
        }
        size_t avail = head - tail;
        if (avail == 0)
            break;

        size_t off = tail & PTY_RING_MASK;
        size_t contig = PTY_RING_SIZE - off;
        size_t chunk = avail < contig ? avail : contig;
        if (chunk > PTY_RING_MAX_CHUNK) chunk = PTY_RING_MAX_CHUNK;
        if (switch_tail != switch_head) {
            size_t until = r->switch_at[switch_tail % PTY_RING_SWITCHES] - tail;
            if (chunk > until)
                chunk = until;
        }

        r->cbs.data(r->ud, r->src_fd[r->tail_src], r->buf + off, chunk,
                    avail - chunk);

        tail += chunk;
        any = true;

        if (yasl_time_ns() >= deadline)
            break;
    }
    atomic_store_explicit(&r->switch_tail, switch_tail, memory_order_release);
    atomic_store_explicit(&r->tail, tail, memory_order_release);

    atomic_store_explicit(&r->awake, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head != tail)
        event_signal(r->event_wr);

    if (atomic_load_explicit(&r->need_space, memory_order_acquire) &&
        !ring_low(head, tail,
                  atomic_load_explicit(&r->switch_head, memory_order_acquire),
                  switch_tail))
        event_signal(r->space_wr);

    if (any && r->cbs.drained)
        r->cbs.drained(r->ud);

    size_t end = atomic_load_explicit(&r->end_at, memory_order_acquire);
    if (end && tail - (end - 1) <= PTY_RING_SIZE && !r->ended) {
        r->ended = true;
        if (r->cbs.ended)
            r->cbs.ended(r->ud);
    }
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
        munmap(r->buf, PTY_RING_SIZE);
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
    r->buf = mmap(NULL, PTY_RING_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!r->buf || r->buf == MAP_FAILED) {
        r->buf = NULL;
        return -1;
    }
    platform_madv_dontfork(r->buf, PTY_RING_SIZE);
    platform_madv_nohugepage(r->buf, PTY_RING_SIZE);
    r->event_fd = platform_eventfd(&r->event_wr);
    if (r->event_fd < 0)
        goto fail;
    r->space_fd = platform_eventfd(&r->space_wr);
    if (r->space_fd < 0)
        goto fail;
    r->event_watch = yasl_loop_add_fd(loop, r->event_fd, POLLIN, on_ring_event, r);
    if (r->event_watch <= 0 ||
        pthread_create(&r->thread, NULL, pty_ring_worker, r) != 0)
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
    event_signal(r->space_wr);
    if (atomic_load(&r->worker_done))
        publish_end(r, atomic_load_explicit(&r->head, memory_order_acquire));
}

bool pty_ring_owns_fd(const struct pty_ring *r, int fd)
{
    return r->active && fd >= 0 && (r->src_fd[0] == fd || r->src_fd[1] == fd);
}
