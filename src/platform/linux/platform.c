#define _GNU_SOURCE

#include "platform/platform.h"
#include "util/proc.h"
#include "util/str.h"
#include "yasl/yasl.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <locale.h>

int platform_eventfd(int *wr_fd)
{
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        *wr_fd = -1;
        return -1;
    }
    *wr_fd = fd;
    return fd;
}

int platform_fastpipe_create(int fds[2], int buf_size)
{
    if (pipe2(fds, O_CLOEXEC) < 0) {
        perror("pipe2");
        return -1;
    }
    fcntl(fds[0], F_SETPIPE_SZ, buf_size);
    return 0;
}

int platform_exe_path(char *out, size_t n)
{
    ssize_t len = readlink("/proc/self/exe", out, n);
    if (len <= 0 || (size_t)len >= n)
        return -1;
    out[len] = '\0';
    return 0;
}

int platform_resource_dir(char *out, size_t n)
{
    (void)out;
    (void)n;
    return -1;
}

bool platform_locale_valid(const char *loc)
{
    if (!loc || !loc[0])
        return false;
    locale_t l = newlocale(LC_CTYPE_MASK, loc, (locale_t)0);
    if (!l)
        return false;
    freelocale(l);
    return true;
}

void platform_locale_default(char *out, size_t n)
{
    snprintf(out, n, "en_US.UTF-8");
}

char **platform_locales_list(size_t *count)
{
    *count = 0;
    static const char *const COMMON[] = {
        "C.UTF-8", "en_US.UTF-8", "en_GB.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8",
        "es_ES.UTF-8", "it_IT.UTF-8", "pt_BR.UTF-8", "pt_PT.UTF-8", "ru_RU.UTF-8",
        "tr_TR.UTF-8", "zh_CN.UTF-8", "ja_JP.UTF-8", "ko_KR.UTF-8", "nl_NL.UTF-8",
        "pl_PL.UTF-8", "sv_SE.UTF-8", "uk_UA.UTF-8",
    };
    size_t total = sizeof(COMMON) / sizeof(COMMON[0]);
    char **out = calloc(total + 1, sizeof(*out));
    if (!out)
        return NULL;
    for (size_t i = 0; i < total; i++) {
        out[*count] = strdup(COMMON[i]);
        if (out[*count])
            (*count)++;
    }
    return out;
}

void platform_close_fds_from(int first)
{
    close_range((unsigned)first, ~0u, 0);
}

/*
 * The kernel starts a process with room for 64 descriptors. It makes the
 * table larger when a descriptor number does not fit. In a process with
 * threads, it then waits for an RCU grace period, which takes 4 ms to 9 ms.
 * A window reaches 64 descriptors in the first resize, when the old and the
 * new swapchain exist together, so that frame was late.
 *
 * A duplicate at number 1023 makes the table 1024 entries large, which is
 * 8 KiB of kernel memory. The table does not become smaller when the
 * duplicate closes.
 */
void platform_reserve_fds(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    int top = fcntl(fd, F_DUPFD_CLOEXEC, 1023);
    if (top >= 0)
        close(top);
    close(fd);
}

void platform_thread_interactive(void)
{
}

void platform_open_url(const char *url)
{
    if (!str_url_scheme_allowed(url))
        return;
    proc_spawn_detached((char *const []){ "xdg-open", (char *)url, NULL });
}

void platform_notify(const char *title, const char *body)
{
    (void)yasl_notify(title ? title : "", body ? body : "");
}

void platform_bell(void)
{
}

bool platform_prefers_high_contrast(void)
{
    const char *gtk_theme = getenv("GTK_THEME");
    return (gtk_theme && strstr(gtk_theme, "HighContrast")) ||
           getenv("ACCESSIBILITY_HIGH_CONTRAST");
}
