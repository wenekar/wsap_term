#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "loop/latency_probe.h"
#include "yasl/yasl.h"
#include "platform/platform.h"
#include "session/session.h"
#include "wsap_vtcore/unicode/unicode.h"
#include "util/name.h"
#include "util/proc.h"

#define PTY_READ_BUF_SZ 262144

static alignas(64) char session_read_buf[PTY_READ_BUF_SZ];

/* Every live session. Children are process-wide, so the reaping is too. */
static struct session *live_sessions;

static void notify_output(struct session *s)
{
    if (wsap_vtcore_modes(s->term).synchronized)
        return;
    if (s->host && s->host->output)
        s->host->output(s->host_ud);
}

void session_on_keystroke(struct session *s, uint64_t time_ms)
{
    s->last_keystroke_ms = time_ms;
}

uint64_t session_last_keystroke_ms(const struct session *s)
{
    return s->last_keystroke_ms;
}

static void session_feed(struct session *s, const char *buf, size_t len)
{
    wsap_vtcore_feed(s->term, buf, len, s->onlcr ? WSAP_VTCORE_FEED_ONLCR : 0);
}

static void on_term_response(void *ud, const char *data, size_t len)
{
    struct session *s = ud;
    if (!session_write_queue_push_bytes(&s->input_writer, data, len))
        fprintf(stderr, "[%s] failed to queue terminal input\n", app_name());
}

static void on_term_title(void *ud, const char *title)
{
    struct session *s = ud;
    if (!title || !title[0])
        return;
    char *copy = strdup(title);
    if (!copy)
        return;
    free(s->title);
    s->title = copy;
    if (s->host && s->host->title)
        s->host->title(s->host_ud);
}

static void on_term_clipboard(void *ud, const char *selection, const char *data,
                              size_t len)
{
    struct session *s = ud;
    if (!data || !strchr(selection, 'c') || !s->host || !s->host->clipboard)
        return;
    char *text = strndup(data, len);
    if (!text)
        return;
    s->host->clipboard(s->host_ud, text);
    free(text);
}

static void on_term_notify(void *ud, const char *title, const char *body)
{
    (void)ud;
    platform_notify(title, body);
}

static void on_term_bell(void *ud)
{
    struct session *s = ud;
    platform_bell();
    if (s && s->host && s->host->bell)
        s->host->bell(s->host_ud);
}

static void on_term_trace(void *ud, const char *line)
{
    (void)ud;
    fprintf(stderr, "%s\n", line);
}

static void sync_transport_newline(struct session *s, int fd)
{
    int master_fd = pty_master_fd(&s->pty);
    if (master_fd < 0)
        return;

    bool from_master = fd == master_fd;
    if (from_master) {
        s->onlcr = false;
        return;
    }
    if (!from_master && fd != pty_pipe_fd(&s->pty))
        return;

    struct termios tio;
    if (tcgetattr(master_fd, &tio) < 0)
        return;
    s->onlcr = (tio.c_oflag & ONLCR) && (tio.c_oflag & OPOST);
}

