#define _GNU_SOURCE
#include <poll.h>
#include "util/text_limit.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yasl/yasl.h"
#include "util/time.h"
#include "session/write_queue.h"
#include "input/input.h"

static int pipe_nonblocking_cloexec(int fds[2])
{
    if (pipe(fds) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(fds[i], F_SETFL,
                  fcntl(fds[i], F_GETFL) | O_NONBLOCK) < 0) {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
    }
    return 0;
}

struct receiver {
    struct yasl_loop *loop;
    const char *expected;
    size_t length, received;
    struct session_write_queue *queue;
    size_t append_after, suffix_len;
    bool appended;
};

static void receive_bytes(void *data, int fd, short events)
{
    (void)events;
    struct receiver *r = data;
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        assert(r->received + (size_t)n <= r->length);
        assert(memcmp(buf, r->expected + r->received, (size_t)n) == 0);
        r->received += (size_t)n;
    }
    assert(n < 0 && errno == EAGAIN);
    if (r->queue && !r->appended && r->received >= r->append_after) {
        assert(session_write_queue_push_bytes(r->queue,
                    r->expected + r->length - r->suffix_len, r->suffix_len));
        r->appended = true;
    }
    if (r->received == r->length)
        yasl_loop_quit(r->loop);
}

static uint64_t timeout(void *data, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    (void)data;
    assert(!"paste did not complete before deadline");
    return 0;
}

static void check_paste(const char *text, size_t len, bool bracketed,
                        const char *suffix, const char *expected, size_t expected_len, bool raw)
{
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++) {
        assert(fcntl(fds[i], F_SETFD, FD_CLOEXEC) == 0);
        assert(fcntl(fds[i], F_SETFL,
                     fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    }
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    struct session_write_queue storage = {0};
    struct session_write_queue *queue = &storage;
    assert(session_write_queue_bind(queue, loop, fds[1]));
    assert(raw ? session_write_queue_push_bytes(queue, text, len)
               : session_write_queue_push_paste(queue, text, len, bracketed));
    if (suffix)
        assert(session_write_queue_push_bytes(queue, suffix, strlen(suffix)));
    struct receiver receiver = { .loop = loop, .expected = expected, .length = expected_len };
    assert(yasl_loop_add_fd(loop, fds[0], POLLIN, receive_bytes, &receiver) > 0);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(2000), timeout, NULL) != 0);
    assert(yasl_loop_run(loop) == 0);
    assert(receiver.received == expected_len);
    session_write_queue_destroy(queue);
    close(fds[0]);
    close(fds[1]);
    yasl_loop_destroy(loop);
}

static void check_queue_cap(void)
{
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++) {
        assert(fcntl(fds[i], F_SETFL,
                     fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    }
    char fill[4096];
    memset(fill, 'f', sizeof(fill));
    while (write(fds[1], fill, sizeof(fill)) > 0)
        ;
    struct yasl_loop *loop = yasl_loop_create();
    assert(loop);
    struct session_write_queue storage = {0};
    struct session_write_queue *queue = &storage;
    assert(session_write_queue_bind(queue, loop, fds[1]));

    const size_t chunk = 1024 * 1024;
    char *buf = malloc(chunk);
    assert(buf);
    memset(buf, 'q', chunk);

    size_t queued = 0;
    bool failed = false;
    for (int i = 0; i < 20; i++) {
        if (!session_write_queue_push_bytes(queue, buf, chunk)) {
            failed = true;
            break;
        }
        queued += chunk;
    }
    assert(failed);
    assert(queued <= 16 * 1024 * 1024);
    assert(!session_write_queue_push_bytes(queue, buf, chunk));
    assert(!session_write_queue_push_paste(queue, buf, chunk, true));
    session_write_queue_destroy(queue);
    close(fds[0]);
    close(fds[1]);
    yasl_loop_destroy(loop);
    free(buf);
}

static void check_reuse_and_atomic_rejection(void)
{
    int fds[2];
    assert(pipe_nonblocking_cloexec(fds) == 0);
    struct yasl_loop *loop = yasl_loop_create();
    struct session_write_queue queue = {0};
    assert(session_write_queue_bind(&queue, loop, fds[1]));
    const size_t suffix = 1024 * 1024;
    size_t len = INPUT_TEXT_MAX + suffix;
    char *text = malloc(len);
    assert(text);
    for (size_t i = 0; i < len; i++) text[i] = (char)(i % 251);

    assert(!session_write_queue_push_bytes(&queue, text, len));
    char byte;
    assert(read(fds[0], &byte, 1) < 0 && errno == EAGAIN);
    assert(session_write_queue_push_bytes(&queue, text, INPUT_TEXT_MAX));
    assert(!session_write_queue_push_bytes(&queue, text, suffix));
    assert(!session_write_queue_bind(&queue, loop, -1));
    struct receiver receiver = {
        .loop = loop, .expected = text, .length = len, .queue = &queue,
        .append_after = INPUT_TEXT_MAX / 2, .suffix_len = suffix,
    };
    assert(yasl_loop_add_fd(loop, fds[0], POLLIN, receive_bytes, &receiver) > 0);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(5000), timeout, NULL) != 0);
    assert(yasl_loop_run(loop) == 0);
    assert(receiver.appended && receiver.received == len);
    session_write_queue_destroy(&queue);
    yasl_loop_destroy(loop);
    close(fds[0]); close(fds[1]); free(text);
}

