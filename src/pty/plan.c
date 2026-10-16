#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "pty/internal.h"
#include "pty/sync.h"
#include "platform/platform.h"
#include "util/proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WSAP_TERMINFO_NAME "wsap_term-256color"
#define WSAP_TERM_FALLBACK "xterm-256color"
#define DEFAULT_PATH "/bin:/usr/bin"

extern char **environ;

static void ti_strcat(char *out, size_t n, const char *s)
{
    size_t len = strlen(out);
    if (len + 1 < n)
        snprintf(out + len, n - len, "%s", s);
}

static void terminfo_dirs(char *out, size_t n)
{
    out[0] = '\0';
#ifdef WSAP_DEV_TERMINFO_DIR
    ti_strcat(out, n, WSAP_DEV_TERMINFO_DIR ":");
#endif
    const char *existing = getenv("TERMINFO_DIRS");
    if (existing && existing[0]) {
        ti_strcat(out, n, existing);
        ti_strcat(out, n, ":");
    }
    char bundle_dir[4096];
    if (platform_resource_dir(bundle_dir, sizeof(bundle_dir)) == 0) {
        ti_strcat(out, n, bundle_dir);
        ti_strcat(out, n, "/terminfo:");
    }
#ifdef WSAP_DATADIR
    ti_strcat(out, n, WSAP_DATADIR "/terminfo:");
#endif
    const char *home = proc_home_dir();
    if (home && home[0]) {
        ti_strcat(out, n, home);
        ti_strcat(out, n, "/.local/share/terminfo:");
    }
    ti_strcat(out, n, "/etc/terminfo:/lib/terminfo:/usr/share/terminfo"
                      ":/usr/local/share/terminfo:");
}

static bool ti_db_has(const char *dir, size_t len, const char *name)
{
    if (!len || len > 4096)
        return false;
    char path[4096 + 64];
    snprintf(path, sizeof(path), "%.*s/%02x/%s", (int)len, dir,
             (unsigned char)name[0], name);
    if (access(path, R_OK) == 0)
        return true;
    snprintf(path, sizeof(path), "%.*s/%c/%s", (int)len, dir, name[0], name);
    return access(path, R_OK) == 0;
}

static bool terminfo_resolves(const char *name, const char *dirs)
{
    const char *ti = getenv("TERMINFO");
    if (ti && ti[0] && ti_db_has(ti, strlen(ti), name))
        return true;
    const char *home = proc_home_dir();
    if (home && home[0]) {
        char user_db[4096];
        int w = snprintf(user_db, sizeof(user_db), "%s/.terminfo", home);
        if (w > 0 && (size_t)w < sizeof(user_db) &&
            ti_db_has(user_db, (size_t)w, name))
            return true;
    }
    for (const char *p = dirs; p && *p;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (ti_db_has(p, len, name))
            return true;
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
}

static void set_terminal_env(struct pty_env *env)
{
    char ti_dirs[16384];
    terminfo_dirs(ti_dirs, sizeof(ti_dirs));
    pty_env_set(env, "TERMINFO_DIRS", ti_dirs);
    pty_env_set(env, "TERM", terminfo_resolves(WSAP_TERMINFO_NAME, ti_dirs)
                                 ? WSAP_TERMINFO_NAME : WSAP_TERM_FALLBACK);
    pty_env_set(env, "COLORTERM", "truecolor");
    pty_env_set(env, "TERM_PROGRAM", "wsap_term");
    pty_env_unset(env, "TERM_PROGRAM_VERSION");

    if (pty_env_get(env, "LANG") || pty_env_get(env, "LC_ALL") ||
        pty_env_get(env, "LC_CTYPE"))
        return;
    char locale[128];
    platform_locale_default(locale, sizeof(locale));
    pty_env_set(env, "LANG", locale);
}

static void set_preload_env(struct pty_env *env)
{
    char exe_dir[4096] = { 0 };
    if (platform_exe_path(exe_dir, sizeof(exe_dir)) != 0)
        return;
    char *last_slash = strrchr(exe_dir, '/');
    if (!last_slash)
        return;
    *last_slash = '\0';

    char preload_path[5000];
    snprintf(preload_path, sizeof(preload_path), "%s/%s", exe_dir,
             PLATFORM_PRELOAD_LIB);
    if (access(preload_path, R_OK) != 0)
        return;

    const char *existing = pty_env_get(env, PLATFORM_PRELOAD_ENV);
    if (existing && existing[0]) {
        char combined[10240];
        snprintf(combined, sizeof(combined), "%s:%s", preload_path, existing);
        pty_env_set(env, PLATFORM_PRELOAD_ENV, combined);
    } else {
        pty_env_set(env, PLATFORM_PRELOAD_ENV, preload_path);
    }
}

static void set_transport_env(struct pty_env *env, int slave_fd, int pipe_wr,
                              const char *sync_name)
{
    pty_env_unset(env, "WSAP_PTY_PATH");
    pty_env_unset(env, PTY_SYNC_ENV);
    if (pipe_wr < 0) {
        pty_env_unset(env, "WSAP_FAST_PIPE");
        pty_env_unset(env, "WSAP_PIPE_ID");
        return;
    }

    if (sync_name)
        pty_env_set(env, PTY_SYNC_ENV, sync_name);
    char slave_path[256];
    if (ttyname_r(slave_fd, slave_path, sizeof(slave_path)) == 0)
        pty_env_set(env, "WSAP_PTY_PATH", slave_path);
    pty_env_unset(env, "WSAP_NO_PRELOAD");
#ifndef __APPLE__
    pty_env_set(env, "fish_use_posix_spawn", "0");
#endif
    pty_env_set(env, "WSAP_FAST_PIPE", "1");

    struct stat pipe_st;
    if (fstat(pipe_wr, &pipe_st) == 0) {
        char id_buf[64];
        snprintf(id_buf, sizeof(id_buf), "%llu:%llu",
                 (unsigned long long)pipe_st.st_dev,
                 (unsigned long long)pipe_st.st_ino);
        pty_env_set(env, "WSAP_PIPE_ID", id_buf);
    }
    set_preload_env(env);
}

static char **split_cmdline(const char *cmd, char **buf_out)
{
    size_t len = strlen(cmd);
    char *buf = malloc(len + 1);
    char **argv = malloc((len / 2 + 2) * sizeof(*argv));
    if (!buf || !argv) {
        free(buf);
        free(argv);
        return NULL;
    }
    size_t argc = 0;
    char *out = buf;
    for (const char *p = cmd;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[argc++] = out;
        for (char quote = 0; *p; p++) {
            char c = *p;
            if (quote == '\'') {
                if (c == '\'')
                    quote = 0;
                else
                    *out++ = c;
            } else if (quote == '"') {
                if (c == '"')
                    quote = 0;
                else if (c == '\\' && p[1] && strchr("\"\\$`", p[1]))
                    *out++ = *++p;
                else
                    *out++ = c;
            } else if (c == ' ' || c == '\t') {
                break;
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '\\' && p[1]) {
                *out++ = *++p;
            } else {
                *out++ = c;
            }
        }
        *out++ = '\0';
    }
    argv[argc] = NULL;
    *buf_out = buf;
    return argv;
}

static char *resolve_path(const char *file, const char *path_env)
{
    if (strchr(file, '/'))
        return strdup(file);
    const char *p = path_env ? path_env : DEFAULT_PATH;
    size_t flen = strlen(file);
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        char *cand = malloc(dlen + flen + 3);
        if (!cand)
            return NULL;
        if (dlen)
            memcpy(cand, p, dlen);
        else
            cand[dlen++] = '.';
        cand[dlen] = '/';
        memcpy(cand + dlen + 1, file, flen + 1);
        struct stat st;
        if (access(cand, X_OK) == 0 && stat(cand, &st) == 0 &&
            S_ISREG(st.st_mode))
            return cand;
        free(cand);
        if (!colon)
            return strdup(file);
        p = colon + 1;
    }
}

