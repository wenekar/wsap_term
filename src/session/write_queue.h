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
 * Binds write queue `queue` to event loop `loop` and destination descriptor `fd`.
 * Returns true on success, or false on allocation failure.
 */
bool session_write_queue_bind(struct session_write_queue *queue, struct yasl_loop *loop, int fd);

/* Releases buffer memory and unregisters event loop watches for `queue`. */
void session_write_queue_destroy(struct session_write_queue *queue);

/*
 * Enqueues raw bytes `data` of length `len` into write queue `queue`.
 * Returns true on success, or false on allocation failure.
 */
bool session_write_queue_push_bytes(struct session_write_queue *queue, const char *data, size_t len);

/*
 * Enqueues clipboard `text` into `queue`, optionally wrapping with bracketed paste escapes.
 * Returns true on success, or false on allocation failure.
 */
bool session_write_queue_push_paste(struct session_write_queue *queue, const char *text, size_t len,
                        bool bracketed);

#endif
