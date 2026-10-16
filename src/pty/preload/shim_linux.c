#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pty/cmdline.h"
#include "pty/preload/shim.h"

extern char **environ;

char fp_preload[4096];

__attribute__((constructor)) static void capture_preload(void)
{
    const char *preload = getenv("LD_PRELOAD");
    if (preload)
        snprintf(fp_preload, sizeof(fp_preload), "%s", preload);
}

static int stdio_uses_fast_pipe(void)
{
    return fp_is_fast_pipe(STDOUT_FILENO) || fp_is_fast_pipe(STDERR_FILENO);
}

#define FP_RESOLVE(type, var, name) \
    static type var; \
    if (!var) \
        var = (type)dlsym(RTLD_NEXT, name)

typedef int (*exec_fn)(const char *, char *const[], char *const[]);
typedef int (*execvp_fn)(const char *, char *const[]);
typedef int (*fexecve_fn)(int, char *const[], char *const[]);
typedef int (*spawn_fn)(pid_t *, const char *,
                        const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *, char *const[], char *const[]);

struct exec_ctx {
    int saved[2];
    char **env;
    char *env_entry;
};

static int exec_begin(struct exec_ctx *c, const char *cmd, char *const envp[],
                      char *const **env_out)
{
    *c = (struct exec_ctx){ .saved = { -1, -1 } };
    *env_out = envp;
    if (pty_is_tty_cmd(pty_cmd_basename(cmd))) {
        if (fp_stdio_to_tty(c->saved, 0) < 0)
            return -1;
        if (!fp_preload[0] || !fp_env_has_preload(envp))
            return 0;
        c->env = fp_env_without_fast_preload(envp, &c->env_entry);
        if (!c->env) {
            errno = ENOMEM;
            return -1;
        }
        *env_out = c->env;
        return 0;
    }
    if (fp_env_disables_preload(envp))
        return 0;
    if (!stdio_uses_fast_pipe() || !fp_preload[0])
        return 0;
    c->env = fp_env_with_preload(envp, &c->env_entry);
    if (!c->env) {
        errno = ENOMEM;
        return -1;
    }
    *env_out = c->env;
    return 0;
}

static void exec_end(struct exec_ctx *c)
{
    int error = errno;
    fp_stdio_restore(c->saved);
    free(c->env_entry);
    free(c->env);
    errno = error;
}

static int exec_hooked(exec_fn execute, const char *path, char *const argv[],
                       char *const envp[])
{
    struct exec_ctx c;
    char *const *env;
    if (exec_begin(&c, path, envp, &env) < 0) {
        exec_end(&c);
        return -1;
    }
    int result = execute(path, argv, env);
    exec_end(&c);
    return result;
}

static int execve_shim(const char *path, char *const argv[], char *const envp[])
{
    FP_RESOLVE(exec_fn, real_execve, "execve");
    if (!real_execve)
        return -1;
    return exec_hooked(real_execve, path, argv, envp);
}

static int execvpe_shim(const char *file, char *const argv[], char *const envp[])
{
    FP_RESOLVE(exec_fn, real_execvpe, "execvpe");
    if (!real_execvpe) {
        errno = ENOSYS;
        return -1;
    }
    return exec_hooked(real_execvpe, file, argv, envp);
}

static int execvp_shim(const char *file, char *const argv[])
{
    FP_RESOLVE(exec_fn, real_execvpe, "execvpe");
    if (real_execvpe)
        return exec_hooked(real_execvpe, file, argv, environ);
    FP_RESOLVE(execvp_fn, real_execvp, "execvp");
    if (!real_execvp)
        return -1;
    return real_execvp(file, argv);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return execve_shim(path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    return execve_shim(path, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    return execvpe_shim(file, argv, envp);
}

int execvp(const char *file, char *const argv[])
{
    return execvp_shim(file, argv);
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
    FP_RESOLVE(fexecve_fn, real_fexecve, "fexecve");
    if (!real_fexecve) {
        errno = ENOSYS;
        return -1;
    }

    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    char path[4096] = "";
    ssize_t len = readlink(link, path, sizeof(path) - 1);
    if (len > 0)
        path[len] = '\0';

    struct exec_ctx c;
    char *const *env;
    if (exec_begin(&c, path, envp, &env) < 0) {
        exec_end(&c);
        return -1;
    }
    int result = real_fexecve(fd, argv, env);
    exec_end(&c);
    return result;
}

static char **argv_from_va(const char *arg, va_list ap, char *const **envp_out)
{
    va_list count_ap;
    va_copy(count_ap, ap);
    size_t count = 0;
    for (const char *a = arg; a; a = va_arg(count_ap, char *))
        count++;
    if (envp_out)
        *envp_out = va_arg(count_ap, char *const *);
    va_end(count_ap);

    char **argv = malloc((count + 1) * sizeof(*argv));
    if (!argv)
        return NULL;
    if (arg) {
        argv[0] = (char *)arg;
        for (size_t i = 1; i < count; i++)
            argv[i] = va_arg(ap, char *);
    }
    argv[count] = NULL;
    return argv;
}

static int execl_common(exec_fn execute, const char *path, char **argv,
                        char *const envp[])
{
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    int result = execute(path, argv, envp);
    int error = errno;
    free(argv);
    errno = error;
    return result;
}

int execl(const char *path, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    char **argv = argv_from_va(arg, ap, NULL);
    va_end(ap);
    return execl_common(execve_shim, path, argv, environ);
}

int execle(const char *path, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    char *const *envp = NULL;
    char **argv = argv_from_va(arg, ap, &envp);
    va_end(ap);
    return execl_common(execve_shim, path, argv, envp);
}

static int execvp_exec_adapter(const char *file, char *const argv[],
                               char *const envp[])
{
    (void)envp;
    return execvp_shim(file, argv);
}

int execlp(const char *file, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    char **argv = argv_from_va(arg, ap, NULL);
    va_end(ap);
    return execl_common(execvp_exec_adapter, file, argv, environ);
}

static int spawn_hooked(spawn_fn execute, pid_t *pid, const char *path,
                        const posix_spawn_file_actions_t *actions,
                        const posix_spawnattr_t *attr, char *const argv[],
                        char *const envp[])
{
    struct exec_ctx c;
    char *const *env;
    if (exec_begin(&c, path, envp, &env) < 0) {
        exec_end(&c);
        return errno;
    }
    int result = execute(pid, path, actions, attr, argv, env);
    exec_end(&c);
    return result;
}

#define FP_WRAP_SPAWN(name)                                              \
    int name(pid_t *pid, const char *path,                               \
            const posix_spawn_file_actions_t *actions,                   \
            const posix_spawnattr_t *attr, char *const argv[],           \
            char *const envp[])                                          \
    {                                                                    \
        FP_RESOLVE(spawn_fn, real_##name, #name);                        \
        if (!real_##name)                                                \
            return ENOSYS;                                               \
        return spawn_hooked(real_##name, pid, path, actions, attr,       \
                            argv, envp);                                 \
    }

FP_WRAP_SPAWN(posix_spawn)
FP_WRAP_SPAWN(posix_spawnp)
