#ifndef PTY_INTERNAL_H
#define PTY_INTERNAL_H

#include "pty/pty.h"

/* A child environment built in the parent. A zero value is an empty
 * environment. Allocation failure sets `failed`; later calls do nothing. */
struct pty_env {
    char **vars;
    size_t len;
    size_t cap;
    bool failed;
};

void pty_env_init(struct pty_env *e, char *const *base);
const char *pty_env_get(const struct pty_env *e, const char *name);
void pty_env_set(struct pty_env *e, const char *name, const char *value);
/* Set from a NAME=value entry. An entry without '=' is ignored. */
void pty_env_put(struct pty_env *e, const char *entry);
void pty_env_unset(struct pty_env *e, const char *name);
void pty_env_free(struct pty_env *e);

/* Everything the child needs, prepared before fork(). After fork() in a
 * threaded process the child may only make async-signal-safe calls, so
 * it must not allocate, format strings, or read the environment. */
struct pty_plan {
    char **argv;
    char *argbuf;
    char *path;
    char **sh_argv;
    struct pty_env env;
    char *cwd;
    char *cwd_fallback;
};

/* Split the command and pick the directory. The caller then opens the
 * transports, because their choice depends on argv. */
int pty_plan_init(struct pty_plan *plan, const struct pty_spec *spec);
/* Build the environment and resolve the executable. `pipe_wr` is -1
 * without a fast pipe, and `sync_name` is NULL without a barrier. */
int pty_plan_env(struct pty_plan *plan, const struct pty_spec *spec,
                 int slave_fd, int pipe_wr, const char *sync_name);
void pty_plan_free(struct pty_plan *plan);

/* Open the fast pipe if it is enabled. Leaves both fds -1 when it is
 * disabled. Returns -1 only on failure. */
int pty_pipe_open(int fds[2]);

/* Open the barrier's listening socket and write its name to `name`.
 * Returns the fd, or -1 on failure. */
int pty_sync_listen(char *name, size_t n);

/* Post-fork child setup: fd routing, session, cwd, exec.
 * Never returns; exits via exec or _exit. */
_Noreturn void pty_exec_child(int master_fd, int slave_fd,
                              const int pipe_fds[2],
                              const struct pty_plan *plan);

#endif
