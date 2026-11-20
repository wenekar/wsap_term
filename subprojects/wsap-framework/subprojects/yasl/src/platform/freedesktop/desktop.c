#define _GNU_SOURCE
#include "common/desktop.h"
#include "platform/freedesktop/dbus.h"
#include "common/private_desktop.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static size_t copy_str(char *buf, size_t cap, const char *s)
{
    size_t len = strlen(s);
    if (!len || len >= cap) return 0;
    memcpy(buf, s, len + 1);
    return len;
}

/* Find an executable in PATH as execvp would, but before forking, so a missing
 * program fails the call. */
static bool find_program(const char *name, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    size_t name_len = strlen(name);
    while (*path) {
        const char *end = strchr(path, ':');
        size_t dir_len = end ? (size_t)(end - path) : strlen(path);
        if (dir_len && dir_len + 1 + name_len < cap) {
            memcpy(out, path, dir_len);
            out[dir_len] = '/';
            memcpy(out + dir_len + 1, name, name_len + 1);
            if (access(out, X_OK) == 0) return true;
        }
        if (!end) break;
        path = end + 1;
    }
    return false;
}

/* Start the program in a new session with the state a helper needs: default
 * signal dispositions, an empty signal mask, stdin on /dev/null, and no
 * descriptors above stderr. A double fork lets init collect the program, so the
 * library needs no reap thread and no SIGCHLD handler. Only async-signal-safe
 * calls run in the child. */
static bool spawn_detached(char *const argv[], const char *program)
{
    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max < 0 || open_max > 65536) open_max = 65536;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (setsid() < 0) _exit(127);
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
#ifdef NSIG
        for (int sig = 1; sig < NSIG; sig++)
            if (sig != SIGKILL && sig != SIGSTOP) signal(sig, SIG_DFL);
#endif
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null != STDIN_FILENO) close(null);
        }
        for (int fd = 3; fd < open_max; fd++) close(fd);
        execv(program, argv);
        _exit(127);
    }
    /* Collect the intermediate child, which exits at once. An app that
     * reaps its own children can win the race; the program is still started. */
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return true;
}

bool desktop_open_uri(const char *uri)
{
    char program[4096];
    if (!find_program("xdg-open", program, sizeof(program))) return false;
    char *argv[] = {"xdg-open", (char *)uri, NULL};
    return spawn_detached(argv, program);
}

bool desktop_notify(const char *app_id, const char *title, const char *body)
{
    return bus_notify(app_id ? app_id : "yasl", title, body);
}

uint32_t desktop_appearance(void)
{
    uint32_t flags = 0;
    bool portal = false;
    int64_t value;
    if (portal_read_int("org.freedesktop.appearance", "color-scheme", &value)) {
        portal = true;
        if (value == 1) flags |= YASL_PRIVATE_APPEARANCE_DARK;
        else if (value == 2) flags |= YASL_PRIVATE_APPEARANCE_LIGHT;
    }
    if (portal_read_int("org.freedesktop.appearance", "contrast", &value)) {
        portal = true;
        if (value == 1) flags |= YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST;
    }
    if (portal) return flags;

    /* GTK_THEME is "Name" or "Name:variant", as in "Adwaita:dark". */
    const char *theme = getenv("GTK_THEME");
    if (!theme) return 0;
    if (strstr(theme, "HighContrast")) flags |= YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST;
    if (strstr(theme, ":dark") || strstr(theme, "HighContrastInverse"))
        flags |= YASL_PRIVATE_APPEARANCE_DARK;
    return flags;
}

size_t desktop_executable_path(char *buf, size_t cap)
{
    /* readlink truncates silently, so a full buffer may hold a cut path. */
    ssize_t len = readlink("/proc/self/exe", buf, cap);
    if (len <= 0 || (size_t)len >= cap) return 0;
    buf[len] = '\0';
    return (size_t)len;
}

size_t desktop_resource_dir(char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    errno = ENOTSUP; /* Linux applications have no bundle. */
    return 0;
}

size_t desktop_default_locale(char *buf, size_t cap)
{
    static const char *const vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) return copy_str(buf, cap, v);
    }
    return copy_str(buf, cap, "C.UTF-8");
}

/* Resolve an XDG directory variable that holds an absolute path, or join the
 * fallback onto the home directory. */
static size_t xdg_dir(const char *variable, const char *fallback, char *buf, size_t cap)
{
    const char *value = getenv(variable);
    if (value && value[0] == '/') return copy_str(buf, cap, value);
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') { errno = ENOENT; return 0; }
    char path[4096];
    int len = snprintf(path, sizeof(path), "%s/%s", home, fallback);
    if (len <= 0 || (size_t)len >= sizeof(path)) { errno = ENAMETOOLONG; return 0; }
    return copy_str(buf, cap, path);
}

/* App folders nest under the app ID. An ID that starts with a dot or contains a
 * separator could name a hidden folder or escape the parent, so app_dir() then
 * uses the base folder alone. */
static bool app_id_valid(const char *app_id)
{
    if (!app_id || !*app_id || app_id[0] == '.') return false;
    return !strchr(app_id, '/');
}

static size_t app_dir(const char *variable, const char *fallback, const char *app_id,
                      char *buf, size_t cap)
{
    char base[4096];
    if (!xdg_dir(variable, fallback, base, sizeof(base))) return 0;
    if (!app_id_valid(app_id)) return copy_str(buf, cap, base);
    char path[4096];
    int len = snprintf(path, sizeof(path), "%s/%s", base, app_id);
    if (len <= 0 || (size_t)len >= sizeof(path)) { errno = ENAMETOOLONG; return 0; }
    return copy_str(buf, cap, path);
}

size_t desktop_user_dir(enum yasl_user_dir which, const char *app_id, char *buf, size_t cap)
{
    switch (which) {
    case YASL_DIR_PREFERENCE:
        return app_dir("XDG_CONFIG_HOME", ".config", app_id, buf, cap);
    case YASL_DIR_DATA:
        return app_dir("XDG_DATA_HOME", ".local/share", app_id, buf, cap);
    case YASL_DIR_CACHE:
        return app_dir("XDG_CACHE_HOME", ".cache", app_id, buf, cap);
    case YASL_DIR_HOME: {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') { errno = ENOENT; return 0; }
        return copy_str(buf, cap, home);
    }
    case YASL_DIR_DOCUMENTS:
        return xdg_dir("XDG_DOCUMENTS_DIR", "Documents", buf, cap);
    case YASL_DIR_DOWNLOADS:
        return xdg_dir("XDG_DOWNLOAD_DIR", "Downloads", buf, cap);
    case YASL_DIR_DESKTOP:
        return xdg_dir("XDG_DESKTOP_DIR", "Desktop", buf, cap);
    }
    errno = EINVAL;
    return 0;
}
