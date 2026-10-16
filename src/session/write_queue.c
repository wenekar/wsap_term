#include <poll.h>
#include "util/text_limit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yasl/yasl.h"
#include "session/write_queue.h"
#include "wsap_vtcore/wsap_vtcore.h"

#define QUEUE_LIMIT (INPUT_TEXT_MAX + 12u)
#define WRITE_BUDGET (64u * 1024)

static void unwatch(struct session_write_queue *q)
{
    if (q->watch > 0)
        yasl_loop_del_fd(q->loop, q->watch);
    q->watch = 0;
}

static void discard(struct session_write_queue *q)
{
    unwatch(q);
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
        unwatch(q);
        q->head = 0;
        if (q->capacity > 4096) {
            free(q->data);
            q->data = NULL;
            q->capacity = 0;
        }
    }
    return true;
}

static void drain_ready(void *data, int fd, short events)
{
    (void)fd;
    struct session_write_queue *q = data;
    if (events & (POLLERR | POLLHUP | POLLNVAL))
        discard(q);
    else
        drain(q);
}

static bool reserve_and_watch(struct session_write_queue *q, size_t extra)
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
    if (!q->watch) {
        int watch = yasl_loop_add_fd(q->loop, q->fd, POLLOUT, drain_ready, q);
        if (watch < 0) return false;
        q->watch = watch;
    }
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

static bool valid(struct session_write_queue *q, const char *data, size_t len)
{
    if (!q || !q->loop || (!data && len)) {
        errno = EINVAL;
        return false;
    }
    return true;
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
    discard(q);
    q->loop = loop;
    q->fd = fd;
    return true;
}

void session_write_queue_destroy(struct session_write_queue *q)
{
    if (!q) return;
    discard(q);
    *q = (struct session_write_queue){0};
}

bool session_write_queue_push_bytes(struct session_write_queue *q, const char *data, size_t len)
{
    if (!valid(q, data, len)) return false;
    if (!len) return true;
    if (!reserve_and_watch(q, len)) return false;
    append(q, data, len);
    return drain(q);
}

bool session_write_queue_push_paste(struct session_write_queue *q, const char *text, size_t len,
                        bool bracketed)
{
    if (!valid(q, text, len)) return false;
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
