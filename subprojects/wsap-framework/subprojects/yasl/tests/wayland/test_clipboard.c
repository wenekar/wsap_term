#include "../../src/platform/wayland/clipboard.c"
#include "common/loop.h"
#include "wire.h"
#include <assert.h>
#include <fcntl.h>
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

static void receive(const char *text, size_t len, const char *drop_mime,
                    struct wl_surface *surface, struct yasl_private_vector2d pos,
                    uint64_t request_id,
                    enum yasl_private_transfer_status status, void *ud)
{
    (void)surface; (void)pos;
    struct received *r = ud;
    assert(!drop_mime && len < sizeof(r->text));
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

/* Records a payload without a copy, so the test can send more than one buffer. */
struct large_received {
    size_t len;
    uint64_t request_id;
    char first, last;
    enum yasl_private_transfer_status status;
    int count;
};

static void receive_large(const char *text, size_t len, const char *drop_mime,
                          struct wl_surface *surface, struct yasl_private_vector2d pos,
                          uint64_t request_id,
                          enum yasl_private_transfer_status status, void *ud)
{
    (void)surface; (void)pos;
    struct large_received *r = ud;
    assert(!drop_mime && status == YASL_PRIVATE_TRANSFER_OK && text && len);
    r->len = len;
    r->request_id = request_id;
    r->first = text[0];
    r->last = text[len - 1];
    r->status = status;
    r->count++;
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

/* True when the client offered mime for source. */
static bool offers_mime(const struct wire_inbox *in, uint32_t source, const char *mime)
{
    struct wire_message m;
    size_t pos = 0;
    bool found = false;
    while (wire_next(in, &pos, &m)) {
        if (m.object != source || m.opcode != 0)
            continue;
        size_t offset = 0;
        if (strcmp(wire_get_string(m.args, &offset), mime) == 0)
            found = true;
    }
    return found;
}

/* First argument of the last request with opcode sent to object. */
static uint32_t last_argument(const struct wire_inbox *in, uint32_t object, uint16_t opcode)
{
    struct wire_message m;
    size_t pos = 0;
    uint32_t value = 0;
    while (wire_next(in, &pos, &m))
        if (m.object == object && m.opcode == opcode)
            value = wire_u32(m.args);
    assert(value);
    return value;
}

/* Plays one paste: the compositor asks source for mime and reads the bytes back. */
static size_t request_target(struct wl_display *display, int server, struct yasl_loop *loop,
                             uint32_t source, uint16_t send_opcode, const char *mime,
                             char *out, size_t cap)
{
    int fds[2];
    assert(pipe2(fds, O_CLOEXEC) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    uint8_t args[128];
    size_t n = wire_put_string(args, mime);
    wire_event(server, source, send_opcode, args, n, fds[1]);
    close(fds[1]);
    wire_pump(display);
    size_t len = 0;
    for (int spins = 0; ; spins++) {
        assert(spins < 100 && len < cap);
        assert(yasl_loop_step(loop, 0) >= 0);
        ssize_t got = read(fds[0], out + len, cap - len);
        if (got == 0)
            break;
        if (got > 0)
            len += (size_t)got;
        else
            assert(errno == EAGAIN);
    }
    close(fds[0]);
    return len;
}

/* The text targets: STRING and TEXT carry Latin-1, the UTF-8 targets carry UTF-8. */
static void test_text_targets(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *display = wl_display_connect_to_fd(sockets[0]);
    assert(display);
    struct wl_data_device_manager *manager = (struct wl_data_device_manager *)
        wl_proxy_create((struct wl_proxy *)display, &wl_data_device_manager_interface);
    struct wl_seat *seat = (struct wl_seat *)
        wl_proxy_create((struct wl_proxy *)display, &wl_seat_interface);
    struct yasl_loop *loop = yasl_loop_create();
    assert(manager && seat && loop);
    struct clipboard *c = clipboard_create(display, manager, seat, loop, NULL, NULL);
    assert(c);
    uint32_t device = wl_proxy_get_id((struct wl_proxy *)clipboard_data_device(c));
    struct wire_inbox in = {0};
    char out[64];

    static const char utf8[] = "caf\xc3\xa9 \xc2\xa3"; /* café £ */
    static const char latin1[] = "caf\xe9 \xa3";
    assert(clipboard_copy(c, utf8, 1));
    wire_flush(display);
    wire_read(sockets[1], &in);
    uint32_t source = last_argument(&in, device, 1 /* data device set_selection */);
    assert(offers_mime(&in, source, "STRING") && offers_mime(&in, source, "TEXT"));
    const char *utf8_targets[] = {"UTF8_STRING", "text/plain;charset=utf-8", "text/plain"};
    for (size_t i = 0; i < sizeof(utf8_targets) / sizeof(utf8_targets[0]); i++) {
        size_t n = request_target(display, sockets[1], loop, source, 1 /* send */,
                                  utf8_targets[i], out, sizeof(out));
        assert(n == sizeof(utf8) - 1 && memcmp(out, utf8, n) == 0);
    }
    const char *latin1_targets[] = {"STRING", "TEXT"};
    for (size_t i = 0; i < sizeof(latin1_targets) / sizeof(latin1_targets[0]); i++) {
        size_t n = request_target(display, sockets[1], loop, source, 1 /* send */,
                                  latin1_targets[i], out, sizeof(out));
        assert(n == sizeof(latin1) - 1 && memcmp(out, latin1, n) == 0);
    }

    /* Text that Latin-1 cannot hold offers no Latin-1 target. */
    assert(clipboard_copy(c, "\xe5\xad\x97", 2)); /* 字 */
    wire_flush(display);
    wire_read(sockets[1], &in);
    source = last_argument(&in, device, 1);
    assert(offers_mime(&in, source, "UTF8_STRING"));
    assert(!offers_mime(&in, source, "STRING") && !offers_mime(&in, source, "TEXT"));

    /* The primary selection follows the same rules. */
    struct zwp_primary_selection_device_manager_v1 *primary = (struct zwp_primary_selection_device_manager_v1 *)
        wl_proxy_create((struct wl_proxy *)display, &zwp_primary_selection_device_manager_v1_interface);
    assert(primary);
    clipboard_set_primary_manager(c, primary);
    assert(clipboard_copy_primary(c, utf8, 3));
    wire_flush(display);
    wire_read(sockets[1], &in);
    uint32_t primary_device = last_argument(&in, wl_proxy_get_id((struct wl_proxy *)primary),
                                            1 /* get_device */);
    uint32_t primary_source = last_argument(&in, primary_device, 0 /* set_selection */);
    assert(offers_mime(&in, primary_source, "STRING") && offers_mime(&in, primary_source, "TEXT"));
    size_t n = request_target(display, sockets[1], loop, primary_source, 0 /* send */,
                              "STRING", out, sizeof(out));
    assert(n == sizeof(latin1) - 1 && memcmp(out, latin1, n) == 0);
    n = request_target(display, sockets[1], loop, primary_source, 0 /* send */,
                       "UTF8_STRING", out, sizeof(out));
    assert(n == sizeof(utf8) - 1 && memcmp(out, utf8, n) == 0);

    clipboard_destroy(c);
    wl_proxy_destroy((struct wl_proxy *)primary);
    wl_proxy_destroy((struct wl_proxy *)seat);
    wl_proxy_destroy((struct wl_proxy *)manager);
    yasl_loop_destroy(loop);
    wl_display_disconnect(display);
    close(sockets[1]);
}

/* A paste larger than the old 256 KiB limit reaches the application whole. */
static void test_large_paste(void)
{
    struct large_received got = {0};
    struct clipboard c = {.loop = yasl_loop_create(), .receive = receive_large, .ud = &got};
    assert(c.loop);
    int fds[2];
    assert(pipe(fds) == 0);
    int flags = fcntl(fds[1], F_GETFL);
    assert(flags >= 0 && fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) == 0);
    struct transfer *transfer = start_transfer_text(&c, fds[0], false, NULL);
    assert(transfer);
    transfer->paste = true;
    transfer->request_id = 77;
    static char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    size_t total = 1024 * 1024, sent = 0;
    for (int spins = 0; sent < total; spins++) {
        assert(spins < 10000);
        ssize_t n = write(fds[1], chunk, sizeof(chunk));
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        assert(n < 0 && (errno == EAGAIN || errno == EINTR));
        assert(yasl_loop_step(c.loop, 0) >= 0);
    }
    close(fds[1]);
    for (int steps = 0; c.transfers && steps < 10000; steps++)
        assert(yasl_loop_step(c.loop, 0) >= 0);
    assert(!c.transfers);
    assert(got.count == 1 && got.len == total && got.request_id == 77);
    assert(got.first == 'x' && got.last == 'x');
    yasl_loop_destroy(c.loop);
}

int main(void)
{
    test_closed_receiver(false);
    test_closed_receiver(true);
    test_text_targets();
    test_large_paste();
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
        struct transfer *transfer = start_transfer_text(&c, fds[0], false, c.text);
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
    struct transfer *pending = start_transfer_text(&c, cancel_fds[0], false, NULL);
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
    struct transfer *timed = start_transfer_text(&c, timeout_fds[0], false, NULL);
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
