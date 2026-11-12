#ifndef SESSION_SESSION_H
#define SESSION_SESSION_H

#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>

#include "session/write_queue.h"
#include "pty/pty.h"
#include "pty/ring.h"
#include "wsap_vtcore/wsap_vtcore.h"
#include "util/geom.h"

struct yasl_loop;
struct session;
struct session_host_event;

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
    const char *locale;
    struct vector2i grid;
    int scrollback;
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_color_scheme color_scheme;
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
    uint64_t last_keystroke_ms;
    /* The last winsize sent to the pty, the size that waits for the timer, and
     * the timer. Zero Is Initialization: no pending size, no timer. */
    uint64_t pty_resize_ns;
    uint64_t pty_resize_timer;
    struct vector2i pty_pending;
    /* The host events that the worker thread queues for the loop thread: the
     * title, the clipboard, a notification, and the bell. The loop thread
     * applies them, so the host and the window see one thread only. */
    pthread_mutex_t host_lock;
    struct session_host_event *host_head;
    struct session_host_event *host_tail;

    /* The newline mode of the bytes that the worker reads, and the stream that
     * they came from. */
    int newline_fd;
    bool onlcr;
    bool newline_synced;

    bool reaped;
    bool host_lock_ready;
    alignas(64) struct pty pty;
};

/*
 * Initializes terminal session `s`, spawns the child process, and starts reader threads.
 * Returns true on success, or false on failure.
 */
bool session_init(struct session *s, struct yasl_loop *loop,
                  const struct session_spec *spec);

/* Terminates the child process, stops worker threads, and releases session resources. */
void session_fini(struct session *s);

/* Sets the event host callbacks and user context pointer for session `s`. */
void session_set_host(struct session *s, const struct session_host *host, void *ud);

/*
 * Resizes the terminal emulator grid immediately.
 * Throttles window size updates sent to the pseudo-terminal.
 */
void session_resize(struct session *s, struct vector2i grid);

/*
 * Enqueues raw bytes for writing to the pseudo-terminal.
 * Returns true on success, or false on failure.
 */
bool session_write_bytes(struct session *s, const char *data, size_t len);

/*
 * Enqueues clipboard text for writing to the pseudo-terminal with bracketed paste framing.
 * Returns true on success, or false on failure.
 */
bool session_paste(struct session *s, const char *text, size_t len);

/* Returns the current window title of session `s`, or "shell" when unset. */
const char *session_title(const struct session *s);

/*
 * Copies current working directory of session `s` into buffer `out`.
 * Returns true on success, or false on failure.
 */
bool session_cwd(const struct session *s, char *out, size_t sz);

/* Records timestamp `time_ms` for the most recent keystroke in session `s`. */
void session_on_keystroke(struct session *s, uint64_t time_ms);

/* Returns the timestamp of the most recent keystroke in milliseconds. */
uint64_t session_last_keystroke_ms(const struct session *s);

/* Reaps exited child processes for all active sessions and notifies hosts. */
void session_reap_exited(void);

#endif
