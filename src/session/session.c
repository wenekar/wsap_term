#define _GNU_SOURCE
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
#include "util/time.h"

/* The terminal lock is released between pieces, so the loop thread can take a
 * snapshot of a flood without a long wait. */
#define SESSION_FEED_PIECE (32u * 1024u)

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

/* Runs on the worker thread. */
static void session_feed(struct session *s, const char *buf, size_t len)
{
    unsigned flags = s->onlcr ? WSAP_VTCORE_FEED_ONLCR : 0;
    while (len) {
        size_t piece = len < SESSION_FEED_PIECE ? len : SESSION_FEED_PIECE;
        wsap_vtcore_feed(s->term, buf, piece, flags);
        buf += piece;
        len -= piece;
    }
}

enum session_host_event_kind {
    SESSION_HOST_TITLE,
    SESSION_HOST_CLIPBOARD,
    SESSION_HOST_NOTIFY,
    SESSION_HOST_BELL,
};

struct session_host_event {
    struct session_host_event *next;
    enum session_host_event_kind kind;
    char *text;
    char *body;
};

/* Runs on the worker thread: it queues the payload for the loop thread and
 * never touches the host. The ring signals the loop thread, so a queued event
 * reaches it without a wait here. */
static void host_event_push(struct session *s, enum session_host_event_kind kind,
                            char *text, char *body)
{
    struct session_host_event *event = malloc(sizeof(*event));
    if (!event) {
        free(text);
        free(body);
        return;
    }
    *event = (struct session_host_event){
        .kind = kind,
        .text = text,
        .body = body,
    };
    pthread_mutex_lock(&s->host_lock);
    if (s->host_tail)
        s->host_tail->next = event;
    else
        s->host_head = event;
    s->host_tail = event;
    pthread_mutex_unlock(&s->host_lock);
    pty_ring_wake(&s->ring);
}

/* Runs on the loop thread. */
static void host_event_apply(struct session *s, struct session_host_event *event)
{
    switch (event->kind) {
    case SESSION_HOST_TITLE:
        free(s->title);
        s->title = event->text;
        event->text = NULL;
        if (s->host && s->host->title)
            s->host->title(s->host_ud);
        break;
    case SESSION_HOST_CLIPBOARD:
        if (event->text && s->host && s->host->clipboard)
            s->host->clipboard(s->host_ud, event->text);
        break;
    case SESSION_HOST_NOTIFY:
        platform_notify(event->text, event->body);
        break;
    case SESSION_HOST_BELL:
        platform_bell();
        if (s->host && s->host->bell)
            s->host->bell(s->host_ud);
        break;
    }
    free(event->text);
    free(event->body);
}

/* Runs on the loop thread. It applies every queued event in order, so the
 * host sees the title and the bell of the output before the end of the
 * session. */
static void host_events_dispatch(struct session *s)
{
    pthread_mutex_lock(&s->host_lock);
    struct session_host_event *event = s->host_head;
    s->host_head = s->host_tail = NULL;
    pthread_mutex_unlock(&s->host_lock);
    while (event) {
        struct session_host_event *next = event->next;
        host_event_apply(s, event);
        free(event);
        event = next;
    }
}

static void host_events_free(struct session *s)
{
    if (!s->host_lock_ready)
        return;
    pthread_mutex_lock(&s->host_lock);
    struct session_host_event *event = s->host_head;
    s->host_head = s->host_tail = NULL;
    pthread_mutex_unlock(&s->host_lock);
    while (event) {
        struct session_host_event *next = event->next;
        free(event->text);
        free(event->body);
        free(event);
        event = next;
    }
    pthread_mutex_destroy(&s->host_lock);
    s->host_lock_ready = false;
}

static void on_term_response(void *ud, const char *data, size_t len)
{
    struct session *s = ud;
    if (!session_write_queue_push_bytes(&s->input_writer, data, len))
        fprintf(stderr, "[%s] failed to queue terminal input\n", app_name());
}

static void on_term_title(void *ud, const char *title)
{
    if (!title || !title[0])
        return;
    char *copy = strdup(title);
    if (copy)
        host_event_push(ud, SESSION_HOST_TITLE, copy, NULL);
}

static void on_term_clipboard(void *ud, const char *selection, const char *data,
                              size_t len)
{
    if (!data || !strchr(selection, 'c'))
        return;
    char *text = strndup(data, len);
    if (text)
        host_event_push(ud, SESSION_HOST_CLIPBOARD, text, NULL);
}

