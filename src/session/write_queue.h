#ifndef SESSION_WRITE_QUEUE_H
#define SESSION_WRITE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "util/text_limit.h"

struct yasl_loop;

struct session_write_queue {
    struct yasl_loop *loop;
    int fd, watch;
    char *data;
    size_t capacity, head, length;
};

/* Bind a nonblocking FD. Success discards earlier queued bytes.
 * The caller owns the FD and loop. */
bool session_write_queue_bind(struct session_write_queue *queue, struct yasl_loop *loop, int fd);
void session_write_queue_destroy(struct session_write_queue *queue);
/* Copy bytes and try to write them. Pending bytes keep their order.
 * True means accepted. A later FD error can discard pending bytes. */
bool session_write_queue_push_bytes(struct session_write_queue *queue, const char *data, size_t len);
/* Filter terminal paste text and add optional bracket markers. */
bool session_write_queue_push_paste(struct session_write_queue *queue, const char *text, size_t len,
                        bool bracketed);

#endif
