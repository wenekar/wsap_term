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

bool session_write_queue_bind(struct session_write_queue *queue, struct yasl_loop *loop, int fd);
void session_write_queue_destroy(struct session_write_queue *queue);
bool session_write_queue_push_bytes(struct session_write_queue *queue, const char *data, size_t len);
bool session_write_queue_push_paste(struct session_write_queue *queue, const char *text, size_t len,
                        bool bracketed);

#endif
