#ifndef SESSION_SESSION_H
#define SESSION_SESSION_H

#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "session/write_queue.h"
#include "pty/pty.h"
#include "pty/ring.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include "util/geom.h"

struct yasl_loop;
struct session;

enum session_event_flags {
    SESSION_EVENT_OUTPUT          = 1u << 0,
    SESSION_EVENT_TITLE           = 1u << 1,
    SESSION_EVENT_CLIPBOARD       = 1u << 2,
    SESSION_EVENT_NOTIFY          = 1u << 3,
    SESSION_EVENT_BELL            = 1u << 4,
    SESSION_EVENT_ARM_WRITE_WATCH = 1u << 5,
    SESSION_EVENT_ENDED           = 1u << 6,
};

struct session_notify_item {
    char *title;
    char *body;
};

#define SESSION_NOTIFY_FIFO_CAP 16

struct session_mailbox {
    pthread_mutex_t lock;
    _Atomic uint32_t pending_events;
    _Atomic bool need_wake;
    char *pending_title;
    char *pending_clipboard;
    struct session_notify_item notify_fifo[SESSION_NOTIFY_FIFO_CAP];
    int notify_head;
    int notify_count;
    int event_fd;
    int event_wr;
    int event_watch;
};

struct session_host {
    void (*output)(void *ud);
    void (*title)(void *ud);
    void (*clipboard)(void *ud, const char *text);
    void (*exited)(void *ud);
    void (*bell)(void *ud);
};

struct session_spec {
    const char *command;
    const char *cwd;
    struct vector2i grid;
    int scrollback;
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_blink;
};

struct session {
    alignas(64) struct pty_ring ring;
    char *title;
    struct yasl_loop *loop;
    struct wsap_vtcore *term;
    struct session_write_queue input_writer;
    const struct session_host *host;
    void *host_ud;
    struct session *next_live;
    int pty_watch[2];
    bool newline_synced;
    int newline_fd;
    bool onlcr;
    uint64_t last_keystroke_ms;
    bool reaped;
    struct session_mailbox mailbox;
    alignas(64) struct pty pty;
};

bool session_init(struct session *s, struct yasl_loop *loop,
                  const struct session_spec *spec);
void session_fini(struct session *s);
void session_set_host(struct session *s, const struct session_host *host, void *ud);

void session_resize(struct session *s, struct vector2i grid);
bool session_write_bytes(struct session *s, const char *data, size_t len);
bool session_paste(struct session *s, const char *text, size_t len);
const char *session_title(const struct session *s);
bool session_cwd(const struct session *s, char *out, size_t sz);

void session_on_keystroke(struct session *s, uint64_t time_ms);
uint64_t session_last_keystroke_ms(const struct session *s);

void session_reap_exited(void);

#endif
