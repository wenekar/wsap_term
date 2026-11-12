#ifndef SESSION_WRITE_QUEUE_H
#define SESSION_WRITE_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "util/text_limit.h"

struct yasl_loop;

struct session_write_queue {
    pthread_mutex_t lock;
    struct yasl_loop *loop;
    int fd, watch;
    char *data;
    size_t capacity, head, length;
    void (*arm_pollout)(void *ud);
    void *arm_ud;
};

bool session_write_queue_bind(struct session_write_queue *queue, struct yasl_loop *loop, int fd);
void session_write_queue_destroy(struct session_write_queue *queue);
void session_write_queue_set_arm_cb(struct session_write_queue *queue, void (*cb)(void *ud), void *ud);
void session_write_queue_arm_watch(struct session_write_queue *queue);

bool session_write_queue_stage(struct session_write_queue *queue, const char *data, size_t len);
bool session_write_queue_flush(struct session_write_queue *queue);

bool session_write_queue_push_bytes(struct session_write_queue *queue, const char *data, size_t len);
bool session_write_queue_push_paste(struct session_write_queue *queue, const char *text, size_t len,
                        bool bracketed);

#endif