static bool drain_pty_fd(struct session *s, int fd, uint64_t deadline)
{
    bool any = false;
    sync_transport_newline(s, fd);
    for (;;) {
        ssize_t n = read(fd, session_read_buf, sizeof(session_read_buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        any = true;
        session_feed(s, session_read_buf, (size_t)n);
        latency_probe_scan_output(session_read_buf, (size_t)n);
        if (yasl_time_ns() >= deadline)
            break;
    }
    return any;
}

static void on_ring_data(void *ud, int fd, const char *buf, size_t len,
                         size_t backlog)
{
    struct session *s = ud;
    (void)backlog;
    if (!s->newline_synced || s->newline_fd != fd) {
        sync_transport_newline(s, fd);
        s->newline_synced = true;
        s->newline_fd = fd;
    }
    session_feed(s, buf, len);
    latency_probe_scan_output(buf, len);
}

static void on_ring_drained(void *ud)
{
    struct session *s = ud;
    s->newline_synced = false;
    notify_output(s);
}

static void report_exit(struct session *s)
{
    if (s->host && s->host->exited)
        s->host->exited(s->host_ud);
}

static void on_ring_ended(void *ud)
{
    report_exit(ud);
}

static const struct pty_ring_cbs ring_callbacks = {
    .data = on_ring_data,
    .drained = on_ring_drained,
    .ended = on_ring_ended,
};

static void on_pty_data(void *ud, int fd, short revents)
{
    (void)revents;
    struct session *s = ud;
    uint64_t deadline = yasl_time_ns() + PTY_DRAIN_BUDGET_NS;
    bool any = false;

    if (fd == pty_pipe_fd(&s->pty) && pty_master_fd(&s->pty) >= 0 &&
        !pty_ring_owns_fd(&s->ring, pty_master_fd(&s->pty)))
        any |= drain_pty_fd(s, pty_master_fd(&s->pty), deadline);
    any |= drain_pty_fd(s, fd, deadline);
    if (any)
        notify_output(s);
}

/* The term core, not pty, knows which Unicode version it implements. */
static const char *const child_env[] = {
    "UNICODE_VERSION=" WSAP_VTCORE_UNICODE_DATA_VERSION,
    NULL,
};

static bool start_pty(struct session *s, const struct session_spec *spec)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(s->term);
    struct pty_spec pty_spec = {
        .grid = { .x = size.x, .y = size.y },
        .command = spec->command,
        .cwd = spec->cwd,
        .env = child_env,
    };
    if (pty_spawn(&s->pty, &pty_spec) < 0)
        return false;

    s->onlcr = pty_pipe_fd(&s->pty) >= 0;

    if (pty_ring_start(&s->ring, s->loop, &s->pty, &ring_callbacks, s) < 0)
        fprintf(stderr, "[%s] pty_ring_start failed, falling back to direct read\n",
                app_name());

    const int fds[] = { pty_pipe_fd(&s->pty), pty_master_fd(&s->pty) };
    for (size_t i = 0; i < 2; i++) {
        if (fds[i] < 0 || pty_ring_owns_fd(&s->ring, fds[i]))
            continue;
        s->pty_watch[i] = yasl_loop_add_fd(s->loop, fds[i], POLLIN, on_pty_data, s);
        if (s->pty_watch[i] < 0)
            return false;
    }
    return session_write_queue_bind(&s->input_writer, s->loop, pty_master_fd(&s->pty));
}

bool session_init(struct session *s, struct yasl_loop *loop,
                  const struct session_spec *spec)
{
    memset(s, 0, sizeof(*s));
    s->loop = loop;

    struct vector2i grid_size = vector2i_max(spec->grid, vector2i_splat(1));
    s->term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = grid_size.x, .y = grid_size.y },
        .scrollback = spec->scrollback,
        .theme = spec->theme,
        .cursor_style = spec->cursor_style,
        .cursor_steady = !spec->cursor_blink,
        .callbacks = {
            .ud = s,
            .reply = on_term_response,
            .title = on_term_title,
            .clipboard = on_term_clipboard,
            .notify = on_term_notify,
            .bell = on_term_bell,
            .trace = getenv("WSAP_ESC_TRACE") ? on_term_trace : NULL,
        },
    });
    if (!s->term)
        return false;

    s->next_live = live_sessions;
    live_sessions = s;
    if (!start_pty(s, spec)) {
        session_fini(s);
        return false;
    }
    return true;
}

static void unlink_live(struct session *s)
{
    for (struct session **p = &live_sessions; *p; p = &(*p)->next_live) {
        if (*p == s) {
            *p = s->next_live;
            break;
        }
    }
    s->next_live = NULL;
}

void session_fini(struct session *s)
{
    unlink_live(s);
    session_write_queue_destroy(&s->input_writer);
    pty_ring_stop(&s->ring);
    for (size_t i = 0; i < 2; i++) {
        if (s->pty_watch[i] > 0)
            yasl_loop_del_fd(s->loop, s->pty_watch[i]);
        s->pty_watch[i] = 0;
    }
    pty_destroy(&s->pty);
    wsap_vtcore_free(s->term);
    free(s->title);
    memset(s, 0, sizeof(*s));
}

