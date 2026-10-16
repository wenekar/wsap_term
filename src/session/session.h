#ifndef SESSION_SESSION_H
#define SESSION_SESSION_H

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

/* Whoever shows the session: a terminal tab. Every callback is optional
 * and runs on the loop thread. */
struct session_host {
    /* Output reached the terminal. */
    void (*output)(void *ud);
    void (*title)(void *ud);
    /* The program set the clipboard (OSC 52). */
    void (*clipboard)(void *ud, const char *text);
    /* The child exited and was reaped by session_reap_exited(). */
    void (*exited)(void *ud);
    void (*bell)(void *ud);
};

struct session_spec {
    /* NULL or empty runs the login shell. */
    const char *command;
    const char *cwd;
    /* In cells. */
    struct vector2i grid;
    /* History lines; 0 selects the terminal default. */
    int scrollback;
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_blink;
};

/* A terminal and the program that feeds it. The owner provides the
 * storage, aligned to 64 bytes. */
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
    /* Newline policy was read from termios for newline_fd during this
     * ring batch. */
    bool newline_synced;
    int newline_fd;
    /* The output bypassed tty output processing, so LF implies CR. */
    bool onlcr;
    uint64_t last_keystroke_ms;
    /* Reaped, and the host not told yet. */
    bool reaped;
    alignas(64) struct pty pty;
};

/* Starts the program. On failure s holds nothing to release. */
bool session_init(struct session *s, struct yasl_loop *loop,
                  const struct session_spec *spec);
void session_fini(struct session *s);
void session_set_host(struct session *s, const struct session_host *host, void *ud);

void session_resize(struct session *s, struct vector2i grid);
/* Send bytes in order without encoding them. True means accepted.
 * The queue can finish the write later. */
bool session_write_bytes(struct session *s, const char *data, size_t len);
/* Filter pasted text and use the program's bracketed-paste mode.
 * True means accepted. The queue can finish the write later. */
bool session_paste(struct session *s, const char *text, size_t len);
const char *session_title(const struct session *s);
/* The directory of the foreground program, else of the shell. */
bool session_cwd(const struct session *s, char *out, size_t sz);

/* Record a keystroke. It restarts the cursor blink. */
void session_on_keystroke(struct session *s, uint64_t time_ms);
uint64_t session_last_keystroke_ms(const struct session *s);

/* Reaps every exited child and tells its host. Call on SIGCHLD. */
void session_reap_exited(void);

#endif
