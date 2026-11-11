#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/platform.h"
#include "pty/ring.h"
#include "pty/sync.h"
#include "util/proc.h"
#include "yasl/yasl.h"

#if defined(__APPLE__)
#include <sys/ucred.h>
#endif

/* The bytes of one read from the pty. The worker parses each read into the
 * terminal at once, so this is also the size of its buffer. */
#define PTY_READ_SIZE PTY_CHUNK_MAX
#define PTY_SPIN_ITERS 128
#define PTY_SYNC_REQUEST_MS 100
/* The bytes that the worker parses before it wakes the loop thread. The kernel
 * pty returns a short read, so a signal for each read wakes the loop thread
 * hundreds of times a second in a flood. A burst that stops sooner wakes the
 * loop thread at once. */
#define PTY_WAKE_BYTES (32u * 1024u)

enum { PFD_SRC0, PFD_SRC1, PFD_REQUEST, PFD_SYNC, PFD_COUNT };

enum pump_result { PUMP_EMPTY, PUMP_EOF };

struct worker {
    struct pty_ring *r;
    struct pollfd pfd[PFD_COUNT];
    bool end_done;
};

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

/* Wake the loop thread once for the content that waits. A signal that is
 * already pending covers this content too: the loop thread drains all the
 * content before it reads wake_pending again. */
static void wake_consumer(struct pty_ring *r)
{
    if (atomic_exchange_explicit(&r->wake_pending, 1, memory_order_seq_cst) == 0)
        event_signal(r->event_wr);
}

void pty_ring_wake(struct pty_ring *r)
{
    if (r && r->event_wr >= 0)
        wake_consumer(r);
}

static void publish_end(struct pty_ring *r)
{
    atomic_store_explicit(&r->end_published, 1, memory_order_release);
    if (r->event_wr >= 0)
        event_signal(r->event_wr);
}

static void close_source(struct worker *w, unsigned i)
{
    w->pfd[i].fd = -1;
}

/* Read the pty into the terminal. Each chunk goes to the data callback before
 * the next read, so the parse keeps the pty buffer empty. */
static enum pump_result pump(struct worker *w, unsigned i)
{
    struct pty_ring *r = w->r;
    int fd = r->src_fd[i];
    enum pump_result result = PUMP_EMPTY;
    bool read_any = false;
    size_t unsignaled = 0;
#if defined(__APPLE__)
    int spin = 0;
#endif
    for (;;) {
        ssize_t n = read(fd, r->buf, PTY_READ_SIZE);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
#if defined(__APPLE__)
                /* The pty gives short reads. A short spin keeps a flood off
                 * the poll syscall. */
                if (read_any && ++spin <= PTY_SPIN_ITERS) {
                    cpu_relax();
                    continue;
                }
#endif
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
        read_any = true;
        r->cbs.data(r->ud, fd, r->buf, (size_t)n);
        unsignaled += (size_t)n;
        if (unsignaled >= PTY_WAKE_BYTES) {
            wake_consumer(r);
            unsignaled = 0;
        }
    }
    /* The source runs dry. The loop thread draws the content that waits, so it
     * gets the signal that the batch holds back. */
    if (unsignaled)
        wake_consumer(r);
    return result;
}

static void drain_source(struct worker *w, unsigned i)
{
    if (w->pfd[i].fd < 0)
        return;
    if (pump(w, i) == PUMP_EOF)
        close_source(w, i);
}

/* The child is gone. Drain what it wrote and then publish the end of the
 * output. Every byte reaches the terminal before the end callback. */
static void serve_end(struct worker *w)
{
    if (w->end_done ||
        !atomic_load_explicit(&w->r->end_requested, memory_order_acquire))
        return;
    drain_source(w, PFD_SRC0);
    drain_source(w, PFD_SRC1);
    w->end_done = true;
    publish_end(w->r);
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

/* The child changes the stream that it writes to. Drain the stream that it
 * leaves before it sends the answer, so the terminal sees the bytes in program
 * order. */
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
            drain_source(w, req == PTY_SYNC_LEAVE_PIPE ? PFD_SRC0 : PFD_SRC1);
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
            [PFD_REQUEST] = { .fd = r->request_fd, .events = POLLIN },
            [PFD_SYNC] = { .fd = r->sync_fd, .events = POLLIN },
        },
    };

    while (!atomic_load_explicit(&r->stop, memory_order_relaxed) &&
           (w.pfd[PFD_SRC0].fd >= 0 || w.pfd[PFD_SRC1].fd >= 0)) {
        if (atomic_load_explicit(&r->end_requested, memory_order_acquire)) {
            serve_end(&w);
            break;
        }
        int pr = poll(w.pfd, PFD_COUNT, -1);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        if (w.pfd[PFD_REQUEST].revents & POLLIN)
            drain_fd(r->request_fd);
        if (atomic_load_explicit(&r->stop, memory_order_relaxed))
            break;
        serve_end(&w);
        if (w.end_done)
            break;
        for (unsigned i = PFD_SRC0; i <= PFD_SRC1; i++) {
            if (w.pfd[i].fd >= 0 &&
                (w.pfd[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) &&
                pump(&w, i) == PUMP_EOF)
                close_source(&w, i);
        }
        if (w.pfd[PFD_SYNC].revents & POLLIN)
            serve_sync(&w);
    }
    atomic_store(&r->worker_done, 1);
    if (!atomic_load_explicit(&r->stop, memory_order_relaxed) &&
        atomic_load_explicit(&r->end_requested, memory_order_acquire) &&
        !w.end_done)
        publish_end(r);
    return NULL;
}

/* Only drains the wake signal and tells the loop thread. It parses nothing and
 * it holds no budget. */
static void on_ring_event(void *ud, int fd, short revents)
{
    (void)revents;
    struct pty_ring *r = ud;
    drain_fd(fd);
    atomic_store_explicit(&r->wake_pending, 0, memory_order_seq_cst);
    if (r->cbs.wake)
        r->cbs.wake(r->ud);
    if (!r->ended_delivered &&
        atomic_load_explicit(&r->end_published, memory_order_acquire)) {
        r->ended_delivered = true;
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
    proc_eventfd_close(r->request_fd, r->request_wr);
    r->request_fd = r->request_wr = -1;
    if (r->buf) {
        munmap(r->buf, PTY_READ_SIZE);
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
        .request_fd = -1,
        .request_wr = -1,
        .cbs = *cbs,
        .ud = ud,
    };
    if (r->src_fd[0] < 0)
        return -1;
    r->buf = mmap(NULL, PTY_READ_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!r->buf || r->buf == MAP_FAILED) {
        r->buf = NULL;
        return -1;
    }
    platform_madv_dontfork(r->buf, PTY_READ_SIZE);
    platform_madv_nohugepage(r->buf, PTY_READ_SIZE);
    r->event_fd = platform_eventfd(&r->event_wr);
    if (r->event_fd < 0)
        goto fail;
    r->request_fd = platform_eventfd(&r->request_wr);
    if (r->request_fd < 0)
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
    if (r->request_wr >= 0)
        event_signal(r->request_wr);
    pthread_join(r->thread, NULL);
    ring_release(r);
}

void pty_ring_end(struct pty_ring *r)
{
    if (!r->active)
        return;
    atomic_store(&r->end_requested, 1);
    event_signal(r->request_wr);
    if (atomic_load(&r->worker_done))
        publish_end(r);
}
