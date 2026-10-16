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
