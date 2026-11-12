#include "../../src/platform/wayland/clipboard.c"
#include "common/loop.h"
#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>

struct received {
    int count;
    size_t len;
    uint64_t request_id;
    enum yasl_private_transfer_status status;
    struct clipboard *clipboard;
    bool forget_on_receive;
    char text[32];
};

static void receive(const char *text, size_t len, bool uri_list,
                    struct wl_surface *surface, uint64_t request_id,
                    enum yasl_private_transfer_status status, void *ud)
{
    (void)surface;
    struct received *r = ud;
    assert(!uri_list && len < sizeof(r->text));
    if (status == YASL_PRIVATE_TRANSFER_OK) {
        assert(text);
        memcpy(r->text, text, len);
        r->text[len] = '\0';
    } else {
        assert(!text && len == 0);
    }
    r->len = len;
    r->request_id = request_id;
    r->status = status;
    r->count++;
    if (r->forget_on_receive) {
        r->forget_on_receive = false;
        clipboard_forget_surface(r->clipboard, surface);
    }
}

struct closing_receiver {
    struct yasl_loop *loop;
    int fd, watch;
};

static void close_receiver(void *ud, int fd, short events)
{
    (void)fd;
    struct closing_receiver *receiver = ud;
    assert(events & POLLOUT);
    close(receiver->fd);
    assert(yasl_loop_del_fd(receiver->loop, receiver->watch) == 0);
}

static void test_closed_receiver(bool pending_signal)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGPIPE, &action, NULL) == 0);
        sigset_t pipe_signal, old_mask, pending;
        sigemptyset(&pipe_signal);
        sigaddset(&pipe_signal, SIGPIPE);
        assert(pthread_sigmask(pending_signal ? SIG_BLOCK : SIG_UNBLOCK,
                               &pipe_signal, &old_mask) == 0);
        if (pending_signal) assert(raise(SIGPIPE) == 0);

        int sockets[2], pipe_fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        struct wl_display *display = wl_display_connect_to_fd(sockets[0]);
        assert(display);
        struct wl_data_device_manager *manager = (struct wl_data_device_manager *)
            wl_proxy_create((struct wl_proxy *)display, &wl_data_device_manager_interface);
        struct wl_seat *seat = (struct wl_seat *)
            wl_proxy_create((struct wl_proxy *)display, &wl_seat_interface);
        struct yasl_loop *loop = yasl_loop_create();
        assert(manager && seat && loop && pipe(pipe_fds) == 0);
        struct clipboard *clipboard = clipboard_create(display, manager, seat, loop, NULL, NULL);
        assert(clipboard && clipboard_copy(clipboard, "copy", 1));
        struct closing_receiver receiver = {.loop = loop, .fd = pipe_fds[0]};
        receiver.watch = yasl_loop_add_fd(loop, pipe_fds[1], POLLOUT, close_receiver, &receiver);
        assert(receiver.watch > 0);
        source_listener.send(clipboard, NULL, "text/plain", pipe_fds[1]);
        /* Both watches see a writable pipe. The first closes its receiver. */
        assert(yasl_loop_step(loop, 0) >= 0);
        assert(sigpending(&pending) == 0);
        assert((sigismember(&pending, SIGPIPE) == 1) == pending_signal);
        if (pending_signal) {
            struct timespec timeout = {0};
            assert(sigtimedwait(&pipe_signal, NULL, &timeout) == SIGPIPE);
        }
        assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);
        clipboard_destroy(clipboard);
        wl_proxy_destroy((struct wl_proxy *)seat);
        wl_proxy_destroy((struct wl_proxy *)manager);
        yasl_loop_destroy(loop);
        wl_display_disconnect(display);
        close(sockets[1]);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    test_closed_receiver(false);
    test_closed_receiver(true);
    struct received received = {0};
    struct clipboard c = {.loop = yasl_loop_create(), .receive = receive, .ud = &received};
    received.clipboard = &c;
    assert(c.loop);
    const struct { const char *bytes; size_t len; bool valid; } cases[] = {
        {"é字😀", sizeof("é字😀") - 1, true},
        {"", 0, true},
        {"\xff", 1, false},
        {"\xf0\x9f\x98", 3, false}, /* Incomplete non-BMP character. */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        received = (struct received){0};
        received.clipboard = &c;
        int fds[2];
        assert(pipe(fds) == 0);
        struct transfer *transfer = start_transfer_text(&c, fds[0], false, false, c.text);
        assert(transfer);
        yasl_private_timer_id timeout_id = transfer->timeout;
        assert(timeout_id && yasl_private_loop_timer_armed(c.loop, timeout_id));
        transfer->paste = true;
        transfer->request_id = 42;
        assert(write(fds[1], cases[i].bytes, cases[i].len) == (ssize_t)cases[i].len);
        close(fds[1]);
        for (int steps = 0; c.transfers && steps < 10; steps++)
            assert(yasl_loop_step(c.loop, 0) >= 0);
        assert(!c.transfers);
        assert(!yasl_private_loop_timer_armed(c.loop, timeout_id));
        assert(received.count == 1);
        assert(received.request_id == 42);
        assert(received.status == (cases[i].valid ? YASL_PRIVATE_TRANSFER_OK : YASL_PRIVATE_TRANSFER_FAILED));
        if (cases[i].valid) {
            assert(received.len == cases[i].len);
            assert(memcmp(received.text, cases[i].bytes, received.len) == 0);
        }
    }
    received = (struct received){0};
    received.clipboard = &c;
    int cancel_fds[2];
    assert(pipe(cancel_fds) == 0);
    struct transfer *pending = start_transfer_text(&c, cancel_fds[0], false, false, NULL);
    assert(pending);
    yasl_private_timer_id cancel_timeout_id = pending->timeout;
    assert(cancel_timeout_id && yasl_private_loop_timer_armed(c.loop, cancel_timeout_id));
    pending->paste = true;
    pending->request_id = 99;
    struct wl_surface *target = (struct wl_surface *)(uintptr_t)1;
    pending->drop_surface = target;
    clipboard_forget_surface(&c, target);
    close(cancel_fds[1]);
    assert(!c.transfers && received.count == 0);
    assert(!yasl_private_loop_timer_armed(c.loop, cancel_timeout_id));

    received.forget_on_receive = true;
    int timeout_fds[2];
    assert(pipe(timeout_fds) == 0);
    struct transfer *timed = start_transfer_text(&c, timeout_fds[0], false, false, NULL);
    assert(timed && timed->timeout);
    timed->paste = true;
    timed->request_id = 100;
    struct wl_surface *timeout_target = (struct wl_surface *)(uintptr_t)2;
    timed->drop_surface = timeout_target;
    yasl_private_timer_id timeout_id = timed->timeout;
    assert(yasl_loop_del_timer(c.loop, timeout_id));
    (void)transfer_timeout(timed, timeout_id, CLIPBOARD_TRANSFER_TIMEOUT_NS);
    close(timeout_fds[1]);
    assert(!c.transfers && received.count == 1);
    assert(received.request_id == 100 && received.status == YASL_PRIVATE_TRANSFER_FAILED);
    yasl_loop_destroy(c.loop);
    return 0;
}
