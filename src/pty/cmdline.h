#ifndef PTY_CMDLINE_H
#define PTY_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *const PTY_TTY_CMDS[] = {
    "sudo", "doas", "su", "pkexec", "bwrap", "agy", "codex",
};

static const char *const PTY_WRAPPER_CMDS[] = {
    "env", "nice", "command",
};

static inline const char *pty_cmd_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static inline bool pty_cmd_in(const char *basename, const char *const *list,
                              size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(basename, list[i]))
            return true;
    return false;
}

static inline bool pty_is_tty_cmd(const char *basename)
{
    return pty_cmd_in(basename, PTY_TTY_CMDS,
                      sizeof(PTY_TTY_CMDS) / sizeof(PTY_TTY_CMDS[0]));
}

static inline bool pty_is_wrapper_cmd(const char *basename)
{
    return pty_cmd_in(basename, PTY_WRAPPER_CMDS,
                      sizeof(PTY_WRAPPER_CMDS) / sizeof(PTY_WRAPPER_CMDS[0]));
}

static inline bool pty_argv_needs_tty(char *const argv[])
{
    for (size_t i = 0; argv[i]; i++) {
        const char *base = pty_cmd_basename(argv[i]);
        if (pty_is_tty_cmd(base))
            return true;
        if (i == 0 && !pty_is_wrapper_cmd(base))
            return false;
    }
    return false;
}

#endif
