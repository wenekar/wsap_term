#define _GNU_SOURCE

#include "platform/platform.h"
#include "util/proc.h"
#include "util/str.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <gio/gio.h>

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

void platform_locale_default(char *out, size_t n)
{
    snprintf(out, n, "en_US.UTF-8");
}

void platform_close_fds_from(int first)
{
    close_range((unsigned)first, ~0u, 0);
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
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        g_clear_error(&error);
        return;
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry",
                          g_variant_new_string("wsap_term"));
    g_dbus_connection_call(bus, "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susss@as@a{sv}i)", "wsap_term", 0u, "",
            title ? title : "", body ? body : "",
            g_variant_new_strv(NULL, 0), g_variant_builder_end(&hints), -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_object_unref(bus);
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
