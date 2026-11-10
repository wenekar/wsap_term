#define _GNU_SOURCE
#include <poll.h>
#include "util/text_limit.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform/platform.h"
#include "yasl/yasl.h"
#include "session/write_queue.h"
#include "util/proc.h"
#include "wsap_vtcore/wsap_vtcore.h"

#define QUEUE_LIMIT (INPUT_TEXT_MAX + 12u)
#define WRITE_BUDGET (64u * 1024)

static void drain_ready(void *data, int fd, short events);
static void flush_ready(void *data, int fd, short events);

/* The calls below run with the lock held. Only a call on the loop thread
 * changes the loop. */

static bool on_loop_thread(const struct session_write_queue *q)
{
    return pthread_equal(q->owner, pthread_self());
}

static void unwatch(struct session_write_queue *q)
{
    if (q->watch > 0 && q->loop)
        yasl_loop_del_fd(q->loop, q->watch);
    q->watch = 0;
}

static void discard(struct session_write_queue *q)
{
    free(q->data);
    q->data = NULL;
    q->capacity = q->head = q->length = 0;
}

static bool drain(struct session_write_queue *q)
{
    size_t budget = WRITE_BUDGET;
    while (q->length && budget) {
        size_t n = q->capacity - q->head;
        if (n > q->length) n = q->length;
        if (n > budget) n = budget;
        ssize_t written = write(q->fd, q->data + q->head, n);
        if (written < 0) {
            int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
                return true;
            discard(q);
            errno = error;
            return false;
        }
        if (!written) {
            discard(q);
            errno = EIO;
            return false;
        }
        q->head = (q->head + (size_t)written) % q->capacity;
        q->length -= (size_t)written;
        budget -= (size_t)written;
    }
    if (!q->length) {
        q->head = 0;
        if (q->capacity > 4096) {
            free(q->data);
            q->data = NULL;
            q->capacity = 0;
        }
    }
    return true;
}

static bool reserve(struct session_write_queue *q, size_t extra)
{
    if (extra > QUEUE_LIMIT - q->length) {
        errno = ENOBUFS;
        return false;
    }
    size_t need = q->length + extra;
    if (need > q->capacity) {
        size_t target = need > 4096 ? need : 4096;
        size_t capacity = 1ULL << (64 - __builtin_clzll(target - 1));
        if (capacity > QUEUE_LIMIT) capacity = QUEUE_LIMIT;
        char *data = malloc(capacity);
        if (!data) return false;
        if (q->length) {
            size_t first = q->capacity - q->head;
            if (first > q->length) first = q->length;
            memcpy(data, q->data + q->head, first);
            memcpy(data + first, q->data, q->length - first);
        }
        free(q->data);
        q->data = data;
        q->capacity = capacity;
        q->head = 0;
    }
    return true;
}

static bool watch(struct session_write_queue *q)
{
    if (q->watch)
        return true;
    int watch_id = yasl_loop_add_fd(q->loop, q->fd, POLLOUT, drain_ready, q);
    if (watch_id <= 0)
        return false;
    q->watch = watch_id;
    return true;
}

static void append(struct session_write_queue *q, const char *data, size_t len)
{
    if (!len) return;
    size_t tail = (q->head + q->length) % q->capacity;
    size_t first = q->capacity - tail;
    if (first > len) first = len;
    memcpy(q->data + tail, data, first);
    memcpy(q->data, data + first, len - first);
    q->length += len;
}

/* Wake the loop thread after a push from another thread left bytes behind.
 * Any thread. */
static void wake_loop(struct session_write_queue *q)
{
    uint64_t one = 1;
    ssize_t n = write(q->wake_wr, &one, sizeof(one));
    (void)n;
}

static void drain_wake(int fd)
{
    char drain[64];
    while (read(fd, drain, sizeof(drain)) > 0) {}
}

static void drain_ready(void *data, int fd, short events)
{
    (void)fd;
    struct session_write_queue *q = data;
    pthread_mutex_lock(&q->lock);
    if (events & (POLLERR | POLLHUP | POLLNVAL))
        discard(q);
    else
        (void)drain(q);
    if (!q->length)
        unwatch(q);
    pthread_mutex_unlock(&q->lock);
}

