#define _GNU_SOURCE
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "util/grow.h"
#include "util/png.h"
#include "ui/yasl/debug_internal.h"
#include "yasl/yasl.h"
#include "util/time.h"

#define DBG_TIMEOUT_MS 3000
#define DBG_TICK_MS 50

struct dbg_pending {
    struct dbg_client *client;
    struct window *window;
    int surface;
    enum dbg_pending_kind kind;
    uint64_t deadline_ns;
    char *path;
};

static struct {
    struct yasl_loop *loop;
    int listen_fd;
    int listen_watch;
    uint64_t timer;
    uint64_t resume_timer;
    struct dbg_client *clients;
    struct dbg_pending *pending;
    int n_pending, pending_cap;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} srv = { .listen_fd = -1, .listen_watch = -1 };

/* Kept apart from srv: they outlive the server. */
static struct window_debug_hooks hooks;

void window_debug_set_hooks(const struct window_debug_hooks *h)
{
    hooks = h ? *h : (struct window_debug_hooks){0};
}

const struct window_debug_hooks *dbg_hooks(void)
{
    return &hooks;
}

/* ---- replies ------------------------------------------------------ */

static void client_flush(struct dbg_client *c)
{
    if (c->fd < 0)
        return;
    while (c->out_len && !c->dead) {
        ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                c->dead = true;
            break;
        }
        memmove(c->out, c->out + n, c->out_len - (size_t)n);
        c->out_len -= (size_t)n;
    }
    if (!c->dead && c->watch >= 0)
        yasl_loop_set_fd_events(srv.loop, c->watch, POLLIN | (c->out_len ? POLLOUT : 0));
}

static void client_send(struct dbg_client *c, const char *data, size_t len)
{
    if (!c || c->dead)
        return;
    if (c->out_len + len + 1 > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < c->out_len + len + 1)
            cap *= 2;
        char *out = realloc(c->out, cap);
        if (!out) {
            c->dead = true;
            return;
        }
        c->out = out;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    c->out[c->out_len++] = '\n';
    client_flush(c);
}

static void send_json(struct dbg_client *c, struct json_writer *j)
{
    size_t len;
    char *line = json_take(j, &len);
    if (line)
        client_send(c, line, len);
    else {
        static const char error[] = "{\"ok\":false,\"error\":\"JSON encoding failed\"}";
        client_send(c, error, sizeof(error) - 1);
    }
    free(line);
}

void dbg_reply_ok(struct dbg_client *c, struct json_writer *result)
{
    struct json_writer j = {0};
    json_obj_begin(&j);
    json_key(&j, "ok"); json_bool(&j, true);
    json_key(&j, "result");
    if (result)
        json_append(&j, result);
    else
        json_null(&j);
    json_obj_end(&j);
    send_json(c, &j);
}

void dbg_reply_err(struct dbg_client *c, const char *msg)
{
    struct json_writer j = {0};
    json_obj_begin(&j);
    json_key(&j, "ok"); json_bool(&j, false);
    json_key(&j, "error"); json_str(&j, msg);
    json_obj_end(&j);
    send_json(c, &j);
}

/* ---- pending frames ----------------------------------------------- */

static void client_process(struct dbg_client *c);

static uint64_t resume_clients(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)ud; (void)id; (void)interval_ns;
    srv.resume_timer = 0;
    for (struct dbg_client *c = srv.clients; c; c = c->next)
        client_process(c);
    return 0;
}

/* Buffered lines resume from the loop, never from inside the frame or
 * timer that answered: they may inject input or close windows. */
static void pending_finish(int i)
{
    struct dbg_client *c = srv.pending[i].client;
    free(srv.pending[i].path);
    srv.pending[i] = srv.pending[--srv.n_pending];
    if (!c)
        return;
    c->waiting = false;
    if (!srv.resume_timer)
        srv.resume_timer = yasl_loop_add_timer(srv.loop, MS_TO_NS(1), resume_clients, NULL);
}

static uint64_t pending_timer(void *ud, uint64_t id, uint64_t interval_ns);

static void arm_timer(void)
{
    if (!srv.timer && srv.n_pending > 0)
        srv.timer = yasl_loop_add_timer(srv.loop, MS_TO_NS(DBG_TICK_MS), pending_timer, NULL);
}