static void on_term_notify(void *ud, const char *title, const char *body)
{
    char *title_copy = title ? strdup(title) : NULL;
    char *body_copy = body ? strdup(body) : NULL;
    if ((title && !title_copy) || (body && !body_copy)) {
        free(title_copy);
        free(body_copy);
        return;
    }
    host_event_push(ud, SESSION_HOST_NOTIFY, title_copy, body_copy);
}

static void on_term_bell(void *ud)
{
    host_event_push(ud, SESSION_HOST_BELL, NULL, NULL);
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

/* Runs on the worker thread: it parses the chunk into the terminal. */
static void on_ring_data(void *ud, int fd, const char *buf, size_t len)
{
    struct session *s = ud;
    if (!s->newline_synced || s->newline_fd != fd) {
        sync_transport_newline(s, fd);
        s->newline_synced = true;
        s->newline_fd = fd;
    }
    session_feed(s, buf, len);
    latency_probe_scan_output(buf, len);
}

/* Runs on the loop thread: it applies the queued host events and asks for a
 * frame. It parses no bytes and holds no budget. */
static void on_ring_wake(void *ud)
{
    struct session *s = ud;
    host_events_dispatch(s);
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
    .wake = on_ring_wake,
    .ended = on_ring_ended,
};

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

    /* The queue comes first: the worker can answer the terminal during its
     * first read. */
    if (pty_master_fd(&s->pty) < 0 ||
        !session_write_queue_bind(&s->input_writer, s->loop, pty_master_fd(&s->pty)))
        return false;
    if (pty_ring_start(&s->ring, s->loop, &s->pty, &ring_callbacks, s) < 0) {
        fprintf(stderr, "[%s] pty_ring_start failed\n", app_name());
        return false;
    }
    return true;
}

bool session_init(struct session *s, struct yasl_loop *loop,
                  const struct session_spec *spec)
{
    memset(s, 0, sizeof(*s));
    s->loop = loop;
    if (pthread_mutex_init(&s->host_lock, NULL) != 0)
        return false;
    s->host_lock_ready = true;

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
    if (!s->term) {
        session_fini(s);
        return false;
    }
    wsap_vtcore_set_color_scheme(s->term, spec->color_scheme);

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
    if (s->pty_resize_timer)
        yasl_loop_del_timer(s->loop, s->pty_resize_timer);
    /* Stop the worker before the terminal and the queue go away. */
    pty_ring_stop(&s->ring);
    session_write_queue_destroy(&s->input_writer);
    host_events_free(s);
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

/* The shortest time between two winsize changes that the shell sees. */
#define PTY_RESIZE_INTERVAL_MS 40

static void send_pty_size(struct session *s, struct vector2i grid)
{
    pty_resize(&s->pty, grid);
    s->pty_resize_ns = yasl_time_ns();
    s->pty_pending = (struct vector2i){0};
}

static uint64_t on_pty_resize_timer(void *ud, uint64_t id, uint64_t delay_ns)
{
    struct session *s = ud;
    (void)id;
    (void)delay_ns;
    s->pty_resize_timer = 0;
    if (s->pty_pending.x > 0 && s->pty_pending.y > 0 && pty_master_fd(&s->pty) >= 0)
        send_pty_size(s, s->pty_pending);
    return 0;
}

void session_resize(struct session *s, struct vector2i grid)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(s->term);
    if (grid.x == size.x && grid.y == size.y)
        return;
    if (!wsap_vtcore_resize(s->term, (struct wsap_vtcore_vector2i){ .x = grid.x, .y = grid.y }))
        return;
    if (pty_master_fd(&s->pty) < 0)
        return;
    uint64_t now = yasl_time_ns();
    uint64_t interval = MS_TO_NS(PTY_RESIZE_INTERVAL_MS);
    uint64_t age = now - s->pty_resize_ns;
    if (!s->pty_resize_ns || age >= interval) {
        send_pty_size(s, grid);
        return;
    }
    s->pty_pending = grid;
    if (s->pty_resize_timer)
        return;
    s->pty_resize_timer = yasl_loop_add_timer(s->loop, interval - age, on_pty_resize_timer, s);
    if (!s->pty_resize_timer)
        send_pty_size(s, grid);
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

void session_reap_exited(void)
{
    for (struct session *s = live_sessions; s; s = s->next_live) {
        if (s->pty.pid > 0 && waitpid(s->pty.pid, NULL, WNOHANG) == s->pty.pid) {
            s->pty.pid = 0;
            s->reaped = true;
        }
    }
    for (struct session *s = live_sessions; s; s = s->next_live) {
        if (!s->reaped)
            continue;
        s->reaped = false;
        /* The worker drains the last output and then publishes the end. The
         * exit callback runs on the loop thread after the last byte. */
        pty_ring_end(&s->ring);
    }
}
