#ifndef SESSION_WRITE_QUEUE_H
#define SESSION_WRITE_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "util/text_limit.h"

struct yasl_loop;

/*
 * The bytes that wait for the pty to accept them.
 *
 * Any thread may push bytes. The push writes what the pty accepts at once and
 * keeps the rest. Only the loop thread changes the loop, so a push from another
 * thread wakes the loop through an eventfd, and the loop thread then watches
 * the pty for room and writes the rest.
 */
struct session_write_queue {
    pthread_mutex_t lock;
    struct yasl_loop *loop;
    pthread_t owner;
    char *data;
    size_t capacity, head, length;
    int fd, watch;
    int wake_fd, wake_wr, wake_watch;
    bool lock_ready;
};

/*
 * Binds write queue `queue` to event loop `loop` and destination `fd`, which
 * must be non-blocking. Call it on the loop thread. A bind discards pending
 * bytes and the watch of an earlier binding.
 * Returns true on success, or false with errno set when `fd` is not
 * non-blocking, or when a descriptor or watch cannot be created.
 */
bool session_write_queue_bind(struct session_write_queue *queue, struct yasl_loop *loop, int fd);

/*
 * Releases the bytes, the watches, and the eventfd of `queue`, destroys its
 * lock, and zeroes the struct for a later binding. Call it on the loop thread.
 * It also accepts a queue that was never bound.
 */
void session_write_queue_destroy(struct session_write_queue *queue);

/*
 * Enqueues raw bytes `data` of length `len`. Any thread can push, and the queue
 * copies the bytes. A push writes what the pty accepts now and keeps the rest
 * for the loop thread.
 * Returns true when the queue takes every byte. A failure returns false with
 * errno set to ENOBUFS (queue full), EINVAL (no destination), or ENOMEM.
 */
bool session_write_queue_push_bytes(struct session_write_queue *queue, const char *data, size_t len);

/*
 * Enqueues clipboard `text` of length `len` for the pty, with bracketed paste
 * escapes when `bracketed` is set. The terminal core filters the text.
 * Returns true when the queue takes every byte. It returns false when `len`
 * exceeds INPUT_TEXT_MAX (EMSGSIZE), when `text` is NULL with a nonzero `len`
 * (EINVAL), when the queue is unbound (EINVAL), or when the push fails.
 */
bool session_write_queue_push_paste(struct session_write_queue *queue, const char *text, size_t len,
                        bool bracketed);

#endif
