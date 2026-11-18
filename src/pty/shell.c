#define _GNU_SOURCE

#include "pty/shell.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

const char *pty_default_shell(void)
{
    const char *shell = getenv("SHELL");
    if (shell && shell[0])
        return shell;
    const struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && pw->pw_shell[0])
        return pw->pw_shell;
    return "/bin/sh";
}

bool pty_read_pid_cwd_buf(pid_t pid, char *out, size_t sz)
{
    if (pid <= 0 || !out || sz == 0)
        return false;
#ifdef __APPLE__
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) > 0 &&
        vpi.pvi_cdir.vip_path[0] &&
        access(vpi.pvi_cdir.vip_path, X_OK) == 0) {
        size_t len = strlen(vpi.pvi_cdir.vip_path);
        if (len >= sz)
            return false;
        memcpy(out, vpi.pvi_cdir.vip_path, len + 1);
        return true;
    }
#else
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/cwd", (int)pid);
    ssize_t len = readlink(proc_path, out, sz);
    if (len > 0 && (size_t)len < sz) {
        out[len] = '\0';
        if (access(out, X_OK) == 0)
            return true;
    }
#endif
    return false;
}
