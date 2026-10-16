#ifndef PTY_INTERNAL_H
#define PTY_INTERNAL_H

#include "pty/pty.h"

struct pty_env {
    char **vars;
    size_t len;
    size_t cap;
    bool failed;
};

void pty_env_init(struct pty_env *e, char *const *base);
const char *pty_env_get(const struct pty_env *e, const char *name);
void pty_env_set(struct pty_env *e, const char *name, const char *value);
void pty_env_put(struct pty_env *e, const char *entry);
void pty_env_unset(struct pty_env *e, const char *name);
void pty_env_free(struct pty_env *e);

struct pty_plan {
    char **argv;
    char *argbuf;
    char *path;
    char **sh_argv;
    struct pty_env env;
    char *cwd;
    char *cwd_fallback;
};

int pty_plan_init(struct pty_plan *plan, const struct pty_spec *spec);
int pty_plan_env(struct pty_plan *plan, const struct pty_spec *spec,
                 int slave_fd, int pipe_wr, const char *sync_name);
void pty_plan_free(struct pty_plan *plan);

int pty_pipe_open(int fds[2]);

int pty_sync_listen(char *name, size_t n);

_Noreturn void pty_exec_child(int master_fd, int slave_fd,
                              const int pipe_fds[2],
                              const struct pty_plan *plan);

#endif
