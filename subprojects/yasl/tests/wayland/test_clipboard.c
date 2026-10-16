#include "../../src/platform/wayland/clipboard.c"
#include "common/loop.h"
#include <assert.h>

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

int main(void)
{
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
