#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yasl/yasl.h"
#include "session/write_queue.h"
#include "util/text_limit.h"
#include "wsap_vtcore/wsap_vtcore.h"

#define QUEUE_LIMIT (INPUT_TEXT_MAX + 12u)
#define WRITE_BUDGET (64u * 1024)

static void drain_ready(void *data, int fd, short events);

static void unwatch_locked(struct session_write_queue *q)
{
    if (q->watch > 0 && q->loop)
        yasl_loop_del_fd(q->loop, q->watch);
    q->watch = 0;
}

static void discard_locked(struct session_write_queue *q)
{
    unwatch_locked(q);
    free(q->data);
    q->data = NULL;
    q->capacity = q->head = q->length = 0;
}

static void maybe_arm_watch_locked(struct session_write_queue *q)
{
    if (q->length > 0 && !q->watch) {
        if (q->arm_pollout)
            q->arm_pollout(q->arm_ud);
        else if (q->loop && q->fd >= 0) {
            int watch = yasl_loop_add_fd(q->loop, q->fd, POLLOUT, drain_ready, q);
            if (watch > 0)
                q->watch = watch;
        }
    }
}

static bool drain_locked(struct session_write_queue *q)
{
    size_t budget = WRITE_BUDGET;
    while (q->length && budget) {
        size_t n = q->capacity - q->head;
        if (n > q->length)
            n = q->length;
        if (n > budget)
            n = budget;
        ssize_t written = write(q->fd, q->data + q->head, n);
        if (written < 0) {
            int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
                maybe_arm_watch_locked(q);
                return true;
            }
            discard_locked(q);
            errno = error;
            return false;
        }
        if (!written) {
            discard_locked(q);
            errno = EIO;
            return false;
        }
        q->head = (q->head + (size_t)written) % q->capacity;
        q->length -= (size_t)written;
        budget -= (size_t)written;
    }
    if (!q->length) {
        if (!q->arm_pollout)
            unwatch_locked(q);
        q->head = 0;
        if (q->capacity > 4096) {
            free(q->data);
            q->data = NULL;
            q->capacity = 0;
        }
    } else {
        maybe_arm_watch_locked(q);
    }
    return true;
}

static void drain_ready(void *data, int fd, short events)
{
    (void)fd;
    struct session_write_queue *q = data;
    pthread_mutex_lock(&q->lock);
    if (events & (POLLERR | POLLHUP | POLLNVAL)) {
        discard_locked(q);
    } else {
        drain_locked(q);
        if (q->length == 0)
            unwatch_locked(q);
    }
    pthread_mutex_unlock(&q->lock);
}

static void append_locked(struct session_write_queue *q, const char *data, size_t len)
{
    if (!len)
        return;
    size_t tail = (q->head + q->length) % q->capacity;
    size_t first = q->capacity - tail;
    if (first > len)
        first = len;
    memcpy(q->data + tail, data, first);
    memcpy(q->data, data + first, len - first);
    q->length += len;
}

static bool valid(struct session_write_queue *q, const char *data, size_t len)
{
    if (!q || !q->loop || (!data && len)) {
        errno = EINVAL;
        return false;
    }
    return true;
}

void session_write_queue_set_arm_cb(struct session_write_queue *q, void (*cb)(void *ud), void *ud)
{
    if (!q)
        return;
    pthread_mutex_lock(&q->lock);
    q->arm_pollout = cb;
    q->arm_ud = ud;
    pthread_mutex_unlock(&q->lock);
}

void session_write_queue_arm_watch(struct session_write_queue *q)
{
    if (!q)
        return;
    pthread_mutex_lock(&q->lock);
    if (!q->watch && q->length > 0 && q->loop && q->fd >= 0) {
        int watch = yasl_loop_add_fd(q->loop, q->fd, POLLOUT, drain_ready, q);
        if (watch > 0)
            q->watch = watch;
    }
    pthread_mutex_unlock(&q->lock);
}