static uint64_t pending_timer(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)ud; (void)id; (void)interval_ns;
    srv.timer = 0;
    uint64_t now = yasl_time_ns();
    for (int i = srv.n_pending - 1; i >= 0; i--) {
        if (now < srv.pending[i].deadline_ns)
            continue;
        if (srv.pending[i].client)
            dbg_reply_err(srv.pending[i].client, "timed out waiting for a frame");
        pending_finish(i);
    }
    arm_timer();
    return 0;
}

bool dbg_pending_add(struct dbg_client *c, struct window *w, int surface,
                     enum dbg_pending_kind kind, const char *path,
                     char *err, size_t err_size)
{
    if (!window_surface(w)) {
        snprintf(err, err_size, "window has no surface, so it draws no frames");
        return false;
    }
    if (window_suspended(w)) {
        snprintf(err, err_size, "window is suspended, so it draws no frames");
        return false;
    }
    char *owned = path ? strdup(path) : NULL;
    if ((path && !owned) || !UTIL_GROW(srv.pending, srv.pending_cap, srv.n_pending + 1)) {
        free(owned);
        snprintf(err, err_size, "out of memory");
        return false;
    }
    srv.pending[srv.n_pending++] = (struct dbg_pending){
        .client = c, .window = w, .surface = surface, .kind = kind,
        .deadline_ns = yasl_time_ns() + (uint64_t)DBG_TIMEOUT_MS * 1000000u,
        .path = owned,
    };
    c->waiting = true;
    arm_timer();
    dbg_redraw(w, surface);
    return true;
}

bool window_debug_wants_pixels(const struct window *w, int surface)
{
    for (int i = 0; i < srv.n_pending; i++)
        if (srv.pending[i].window == w && srv.pending[i].surface == surface &&
            srv.pending[i].kind == DBG_PENDING_SHOT)
            return true;
    return false;
}

static void answer(struct dbg_pending *p, const struct window *w,
                   const struct window_frame *frame)
{
    struct json_writer j = {0};
    if (p->kind == DBG_PENDING_FRAME) {
        json_obj_begin(&j);
        json_key(&j, "frame"); json_uint(&j, window_frame_count(w));
        json_obj_end(&j);
        dbg_reply_ok(p->client, &j);
    } else if (!frame || !frame->target.pixels) {
        dbg_reply_err(p->client, "surface has no CPU pixels to capture");
    } else if (!png_write_xrgb(p->path, frame->target.pixels,
                               frame->target.size, frame->target.size.x)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot write %s: %s", p->path, strerror(errno));
        dbg_reply_err(p->client, msg);
    } else {
        json_obj_begin(&j);
        json_key(&j, "path"); json_str(&j, p->path);
        json_key(&j, "size");
        json_vec(&j, frame->target.size);
        json_obj_end(&j);
        dbg_reply_ok(p->client, &j);
    }
}

void window_debug_presented(struct window *w, int surface,
                            const struct window_frame *frame)
{
    for (int i = srv.n_pending - 1; i >= 0; i--) {
        struct dbg_pending *p = &srv.pending[i];
        if (p->window != w || p->surface != surface)
            continue;
        if (p->client)
            answer(p, w, frame);
        pending_finish(i);
    }
}

void window_debug_destroy(struct window *w)
{
    for (int i = srv.n_pending - 1; i >= 0; i--) {
        if (srv.pending[i].window != w)
            continue;
        if (srv.pending[i].client)
            dbg_reply_err(srv.pending[i].client, "window closed");
        pending_finish(i);
    }
    window_debug_free(w);
}

/* ---- clients ------------------------------------------------------ */

