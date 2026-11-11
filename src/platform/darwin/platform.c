#define _GNU_SOURCE

#include "platform/platform.h"
#include "util/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <pthread/qos.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <locale.h>
#include <xlocale.h>

int platform_eventfd(int *wr_fd)
{
    int fds[2];
    if (proc_nonblocking_pipe(fds) < 0) {
        *wr_fd = -1;
        return -1;
    }
    *wr_fd = fds[1];
    return fds[0];
}

int platform_fastpipe_create(int fds[2], int buf_size)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        return -1;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    return 0;
}

int platform_exe_path(char *out, size_t n)
{
    char raw[4096];
    uint32_t sz = sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (!realpath(raw, resolved))
        return -1;
    if (strlen(resolved) >= n)
        return -1;
    strcpy(out, resolved);
    return 0;
}

int platform_resource_dir(char *out, size_t n)
{
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle)
        return -1;
    CFStringRef ident = CFBundleGetIdentifier(bundle);
    if (!ident)
        return -1;
    CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
    if (!url)
        return -1;
    char path[PATH_MAX];
    bool ok = CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof(path));
    CFRelease(url);
    if (!ok || strlen(path) >= n)
        return -1;
    strcpy(out, path);
    return 0;
}

static bool locale_valid(const char *loc)
{
    locale_t l = newlocale(LC_CTYPE_MASK, loc, NULL);
    if (!l)
        return false;
    freelocale(l);
    return true;
}

void platform_locale_default(char *out, size_t n)
{
    snprintf(out, n, "en_US.UTF-8");
    CFLocaleRef locale = CFLocaleCopyCurrent();
    if (locale) {
        CFStringRef ident = CFLocaleGetIdentifier(locale);
        if (ident) {
            char raw[64];
            if (CFStringGetCString(ident, raw, sizeof(raw), kCFStringEncodingUTF8) && raw[0] != '\0') {
                for (char *p = raw; *p; p++) {
                    if (*p == '-')
                        *p = '_';
                }
                char cand[128];
                if (strstr(raw, ".UTF-8") || strstr(raw, ".utf8"))
                    snprintf(cand, sizeof(cand), "%s", raw);
                else
                    snprintf(cand, sizeof(cand), "%s.UTF-8", raw);

                if (locale_valid(cand))
                    snprintf(out, n, "%s", cand);
            }
        }
        CFRelease(locale);
    }
}

char **platform_locales_list(size_t *count)
{
    *count = 0;
    DIR *dir = opendir("/usr/share/locale");
    if (!dir)
        return NULL;

    size_t cap = 128;
    char **list = malloc(cap * sizeof(*list));
    if (!list) {
        closedir(dir);
        return NULL;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        const char *dot = strstr(name, ".UTF-8");
        if (!dot || strcmp(dot, ".UTF-8") != 0)
            dot = strstr(name, ".utf8");
        if (dot && (strcmp(dot, ".UTF-8") == 0 || strcmp(dot, ".utf8") == 0)) {
            if (locale_valid(name)) {
                if (*count == cap) {
                    size_t new_cap = cap * 2;
                    char **new_list = realloc(list, new_cap * sizeof(*list));
                    if (!new_list)
                        break;
                    list = new_list;
                    cap = new_cap;
                }
                char *dup = strdup(name);
                if (dup)
                    list[(*count)++] = dup;
            }
        }
    }
    closedir(dir);
    return list;
}

void platform_close_fds_from(int first)
{
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < first)
        limit = 1024;
    for (int fd = first; fd < limit; fd++)
        close(fd);
}

void platform_thread_interactive(void)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

bool platform_prefers_high_contrast(void)
{
    return false;
}