bool session_write_queue_bind(struct session_write_queue *q, struct yasl_loop *loop, int fd)
{
    if (!q || !loop) {
        errno = EINVAL;
        return false;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return false;
    if (!(flags & O_NONBLOCK)) {
        errno = EINVAL;
        return false;
    }
    if (!q->loop)
        pthread_mutex_init(&q->lock, NULL);
    pthread_mutex_lock(&q->lock);
    discard_locked(q);
    q->loop = loop;
    q->fd = fd;
    q->arm_pollout = NULL;
    q->arm_ud = NULL;
    pthread_mutex_unlock(&q->lock);
    return true;
}

void session_write_queue_destroy(struct session_write_queue *q)
{
    if (!q || !q->loop)
        return;
    pthread_mutex_lock(&q->lock);
    discard_locked(q);
    q->loop = NULL;
    q->fd = -1;
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    *q = (struct session_write_queue){0};
}

bool session_write_queue_stage(struct session_write_queue *q, const char *data, size_t len)
{
    if (!valid(q, data, len))
        return false;
    if (!len)
        return true;

    pthread_mutex_lock(&q->lock);
    if (len > QUEUE_LIMIT - q->length) {
        static _Atomic uint64_t last_warn_ns;
        uint64_t now = yasl_time_ns();
        uint64_t prev = atomic_load_explicit(&last_warn_ns, memory_order_relaxed);
        if (now >= prev + 1000000000ull &&
            atomic_compare_exchange_strong_explicit(&last_warn_ns, &prev, now,
                                                    memory_order_relaxed,
                                                    memory_order_relaxed)) {
            fprintf(stderr, "[session] write queue overflow (limit %u bytes)\n", QUEUE_LIMIT);
        }
        pthread_mutex_unlock(&q->lock);
        errno = ENOBUFS;
        return false;
    }

    size_t need = q->length + len;
    if (need > q->capacity) {
        size_t target = need > 4096 ? need : 4096;
        size_t capacity = 1ULL << (64 - __builtin_clzll(target - 1));
        if (capacity > QUEUE_LIMIT)
            capacity = QUEUE_LIMIT;
        char *buf = malloc(capacity);
        if (!buf) {
            pthread_mutex_unlock(&q->lock);
            return false;
        }
        if (q->length) {
            size_t first = q->capacity - q->head;
            if (first > q->length)
                first = q->length;
            memcpy(buf, q->data + q->head, first);
            memcpy(buf + first, q->data, q->length - first);
        }
        free(q->data);
        q->data = buf;
        q->capacity = capacity;
        q->head = 0;
    }

    append_locked(q, data, len);
    pthread_mutex_unlock(&q->lock);
    return true;
}

bool session_write_queue_flush(struct session_write_queue *q)
{
    if (!q || !q->loop) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&q->lock);
    bool ok = drain_locked(q);
    pthread_mutex_unlock(&q->lock);
    return ok;
}

bool session_write_queue_push_bytes(struct session_write_queue *q, const char *data, size_t len)
{
    if (!session_write_queue_stage(q, data, len))
        return false;
    return session_write_queue_flush(q);
}

bool session_write_queue_push_paste(struct session_write_queue *q, const char *text, size_t len,
                        bool bracketed)
{
    if (!valid(q, text, len))
        return false;
    if (len > INPUT_TEXT_MAX) {
        errno = EMSGSIZE;
        return false;
    }
    char *buf = malloc(len + WSAP_VTCORE_PASTE_EXTRA);
    if (!buf)
        return false;
    struct wsap_vtcore_modes modes = { .bracketed_paste = bracketed };
    struct wsap_vtcore_input_result r =
        wsap_vtcore_input_encode_paste(&modes, text, len, buf, len + WSAP_VTCORE_PASTE_EXTRA);
    bool ok = r.status != WSAP_VTCORE_INPUT_BYTES || session_write_queue_push_bytes(q, buf, r.len);
    free(buf);
    return ok;
}
