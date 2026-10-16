#define _GNU_SOURCE
#include "common/desktop.h"
#include "platform/freedesktop/dbus.h"
#include "common/private_desktop.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

bool desktop_open_uri(const char *uri)
{
    char program[4096];
    if (!find_program("xdg-open", program, sizeof(program))) return false;
    char *argv[] = {"xdg-open", (char *)uri, NULL};

    /* Prepare everything before fork: the children of a threaded process may
     * only make async-signal-safe calls. */
    struct sigaction dfl = {.sa_handler = SIG_DFL};
    sigemptyset(&dfl.sa_mask);
    sigset_t none;
    sigemptyset(&none);
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;

    /* The middle child exits at once, so the opener is reparented away from
     * the application and never needs reaping here. */
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        pid_t opener = fork();
        if (opener != 0) _exit(opener < 0 ? 1 : 0);
        for (int sig = 1; sig < NSIG; sig++)
            if (sig != SIGKILL && sig != SIGSTOP) sigaction(sig, &dfl, NULL);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int null = open("/dev/null", O_RDONLY);
        if (null > 0) {
            dup2(null, STDIN_FILENO);
            close(null);
        }
        /* Keep application descriptors, such as pty masters, out of the opener. */
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            for (int fd = 3; fd < max_fd; fd++) close(fd);
        execve(program, argv, environ);
        _exit(127);
    }

    int status;
    pid_t done;
    do {
        done = waitpid(pid, &status, 0);
    } while (done < 0 && errno == EINTR);
    /* An application reaper can collect the middle child first. */
    if (done < 0) return errno == ECHILD;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