static void client_close(struct dbg_client *c)
{
    for (int i = 0; i < srv.n_pending; i++)
        if (srv.pending[i].client == c)
            srv.pending[i].client = NULL;
    for (struct dbg_client **pp = &srv.clients; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    if (c->watch >= 0)
        yasl_loop_del_fd(srv.loop, c->watch);
    close(c->fd);
    free(c->out);
    free(c->in);
    free(c);
}

/* Runs buffered lines until one leaves a reply pending. */
static void client_process(struct dbg_client *c)
{
    while (!c->waiting && !c->dead) {
        char *nl = c->in_len ? memchr(c->in, '\n', c->in_len) : NULL;
        if (!nl)
            break;
        size_t used = (size_t)(nl - c->in) + 1;
        char *line = malloc(used);
        if (line) {
            memcpy(line, c->in, used - 1);
            line[used - 1] = '\0';
        }
        memmove(c->in, c->in + used, c->in_len - used);
        c->in_len -= used;
        if (!line) {
            c->dead = true;
            break;
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[len - 1] = '\0';
        dbg_run_line(c, line);
        free(line);
    }
}

static bool client_read(struct dbg_client *c)
{
    for (;;) {
        if (c->in_len == c->in_cap) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 4096;
            char *grown = realloc(c->in, cap);
            if (!grown)
                return false;
            c->in = grown;
            c->in_cap = cap;
        }
        ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            client_process(c);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

static void client_ready(void *ud, int fd, short revents)
{
    (void)fd;
    struct dbg_client *c = ud;
    if (revents & POLLOUT)
        client_flush(c);
    if ((revents & (POLLIN | POLLHUP | POLLERR)) && !client_read(c))
        c->dead = true;
    if (c->dead)
        client_close(c);
}

static bool peer_is_owner(int fd)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t len = sizeof(cred);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
           cred.uid == getuid();
#else
    uid_t uid;
    gid_t gid;
    return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#endif
}

static void listen_ready(void *ud, int fd, short revents)
{
    (void)revents;
    (void)ud;
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0)
            break;
        if (!peer_is_owner(cfd) ||
            fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK) < 0) {
            close(cfd);
            continue;
        }
        fcntl(cfd, F_SETFD, FD_CLOEXEC);
        struct dbg_client *c = calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }
        c->fd = cfd;
        c->watch = yasl_loop_add_fd(srv.loop, cfd, POLLIN, client_ready, c);
        if (c->watch < 0) {
            close(cfd);
            free(c);
            continue;
        }
        c->next = srv.clients;
        srv.clients = c;
    }
}

/* ---- the socket --------------------------------------------------- */

static int unix_socket(bool nonblock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        (nonblock && (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0))) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Replaces a stale socket left by a crashed run, never another file. */
static bool clear_stale_socket(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return errno == ENOENT;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid())
        return false;
    int probe = unix_socket(false);
    if (probe < 0)
        return false;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, path, strlen(path) + 1);
    bool live = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(probe);
    return !live && unlink(path) == 0;
}

char *window_debug_eval(const char *line)
{
    struct dbg_client c = { .fd = -1 };
    char *buf = strdup(line ? line : "");
    if (buf)
        dbg_run_line(&c, buf);
    free(buf);
    free(c.in);
    if (c.out && c.out_len)
        c.out[c.out_len - 1] = '\0';
    return c.out;
}

bool window_debug_server_start(struct yasl_loop *loop, struct window_host *host,
                               const char *path)
{
    if (dbg_server_running || !path || !*path)
        return false;
    if (strlen(path) >= sizeof(srv.path)) {
        fprintf(stderr, "[debug] socket path too long: %s\n", path);
        return false;
    }
    if (!clear_stale_socket(path)) {
        fprintf(stderr, "[debug] %s exists and is not a stale socket\n", path);
        return false;
    }
    int fd = unix_socket(true);
    if (fd < 0) {
        perror("[debug] socket");
        return false;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, path, strlen(path) + 1);
    mode_t old_mask = umask(0177);
    int bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_mask);
    if (bound < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[debug] cannot listen on %s: %s\n", path, strerror(errno));
        close(fd);
        return false;
    }
    int watch = yasl_loop_add_fd(loop, fd, POLLIN, listen_ready, NULL);
    if (watch < 0) {
        close(fd);
        unlink(path);
        return false;
    }
    srv.loop = loop;
    srv.listen_fd = fd;
    srv.listen_watch = watch;
    memcpy(srv.path, path, strlen(path) + 1);
    dbg_server_running = true;
    fprintf(stderr, "[debug] UI inspector listening on %s\n", path);
    for (int i = 0; i < host->count; i++)
        dbg_redraw(host->windows[i], WINDOW_DEBUG_MAIN);
    return true;
}

void window_debug_server_stop(void)
{
    if (!dbg_server_running)
        return;
    while (srv.clients)
        client_close(srv.clients);
    while (srv.n_pending)
        pending_finish(srv.n_pending - 1);
    free(srv.pending);
    if (srv.timer)
        yasl_loop_del_timer(srv.loop, srv.timer);
    if (srv.resume_timer)
        yasl_loop_del_timer(srv.loop, srv.resume_timer);
    if (srv.listen_watch >= 0)
        yasl_loop_del_fd(srv.loop, srv.listen_watch);
    close(srv.listen_fd);
    unlink(srv.path);
    srv = (typeof(srv)){ .listen_fd = -1, .listen_watch = -1 };
    dbg_server_running = false;
}