static void check_failure_recovery(void)
{
    struct session_write_queue queue = {0};
    session_write_queue_destroy(&queue);
    assert(!session_write_queue_push_bytes(&queue, "x", 1));
    int fds[2];
    assert(pipe_nonblocking_cloexec(fds) == 0);
    struct yasl_loop *loop = yasl_loop_create();
    assert(session_write_queue_bind(&queue, loop, fds[1]));
    char buf[32];
    assert(session_write_queue_push_bytes(&queue, "ok", 2));
    assert(read(fds[0], buf, sizeof(buf)) == 2 && !memcmp(buf, "ok", 2));

    int saved = dup(fds[1]);
    assert(saved >= 0);
    close(fds[1]);
    assert(!session_write_queue_push_bytes(&queue, "lost", 4));
    assert(errno == EBADF);
    assert(dup2(saved, fds[1]) == fds[1]);
    close(saved);
    assert(session_write_queue_push_bytes(&queue, "new", 3));
    assert(read(fds[0], buf, sizeof(buf)) == 3 && !memcmp(buf, "new", 3));
    session_write_queue_destroy(&queue);
    session_write_queue_destroy(&queue);
    assert(session_write_queue_bind(&queue, loop, fds[1]));
    assert(session_write_queue_push_bytes(&queue, "again", 5));
    assert(read(fds[0], buf, sizeof(buf)) == 5 && !memcmp(buf, "again", 5));
    session_write_queue_destroy(&queue);
    yasl_loop_destroy(loop);
    close(fds[0]); close(fds[1]);
}

static void check_maximum_paste(void)
{
    char *text = malloc(INPUT_TEXT_MAX);
    char *expected = malloc(INPUT_TEXT_MAX + 12);
    assert(text && expected);
    memset(text, 'x', INPUT_TEXT_MAX);
    memcpy(expected, "\033[200~", 6);
    memcpy(expected + 6, text, INPUT_TEXT_MAX);
    memcpy(expected + 6 + INPUT_TEXT_MAX, "\033[201~", 6);
    check_paste(text, INPUT_TEXT_MAX, true, NULL,
                expected, INPUT_TEXT_MAX + 12, false);
    free(expected);
    free(text);
}

static void check_whole_event(void)
{
    char text[9000];
    memset(text, 'x', sizeof(text));
    memcpy(text + 4095, "\xe2\x82\xac", 3);
    memcpy(text + 8191, "\r\n", 2);
    char expected[sizeof(text) + 12];
    memcpy(expected, "\033[200~", 6);
    memcpy(expected + 6, text, 8192);
    memcpy(expected + 6 + 8192, text + 8193, sizeof(text) - 8193);
    memcpy(expected + 6 + sizeof(text) - 1, "\033[201~", 6);
    struct input_queue input = {0};
    assert(input_queue_push_paste(&input, text, sizeof(text)) == sizeof(text));
    struct input_event ev;
    assert(input_queue_take(&input, &ev));
    check_paste(ev.paste.data, ev.paste.len, true, NULL,
                expected, sizeof(text) + 11, false);
    input_event_release(&ev);
    assert(!input_queue_take(&input, &ev));
}

int main(void)
{
    const char input[] = "hi 世界\r\nnext\n\033\003";
    const char expected[] = "\033[200~hi 世界\rnext\r\033[201~";
    check_paste(input, sizeof(input) - 1, true, NULL, expected, sizeof(expected) - 1, false);

    size_t len = 256 * 1024;
    char *text = malloc(len + 4);
    assert(text);
    memset(text, 'z', len);
    memcpy(text + len, "tail", 4);
    check_paste(text, len, false, "tail", text, len + 4, false);
    memcpy(text + len, "\033\003\n\177", 4);
    check_paste(text, len, false, "\033\003\n\177", text, len + 4, false);
    text[0] = '\0';
    text[1] = '\033';
    text[2] = '\n';
    check_paste(text, len, false, "\033\003\n\177", text, len + 4, true);
    free(text);

    check_paste("\033\003\177", 3, false, "ok", "ok", 2, false);
    check_paste("", 0, true, "ok", "ok", 2, false);

    const char invalid[] = "a\xff\x80\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80"
                           "\xc2\x9b" "b\xe2\x82";
    check_paste(invalid, sizeof(invalid) - 1, true, NULL,
                "\033[200~ab\033[201~", 14, false);
    check_paste("\r\n\r\n\n\t", 6, false, NULL, "\r\r\r\t", 4, false);
    check_paste("\033\003\177", 3, true, "ok", "ok", 2, false);
    check_maximum_paste();
    check_whole_event();
    check_reuse_and_atomic_rejection();
    check_failure_recovery();
    check_queue_cap();
    puts("PASS: paste framing, filtering, ordering, partial-write, and queue-cap tests");
    return 0;
}
