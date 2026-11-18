#define _GNU_SOURCE
#include "common/desktop.h"
#include "platform/freedesktop/dbus.h"
#include "common/private_desktop.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <spawn.h>
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

/* Find an executable in PATH, like execvp but before forking. */
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

/* The opener can outlive the call by seconds. Reap it on a detached thread, so
 * the caller never waits and no child becomes a zombie. */
static void *reap_child(void *ud)
{
    pid_t pid = (pid_t)(intptr_t)ud;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return NULL;
}

/* Start the program in a new session with the state a helper needs: default
 * signal dispositions, an empty signal mask, stdin on /dev/null, and no
 * descriptors above stderr. */
static bool spawn_detached(char *const argv[], const char *program, pid_t *pid_out)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    int error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                                 O_RDONLY, 0);
    if (!error) {
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 34)
        error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
        for (int fd = 3; !error && fd < max_fd; fd++)
            error = posix_spawn_file_actions_addclose(&actions, fd);
#endif
    }
    if (!error) {
        sigset_t defaults, none;
        sigfillset(&defaults);
        sigdelset(&defaults, SIGKILL);
        sigdelset(&defaults, SIGSTOP);
        sigemptyset(&none);
        error = posix_spawnattr_setsigdefault(&attr, &defaults);
        if (!error) error = posix_spawnattr_setsigmask(&attr, &none);
        short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(POSIX_SPAWN_SETSID)
        if (!error) error = posix_spawnattr_setflags(&attr, flags | POSIX_SPAWN_SETSID);
#else
        if (!error) error = posix_spawnattr_setpgroup(&attr, 0);
        if (!error) error = posix_spawnattr_setflags(&attr, flags | POSIX_SPAWN_SETPGROUP);
#endif
    }
    bool ok = false;
    if (!error) {
        error = posix_spawn(pid_out, program, &actions, &attr, argv, environ);
        ok = error == 0;
    }
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (!ok) errno = error ? error : EIO;
    return ok;
}

bool desktop_open_uri(const char *uri)
{
    char program[4096];
    if (!find_program("xdg-open", program, sizeof(program))) return false;
    char *argv[] = {"xdg-open", (char *)uri, NULL};
    pid_t pid;
    if (!spawn_detached(argv, program, &pid)) return false;
    pthread_t thread;
    int error = pthread_create(&thread, NULL, reap_child, (void *)(intptr_t)pid);
    if (error == 0) {
        pthread_detach(thread);
        return true;
    }
    /* No thread to reap it: kill the opener and collect it here. */
    kill(pid, SIGKILL);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    errno = error;
    return false;
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

/* An XDG folder, with its fallback under the home directory. */
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

size_t desktop_user_dir(enum yasl_user_dir which, char *buf, size_t cap)
{
    switch (which) {
    case YASL_DIR_PREFERENCE:
        return xdg_dir("XDG_CONFIG_HOME", ".config", buf, cap);
    case YASL_DIR_DATA:
        return xdg_dir("XDG_DATA_HOME", ".local/share", buf, cap);
    case YASL_DIR_CACHE:
        return xdg_dir("XDG_CACHE_HOME", ".cache", buf, cap);
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
