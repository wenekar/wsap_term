#ifndef PTY_SHELL_H
#define PTY_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* The shell that a new session runs when the spec names no command. */
const char *pty_default_shell(void);

/* The working directory of pid, if this process can read it. */
bool pty_read_pid_cwd_buf(pid_t pid, char *out, size_t sz);

#endif