int pty_plan_init(struct pty_plan *plan, const struct pty_spec *spec)
{
    *plan = (struct pty_plan){0};
    const char *launch = spec->command && spec->command[0]
                         ? spec->command : proc_default_shell();
    plan->argv = split_cmdline(launch, &plan->argbuf);
    if (!plan->argv)
        return -1;
    if (!plan->argv[0]) {
        plan->argv[0] = (char *)"/bin/sh";
        plan->argv[1] = NULL;
    }
    if (spec->cwd && spec->cwd[0] && !(plan->cwd = strdup(spec->cwd)))
        return -1;
    char cur[4096];
    const char *home = proc_home_dir();
    if (getcwd(cur, sizeof(cur)) && !strcmp(cur, "/") && home && home[0] &&
        !(plan->cwd_fallback = strdup(home)))
        return -1;
    return 0;
}

int pty_plan_env(struct pty_plan *plan, const struct pty_spec *spec,
                 int slave_fd, int pipe_wr, const char *sync_name)
{
    struct pty_env *env = &plan->env;
    pty_env_init(env, environ);
    set_terminal_env(env);
    set_transport_env(env, slave_fd, pipe_wr, sync_name);
    for (size_t i = 0; spec->env && spec->env[i]; i++)
        pty_env_put(env, spec->env[i]);
    if (env->failed)
        return -1;

    plan->path = resolve_path(plan->argv[0], pty_env_get(env, "PATH"));
    size_t argc = 0;
    while (plan->argv[argc])
        argc++;
    plan->sh_argv = malloc((argc + 2) * sizeof(*plan->sh_argv));
    if (!plan->path || !plan->sh_argv)
        return -1;
    plan->sh_argv[0] = (char *)"sh";
    plan->sh_argv[1] = plan->path;
    for (size_t i = 1; i <= argc; i++)
        plan->sh_argv[i + 1] = plan->argv[i];
    return 0;
}

void pty_plan_free(struct pty_plan *plan)
{
    pty_env_free(&plan->env);
    free(plan->argv);
    free(plan->argbuf);
    free(plan->path);
    free(plan->sh_argv);
    free(plan->cwd);
    free(plan->cwd_fallback);
    *plan = (struct pty_plan){0};
}
