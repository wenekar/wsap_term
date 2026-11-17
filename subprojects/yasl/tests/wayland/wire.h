/* The compositor end of a socketpair for the Wayland unit tests. The test
 * sends events as raw wire messages and reads the client's requests the same
 * way. Opcodes follow the core protocol and the primary selection XML. */
#ifndef YASL_TEST_WAYLAND_WIRE_H
#define YASL_TEST_WAYLAND_WIRE_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <wayland-client.h>

#define WIRE_MAX_FDS 8
#define WIRE_BYTES 8192

/* Client requests, in order. File descriptors arrive out of band in fds. */
struct wire_inbox {
    uint8_t bytes[WIRE_BYTES];
    size_t len;
    int fds[WIRE_MAX_FDS];
    size_t nfds;
};

struct wire_message {
    uint32_t object;
    uint16_t opcode;
    const uint8_t *args;
    size_t args_len;
};

static inline size_t wire_pad4(size_t n)
{
    return (n + 3) & ~(size_t)3;
}

static inline uint32_t wire_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* Writes a string argument: the length with its NUL, the bytes, and padding. */
static inline size_t wire_put_string(uint8_t *out, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    size_t total = 4 + wire_pad4(n);
    memset(out, 0, total);
    memcpy(out, &n, sizeof(n));
    memcpy(out + 4, s, n);
    return total;
}

/* Reads the string argument at args[*offset] and moves offset past it. */
static inline const char *wire_get_string(const uint8_t *args, size_t *offset)
{
    uint32_t n = wire_u32(args + *offset);
    const char *s = (const char *)args + *offset + 4;
    *offset += 4 + wire_pad4(n);
    return s;
}

/* Sends one event to the client. When fd is not negative, it travels with the event. */
static inline void wire_event(int sock, uint32_t object, uint16_t opcode,
                              const uint8_t *args, size_t args_len, int fd)
{
    uint32_t header[2] = {object, ((uint32_t)(8 + args_len) << 16) | opcode};
    struct iovec iov[2] = {
        {.iov_base = header, .iov_len = sizeof(header)},
        {.iov_base = (void *)args, .iov_len = args_len},
    };
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = args_len ? 2 : 1};
    if (fd >= 0) {
        memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    assert(sendmsg(sock, &msg, MSG_NOSIGNAL) == (ssize_t)(sizeof(header) + args_len));
}

/* Appends every request that the client has flushed. Never blocks. */
static inline void wire_read(int sock, struct wire_inbox *in)
{
    for (;;) {
        char control[CMSG_SPACE(sizeof(int) * WIRE_MAX_FDS)];
        struct iovec iov = {.iov_base = in->bytes + in->len,
                            .iov_len = sizeof(in->bytes) - in->len};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = control, .msg_controllen = sizeof(control)};
        ssize_t n = recvmsg(sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            return;
        }
        assert(n > 0);
        in->len += (size_t)n;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
                continue;
            size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                assert(in->nfds < WIRE_MAX_FDS);
                memcpy(&in->fds[in->nfds++], (char *)CMSG_DATA(cm) + i * sizeof(int),
                       sizeof(int));
            }
        }
    }
}

/* Returns the request at *pos and advances it, or false at the end. Scan from
 * pos zero to visit every request again. */
static inline bool wire_next(const struct wire_inbox *in, size_t *pos, struct wire_message *m)
{
    if (in->len - *pos < 8)
        return false;
    uint32_t header[2];
    memcpy(header, in->bytes + *pos, sizeof(header));
    size_t size = header[1] >> 16;
    assert(size >= 8 && *pos + size <= in->len);
    m->object = header[0];
    m->opcode = (uint16_t)(header[1] & 0xffff);
    m->args = in->bytes + *pos + 8;
    m->args_len = size - 8;
    *pos += size;
    return true;
}

/* Sends buffered client requests to the compositor end. */
static inline void wire_flush(struct wl_display *display)
{
    assert(wl_display_flush(display) >= 0);
}

/* Lets the client read and dispatch the events that the test has sent. Call it
 * only after sending at least one event, or the read blocks. */
static inline void wire_pump(struct wl_display *display)
{
    while (wl_display_prepare_read(display) != 0)
        assert(wl_display_dispatch_pending(display) >= 0);
    wire_flush(display);
    assert(wl_display_read_events(display) >= 0);
    assert(wl_display_dispatch_pending(display) >= 0);
}

#endif
