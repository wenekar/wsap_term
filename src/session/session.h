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
    /* The app ID that prefixes this session's log lines. It belongs to the
     * caller and must outlive the session; NULL is allowed. */
    const char *app_id;
    const char *command;
    const char *cwd;
    const char *locale;
    struct vector2i grid, cell_pixels;
    wsap_vtcore_graphics_fn graphics;
    void *graphics_ud;
    int scrollback;
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_color_scheme color_scheme;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_blink;
};

struct session {
    const char *app_id;
    alignas(64) struct pty_ring ring;
    /* The newline policy for the worker's reads, and the stream that the
     * policy came from. Output read from the pty master keeps the kernel's
     * translation, and the fast pipe gets the ONLCR setting that the session
     * reads from the pty. */
    int newline_fd;
    bool onlcr;
    bool newline_synced;
    bool reaped;
    bool host_lock_ready;
    struct yasl_loop *loop;

    struct session_write_queue input_writer;
    struct wsap_vtcore *term;
    const struct session_host *host;

    /* The host events that the worker thread queues for the loop thread: the
     * title, the clipboard, a notification, and the bell. The loop thread
     * applies them, so every host callback runs on the loop thread. */
    pthread_mutex_t host_lock;
    struct session_host_event *host_head;
    struct session_host_event *host_tail;
    void *host_ud;

    /* The loop thread owns these handles. The reader worker holds copies of
     * the descriptors, so it never reads the struct. */
    struct pty pty;
    char *title;
    struct session *next_live;
    uint64_t last_keystroke_ms;
    /* The time of the last winsize send, the size that waits for the throttle
     * timer, and the timer. A zero time means no send yet, and a zero timer
     * means no wait. */
    uint64_t pty_resize_ns;
    uint64_t pty_resize_timer;
    struct vector2i pty_pending;
    struct vector2i cell_pixels;
};

/*
 * Initializes session `s` from `spec`, spawns the child process, and starts the
 * pty reader worker.
 * Returns false if it cannot initialize the host lock, create the terminal,
 * spawn the child, or start the write queue or reader. On failure, it
 * releases any resources it acquired. Release a successful session with
 * session_fini().
 */
bool session_init(struct session *s, struct yasl_loop *loop,
                  const struct session_spec *spec);

/*
 * Stops the reader worker, terminates the child process, and releases the
 * resources of session `s`. The caller can pass a session that session_init()
 * did not finish.
 */
void session_fini(struct session *s);

/*
 * Sets the host callbacks and user pointer of session `s`. The `struct
 * session_host` and the user pointer must stay valid until session_fini(), and
 * the session calls the handlers on the loop thread. A handler can be NULL.
 */
void session_set_host(struct session *s, const struct session_host *host, void *ud);

/*
 * Resizes the terminal grid at once. The pty gets the new winsize at once when
 * the last change is old enough, and from a timer otherwise. A fast drag then
 * sends few SIGWINCH signals to the shell, and the shell ends at the final
 * size.
 */
void session_resize(struct session *s, struct vector2i grid);

/* Updates the cell size in device pixels for terminal and PTY size reports. */
void session_set_cell_pixels(struct session *s, struct vector2i cell);

/*
 * Enqueues raw bytes for the pseudo-terminal. Any thread can call this. The
 * queue copies the bytes and writes what the pty accepts; the loop thread
 * writes the rest.
 * Returns true when the queue takes every byte. It returns false for a NULL
 * `s`, or when the queue is full (ENOBUFS), has no destination (EINVAL), or
 * cannot grow (ENOMEM).
 */
bool session_write_bytes(struct session *s, const char *data, size_t len);

/*
 * Enqueues clipboard text for the pseudo-terminal. The terminal core filters
 * the text and adds bracketed paste escapes when the terminal has that mode.
 * Returns false when `s` is NULL, `len` is zero, `len` exceeds INPUT_TEXT_MAX,
 * or the write queue refuses the bytes.
 */
bool session_paste(struct session *s, const char *text, size_t len);

/* Returns the title of session `s`, or "shell" when it has none. The session
 * owns the string, which stays valid until the next title change or
 * session_fini(). */
const char *session_title(const struct session *s);

/*
 * Copies the working directory of session `s` into `out` as a NUL-terminated
 * path. It uses the directory that the child reported and falls back to the
 * directory of the foreground process.
 * Returns true when the path fits in `sz` and this process can enter it, or
 * false otherwise.
 */
bool session_cwd(const struct session *s, char *out, size_t sz);

/* Records the time of the last keystroke, which restarts the cursor blink
 * phase. */
void session_on_keystroke(struct session *s, uint64_t time_ms);

/* Returns the recorded keystroke time in milliseconds, or zero before the first
 * keystroke. */
uint64_t session_last_keystroke_ms(const struct session *s);

/*
 * Reaps the exited child of each live session and ends its output. The host
 * exit callback follows the last byte of that session's output. Call it on the
 * loop thread.
 */
void session_reap_exited(void);

#endif