static void flush_ready(void *data, int fd, short events)
{
    (void)events;
    struct session_write_queue *q = data;
    drain_wake(fd);
    pthread_mutex_lock(&q->lock);
    if (q->length && !q->watch && q->loop)
        (void)watch(q);
    if (q->length)
        (void)drain(q);
    if (!q->length)
        unwatch(q);
    pthread_mutex_unlock(&q->lock);
}

bool session_write_queue_bind(struct session_write_queue *q, struct yasl_loop *loop, int fd)
{
    if (!q || !loop) {
        errno = EINVAL;
        return false;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    if (!(flags & O_NONBLOCK)) {
        errno = EINVAL;
        return false;
    }
    if (!q->lock_ready) {
        if (pthread_mutex_init(&q->lock, NULL) != 0) {
            errno = ENOMEM;
            return false;
        }
        q->lock_ready = true;
        q->wake_fd = q->wake_wr = -1;
    }
    pthread_mutex_lock(&q->lock);
    discard(q);
    unwatch(q);
    q->loop = loop;
    q->fd = fd;
    q->owner = pthread_self();
    bool ok = true;
    if (q->wake_fd < 0) {
        q->wake_fd = platform_eventfd(&q->wake_wr);
        ok = q->wake_fd >= 0;
    }
    if (ok && q->wake_watch <= 0) {
        q->wake_watch = yasl_loop_add_fd(loop, q->wake_fd, POLLIN, flush_ready, q);
        ok = q->wake_watch > 0;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

void session_write_queue_destroy(struct session_write_queue *q)
{
    if (!q) return;
    if (!q->lock_ready) {
        *q = (struct session_write_queue){0};
        return;
    }
    pthread_mutex_lock(&q->lock);
    discard(q);
    unwatch(q);
    if (q->wake_watch > 0 && q->loop)
        yasl_loop_del_fd(q->loop, q->wake_watch);
    q->wake_watch = 0;
    proc_eventfd_close(q->wake_fd, q->wake_wr);
    q->wake_fd = q->wake_wr = -1;
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    *q = (struct session_write_queue){0};
}

/* Any thread. The bytes that the pty does not accept stay in the queue, and
 * the loop thread writes them when the pty has room. */
bool session_write_queue_push_bytes(struct session_write_queue *q, const char *data, size_t len)
{
    if (!q || !q->lock_ready || (!data && len)) {
        errno = EINVAL;
        return false;
    }
    if (!len)
        return true;
    bool wake = false;
    pthread_mutex_lock(&q->lock);
    bool ok = false;
    if (!q->loop) {
        errno = EINVAL;
    } else if (!reserve(q, len)) {
        /* The queue is full or out of memory. */
    } else {
        /* Only the loop thread may add the watch. Another thread asks the
         * loop thread to write the rest. */
        bool owner = on_loop_thread(q);
        if (!owner || watch(q)) {
            append(q, data, len);
            ok = drain(q);
            if (ok && !q->length) {
                if (owner)
                    unwatch(q);
            } else if (ok && !owner) {
                wake = true;
            }
        }
    }
    pthread_mutex_unlock(&q->lock);
    if (wake)
        wake_loop(q);
    return ok;
}

bool session_write_queue_push_paste(struct session_write_queue *q, const char *text, size_t len,
                        bool bracketed)
{
    if (!q || !q->lock_ready || (!text && len)) {
        errno = EINVAL;
        return false;
    }
    if (len > INPUT_TEXT_MAX) {
        errno = EMSGSIZE;
        return false;
    }
    char *buf = malloc(len + WSAP_VTCORE_PASTE_EXTRA);
    if (!buf) return false;
    struct wsap_vtcore_modes modes = { .bracketed_paste = bracketed };
    struct wsap_vtcore_input_result r =
        wsap_vtcore_input_encode_paste(&modes, text, len, buf, len + WSAP_VTCORE_PASTE_EXTRA);
    bool ok = r.status != WSAP_VTCORE_INPUT_BYTES || session_write_queue_push_bytes(q, buf, r.len);
    free(buf);
    return ok;
}