void session_set_host(struct session *s, const struct session_host *host, void *ud)
{
    s->host = host;
    s->host_ud = ud;
}

void session_resize(struct session *s, struct vector2i grid)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(s->term);
    if (grid.x == size.x && grid.y == size.y)
        return;
    if (!wsap_vtcore_resize(s->term, (struct wsap_vtcore_vector2i){ .x = grid.x, .y = grid.y }))
        return;
    if (pty_master_fd(&s->pty) >= 0)
        pty_resize(&s->pty, grid);
}

bool session_write_bytes(struct session *s, const char *data, size_t len)
{
    if (!s) return false;
    if (!len) return true;
    if (!session_write_queue_push_bytes(&s->input_writer, data, len)) {
        fprintf(stderr, "[%s] failed to queue terminal input\n", app_name());
        return false;
    }
    latency_probe_stage(LATENCY_STAGE_PTY_WRITTEN);
    if (pty_master_fd(&s->pty) >= 0 &&
        !pty_ring_owns_fd(&s->ring, pty_master_fd(&s->pty)) &&
        drain_pty_fd(s, pty_master_fd(&s->pty), 0))
        notify_output(s);
    return true;
}

bool session_paste(struct session *s, const char *text, size_t len)
{
    return s && len &&
           session_write_queue_push_paste(&s->input_writer, text, len, wsap_vtcore_modes(s->term).bracketed_paste);
}

const char *session_title(const struct session *s)
{
    return s && s->title ? s->title : "shell";
}

bool session_cwd(const struct session *s, char *out, size_t sz)
{
    if (!s || !out || sz == 0)
        return false;

    char *cwd = wsap_vtcore_cwd(s->term);
    bool valid = false;
    if (cwd && cwd[0] == '/' && strlen(cwd) < sz) {
        struct stat st;
        valid = stat(cwd, &st) == 0 && S_ISDIR(st.st_mode) && access(cwd, X_OK) == 0;
        if (valid)
            memcpy(out, cwd, strlen(cwd) + 1);
    }
    wsap_vtcore_text_free(cwd);
    if (valid)
        return true;

    pid_t fg_pid = pty_master_fd(&s->pty) >= 0 ? tcgetpgrp(pty_master_fd(&s->pty)) : -1;
    pid_t target_pid = fg_pid > 0 ? fg_pid : s->pty.pid;
    if (target_pid <= 0)
        return false;
    if (proc_read_pid_cwd_buf(target_pid, out, sz))
        return true;
    return target_pid != s->pty.pid && proc_read_pid_cwd_buf(s->pty.pid, out, sz);
}

/* Reaps first, then ends one session at a time: a host may destroy any
 * session from its exit callback. The exit reaches the host after the
 * child's last output: the ring reports it behind that output, and without
 * a ring the session reads the rest itself. */
void session_reap_exited(void)
{
    for (struct session *s = live_sessions; s; s = s->next_live) {
        if (s->pty.pid > 0 && waitpid(s->pty.pid, NULL, WNOHANG) == s->pty.pid) {
            s->pty.pid = 0;
            s->reaped = true;
        }
    }
    for (;;) {
        struct session *s = live_sessions;
        while (s && !s->reaped)
            s = s->next_live;
        if (!s)
            return;
        s->reaped = false;
        if (s->ring.active) {
            pty_ring_end(&s->ring);
            continue;
        }
        uint64_t deadline = yasl_time_ns() + PTY_DRAIN_BUDGET_NS;
        bool any = false;
        for (size_t i = 0; i < 2; i++)
            if (s->pty_watch[i] > 0)
                any |= drain_pty_fd(s, i ? pty_master_fd(&s->pty) : pty_pipe_fd(&s->pty),
                                    deadline);
        if (any)
            notify_output(s);
        report_exit(s);
    }
}
