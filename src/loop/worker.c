#define _GNU_SOURCE

#include "loop/worker.h"

#include "platform/platform.h"
#include "yasl/yasl.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct work_item {
    struct loop_work work;
    void *result;
    struct work_item *next;
};

struct loop_worker {
    yasl_loop *loop;
    int wake_fd;
    int wake_wr;
    int watch;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    struct work_item *queue_head;
    struct work_item *queue_tail;
    struct work_item *done_head;
    struct work_item *done_tail;
    bool stopping;
};

static void push(struct work_item **head, struct work_item **tail, struct work_item *item)
{
    item->next = NULL;
    if (*tail)
        (*tail)->next = item;
    else
        *head = item;
    *tail = item;
}

static struct work_item *pop(struct work_item **head, struct work_item **tail)
{
    struct work_item *item = *head;
    if (item) {
        *head = item->next;
        if (!*head)
            *tail = NULL;
        item->next = NULL;
    }
    return item;
}

static void wake(struct loop_worker *worker)
{
    uint64_t one = 1;
    ssize_t written;
    do {
        written = write(worker->wake_wr, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);
}

static void *worker_main(void *userdata)
{
    struct loop_worker *worker = userdata;
    pthread_mutex_lock(&worker->mutex);
    for (;;) {
        while (!worker->queue_head && !worker->stopping)
            pthread_cond_wait(&worker->ready, &worker->mutex);
        if (worker->stopping)
            break;
        struct work_item *item = pop(&worker->queue_head, &worker->queue_tail);
        pthread_mutex_unlock(&worker->mutex);
        item->result = item->work.run(item->work.arg);
        pthread_mutex_lock(&worker->mutex);
        push(&worker->done_head, &worker->done_tail, item);
        pthread_mutex_unlock(&worker->mutex);
        wake(worker);
        pthread_mutex_lock(&worker->mutex);
    }
    pthread_mutex_unlock(&worker->mutex);
    return NULL;
}

/* Delivers the results of the worker. The done callbacks run here, on the loop
 * thread, in the order in which the worker finished the items. */
static void on_wake(void *userdata, int fd, short revents)
{
    struct loop_worker *worker = userdata;
    (void)revents;
    uint64_t count;
    while (read(fd, &count, sizeof(count)) == (ssize_t)sizeof(count)) {}

    pthread_mutex_lock(&worker->mutex);
    struct work_item *item = worker->done_head;
    worker->done_head = NULL;
    worker->done_tail = NULL;
    pthread_mutex_unlock(&worker->mutex);

    while (item) {
        struct work_item *next = item->next;
        item->work.done(item->work.arg, item->result);
        free(item);
        item = next;
    }
}

struct loop_worker *loop_worker_create(struct yasl_loop *loop)
{
    if (!loop) {
        errno = EINVAL;
        return NULL;
    }
    struct loop_worker *worker = calloc(1, sizeof(*worker));
    if (!worker)
        return NULL;
    worker->loop = loop;
    worker->wake_wr = -1;
    if (pthread_mutex_init(&worker->mutex, NULL) != 0)
        goto fail_mutex;
    if (pthread_cond_init(&worker->ready, NULL) != 0)
        goto fail_cond;
    worker->wake_fd = platform_eventfd(&worker->wake_wr);
    if (worker->wake_fd < 0)
        goto fail_wake;
    worker->watch = yasl_loop_add_fd(loop, worker->wake_fd, POLLIN, on_wake, worker);
    if (worker->watch <= 0)
        goto fail_watch;
    if (pthread_create(&worker->thread, NULL, worker_main, worker) != 0)
        goto fail_thread;
    return worker;

fail_thread:
    yasl_loop_del_fd(loop, worker->watch);
fail_watch:
    close(worker->wake_fd);
    if (worker->wake_wr >= 0)
        close(worker->wake_wr);
fail_wake:
    pthread_cond_destroy(&worker->ready);
fail_cond:
    pthread_mutex_destroy(&worker->mutex);
fail_mutex:
    free(worker);
    return NULL;
}

static void cancel_all(struct work_item *item)
{
    while (item) {
        struct work_item *next = item->next;
        item->work.cancel(item->work.arg, item->result);
        free(item);
        item = next;
    }
}

void loop_worker_destroy(struct loop_worker *worker)
{
    if (!worker)
        return;
    yasl_loop_del_fd(worker->loop, worker->watch);
    pthread_mutex_lock(&worker->mutex);
    worker->stopping = true;
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);

    pthread_mutex_lock(&worker->mutex);
    struct work_item *queued = worker->queue_head;
    struct work_item *done = worker->done_head;
    worker->queue_head = worker->queue_tail = NULL;
    worker->done_head = worker->done_tail = NULL;
    pthread_mutex_unlock(&worker->mutex);

    cancel_all(queued);
    cancel_all(done);
    close(worker->wake_fd);
    if (worker->wake_wr >= 0)
        close(worker->wake_wr);
    pthread_cond_destroy(&worker->ready);
    pthread_mutex_destroy(&worker->mutex);
    free(worker);
}

int loop_worker_submit(struct loop_worker *worker, const struct loop_work *work)
{
    if (!worker || !work || !work->run || !work->done || !work->cancel) {
        errno = EINVAL;
        return -1;
    }
    struct work_item *item = calloc(1, sizeof(*item));
    if (!item)
        return -1;
    item->work = *work;
    pthread_mutex_lock(&worker->mutex);
    if (worker->stopping) {
        pthread_mutex_unlock(&worker->mutex);
        free(item);
        errno = ESHUTDOWN;
        return -1;
    }
    push(&worker->queue_head, &worker->queue_tail, item);
    pthread_cond_signal(&worker->ready);
    pthread_mutex_unlock(&worker->mutex);
    return 0;
}
