#define _GNU_SOURCE
#include "common/backend.h"
#include "platform/freedesktop/dbus.h"
#include "common/private_desktop.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if YASL_HAS_DBUS
#include <dbus/dbus.h>
#endif

static void sleep_ms(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void test_buffers(void)
{
    char exe[PATH_MAX], real[PATH_MAX];
    size_t len = yasl_private_executable_path(NULL, 0);
    assert(len > 0);
    assert(yasl_private_executable_path(exe, len) == len);
    assert(yasl_private_executable_path(exe, sizeof(exe)) == len && exe[len] == '\0');
    assert(realpath("/proc/self/exe", real) && strcmp(exe, real) == 0);
    /* A short buffer stays untouched. */
    char small[4] = "abc";
    assert(yasl_private_executable_path(small, sizeof(small)) == len);
    assert(strcmp(small, "abc") == 0);
    assert(yasl_private_executable_path(NULL, 8) == 0);

    assert(yasl_private_resource_dir(exe, sizeof(exe)) == 0);

    char locale[64];
    setenv("LC_ALL", "", 1);
    setenv("LC_CTYPE", "de_DE.UTF-8", 1);
    setenv("LANG", "fr_FR.UTF-8", 1);
    assert(yasl_private_default_locale(locale, sizeof(locale)) == 11);
    assert(strcmp(locale, "de_DE.UTF-8") == 0);
    unsetenv("LC_CTYPE");
    assert(yasl_private_default_locale(locale, sizeof(locale)) == 11);
    assert(strcmp(locale, "fr_FR.UTF-8") == 0);
    unsetenv("LANG");
    unsetenv("LC_ALL");
    assert(yasl_private_default_locale(locale, sizeof(locale)) == 7);
    assert(strcmp(locale, "C.UTF-8") == 0);
}

/* A fake xdg-open records its argument, signal state, and descriptors. */
static void test_open_uri(const char *dir)
{
    char script[PATH_MAX], out[PATH_MAX];
    snprintf(script, sizeof(script), "%s/xdg-open", dir);
    snprintf(out, sizeof(out), "%s/opened", dir);
    FILE *f = fopen(script, "w");
    assert(f);
    fprintf(f, "#!/bin/sh\n"
               "{ printf '%%s\\n' \"$1\"; grep -E '^Sig(Blk|Ign)' /proc/self/status;\n"
               "  ls /proc/$$/fd; } > '%s.tmp'\n"
               "mv '%s.tmp' '%s'\n", out, out, out);
    fclose(f);
    assert(chmod(script, 0755) == 0);
    const char *old_path = getenv("PATH");
    char path[8192];
    snprintf(path, sizeof(path), "%s:%s", dir, old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", path, 1);

    struct yasl_private_display display = {0};
    assert(!yasl_private_display_open_uri(NULL, "https://example.com"));
    assert(!yasl_private_display_open_uri(&display, NULL));
    assert(!yasl_private_display_open_uri(&display, ""));
    assert(!yasl_private_display_open_uri(&display, "example.com"));
    assert(!yasl_private_display_open_uri(&display, "-x:option"));
    assert(!yasl_private_display_open_uri(&display, "1http://example.com"));
    assert(!yasl_private_display_open_uri(&display, "http://example.com/\n"));
    assert(access(out, F_OK) != 0);

    /* The opener must not inherit blocked or ignored signals or descriptors. */
    sigset_t block, old_mask;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, &old_mask);
    struct sigaction ignore = {.sa_handler = SIG_IGN}, old_pipe;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &old_pipe);
    int null = open("/dev/null", O_RDONLY);
    int leaked = fcntl(null, F_DUPFD, 200);
    assert(null >= 0 && leaked >= 200);
    close(null);

    const char *uri = "https://example.com/a b?c='d'&e=\"f\"";
    assert(yasl_private_display_open_uri(&display, uri));
    char *text = NULL;
    for (int i = 0; i < 500 && !(text = read_file(out)); i++) sleep_ms(10);
    assert(text);
    char *line = strtok(text, "\n");
    assert(line && strcmp(line, uri) == 0);
    int fds = 0;
    while ((line = strtok(NULL, "\n"))) {
        if (strncmp(line, "Sig", 3) == 0) {
            const char *mask = strchr(line, ':') + 1;
            while (*mask == '\t' || *mask == ' ') mask++;
            assert(strspn(mask, "0") == strlen(mask));
        } else {
            assert(atoi(line) != leaked);
            fds++;
        }
    }
    assert(fds >= 3);

    close(leaked);
    sigaction(SIGPIPE, &old_pipe, NULL);
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    unlink(out);
    unlink(script);

    /* Without xdg-open in PATH, nothing launches. */
    setenv("PATH", dir, 1);
    assert(!yasl_private_display_open_uri(&display, "https://example.com"));
    if (old_path) setenv("PATH", old_path, 1);
}

static void test_appearance_fallback(void)
{
    assert(yasl_private_display_double_click_ms(NULL) == YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT);
    assert(portal_double_click_ms() == 0);
    struct yasl_private_display display = {0};
    assert(yasl_private_display_appearance(NULL) == 0);
    unsetenv("GTK_THEME");
    assert(yasl_private_display_appearance(&display) == 0);
    setenv("GTK_THEME", "Adwaita:dark", 1);
    assert(yasl_private_display_appearance(&display) == YASL_PRIVATE_APPEARANCE_DARK);
    setenv("GTK_THEME", "HighContrast", 1);
    assert(yasl_private_display_appearance(&display) == YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST);
    setenv("GTK_THEME", "HighContrastInverse", 1);
    assert(yasl_private_display_appearance(&display) ==
           (YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST | YASL_PRIVATE_APPEARANCE_DARK));
    unsetenv("GTK_THEME");
    assert(!yasl_private_display_notify(NULL, "title", "body"));
    assert(!yasl_private_display_notify(&display, NULL, "body"));
    assert(!yasl_private_display_notify(&display, "title", "body"));
}

#if YASL_HAS_DBUS
static dbus_int32_t fake_slot = -1;

/* A fake notification server and Settings portal on a private bus. */
struct fake_bus {
    DBusConnection *conn;
    pthread_mutex_t lock;
    char app[64], title[64], body[64], entry[64];
    int notifications;
    bool read_one;
    bool kde;
    _Atomic bool stop;
};

static void reply_setting(DBusConnection *c, DBusMessage *msg, bool nested)
{
    const char *ns = NULL, *key = NULL;
    dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key,
                          DBUS_TYPE_INVALID);
    struct fake_bus *fb = dbus_connection_get_data(c, fake_slot);
    dbus_uint32_t value = 1;
    dbus_int32_t gnome_ms = 333;
    const char *kde_ms = "250";
    int type = DBUS_TYPE_UINT32;
    const void *data = &value;
    if (!ns || !key) {
        type = DBUS_TYPE_INVALID;
    } else if (strcmp(ns, "org.freedesktop.appearance") == 0 &&
               (strcmp(key, "color-scheme") == 0 || strcmp(key, "contrast") == 0)) {
    } else if (strcmp(ns, "org.gnome.desktop.peripherals.mouse") == 0 &&
               strcmp(key, "double-click") == 0) {
        type = DBUS_TYPE_INT32;
        data = &gnome_ms;
    } else if (fb->kde && strcmp(ns, "org.kde.kdeglobals.KDE") == 0 &&
               strcmp(key, "DoubleClickInterval") == 0) {
        type = DBUS_TYPE_STRING;
        data = &kde_ms;
    } else {
        type = DBUS_TYPE_INVALID;
    }
    if (type == DBUS_TYPE_INVALID) {
        DBusMessage *err = dbus_message_new_error(msg,
                "org.freedesktop.portal.Error.NotFound", "Requested setting not found");
        dbus_connection_send(c, err, NULL);
        dbus_message_unref(err);
        return;
    }
    DBusMessage *reply = dbus_message_new_method_return(msg);
    DBusMessageIter it, outer, inner;
    const char signature[2] = {(char)type, '\0'};
    dbus_message_iter_init_append(reply, &it);
    if (nested) {
        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "v", &outer);
        dbus_message_iter_open_container(&outer, DBUS_TYPE_VARIANT, signature, &inner);
        dbus_message_iter_append_basic(&inner, type, data);
        dbus_message_iter_close_container(&outer, &inner);
        dbus_message_iter_close_container(&it, &outer);
    } else {
        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, signature, &outer);
        dbus_message_iter_append_basic(&outer, type, data);
        dbus_message_iter_close_container(&it, &outer);
    }
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void record_notify(struct fake_bus *fb, DBusMessage *msg)
{
    DBusMessageIter it, sub, entry, value;
    const char *app, *icon, *title, *body, *hint;
    dbus_uint32_t replaces;
    dbus_int32_t expire;
    assert(dbus_message_has_signature(msg, "susssasa{sv}i"));
    dbus_message_iter_init(msg, &it);
    dbus_message_iter_get_basic(&it, &app);
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &replaces);
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &icon);
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &title);
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &body);
    dbus_message_iter_next(&it);
    dbus_message_iter_next(&it);
    dbus_message_iter_recurse(&it, &sub);
    dbus_message_iter_recurse(&sub, &entry);
    dbus_message_iter_get_basic(&entry, &hint);
    assert(strcmp(hint, "desktop-entry") == 0);
    dbus_message_iter_next(&entry);
    dbus_message_iter_recurse(&entry, &value);
    const char *desktop_entry;
    dbus_message_iter_get_basic(&value, &desktop_entry);
    dbus_message_iter_next(&it);
    dbus_message_iter_get_basic(&it, &expire);
    assert(replaces == 0 && expire == -1 && icon[0] == '\0');

    pthread_mutex_lock(&fb->lock);
    snprintf(fb->app, sizeof(fb->app), "%s", app);
    snprintf(fb->title, sizeof(fb->title), "%s", title);
    snprintf(fb->body, sizeof(fb->body), "%s", body);
    snprintf(fb->entry, sizeof(fb->entry), "%s", desktop_entry);
    fb->notifications++;
    pthread_mutex_unlock(&fb->lock);
}

static void *serve(void *ud)
{
    struct fake_bus *fb = ud;
    while (!fb->stop && dbus_connection_read_write(fb->conn, 50)) {
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(fb->conn))) {
            if (dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify")) {
                record_notify(fb, msg);
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.portal.Settings", "ReadOne")) {
                if (fb->read_one) {
                    reply_setting(fb->conn, msg, false);
                } else {
                    DBusMessage *err = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_METHOD, "old");
                    dbus_connection_send(fb->conn, err, NULL);
                    dbus_message_unref(err);
                }
            } else if (dbus_message_is_method_call(msg, "org.freedesktop.portal.Settings", "Read")) {
                reply_setting(fb->conn, msg, true);
            }
            dbus_message_unref(msg);
        }
    }
    return NULL;
}

static void test_bus(const char *address)
{
    DBusError err;
    dbus_error_init(&err);
    struct fake_bus fb = {.read_one = true};
    pthread_mutex_init(&fb.lock, NULL);
    fb.conn = dbus_connection_open_private(address, &err);
    assert(fb.conn && dbus_bus_register(fb.conn, &err));
    assert(dbus_connection_allocate_data_slot(&fake_slot));
    assert(dbus_connection_set_data(fb.conn, fake_slot, &fb, NULL));
    assert(dbus_bus_request_name(fb.conn, "org.freedesktop.Notifications", 0, &err) ==
           DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    assert(dbus_bus_request_name(fb.conn, "org.freedesktop.portal.Desktop", 0, &err) ==
           DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve, &fb) == 0);

    struct yasl_private_display display = {.app_id = "org.example.Test"};
    assert(yasl_private_display_notify(&display, "Build done", NULL));
    for (int i = 0; i < 500; i++) {
        pthread_mutex_lock(&fb.lock);
        int n = fb.notifications;
        pthread_mutex_unlock(&fb.lock);
        if (n) break;
        sleep_ms(10);
    }
    pthread_mutex_lock(&fb.lock);
    assert(fb.notifications == 1);
    assert(strcmp(fb.app, "org.example.Test") == 0);
    assert(strcmp(fb.entry, "org.example.Test") == 0);
    assert(strcmp(fb.title, "Build done") == 0 && fb.body[0] == '\0');
    pthread_mutex_unlock(&fb.lock);

    /* Portal settings win over GTK_THEME, through ReadOne or the older Read. */
    setenv("GTK_THEME", "Adwaita", 1);
    uint32_t want = YASL_PRIVATE_APPEARANCE_DARK | YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST;
    assert(yasl_private_display_appearance(&display) == want);
    fb.read_one = false;
    assert(yasl_private_display_appearance(&display) == want);
    unsetenv("GTK_THEME");

    /* A KDE value, sent as a string, wins over the GNOME key. */
    assert(portal_double_click_ms() == 333);
    fb.kde = true;
    assert(portal_double_click_ms() == 250);
    fb.read_one = true;
    assert(portal_double_click_ms() == 250);

    fb.stop = true;
    pthread_join(thread, NULL);
    dbus_connection_close(fb.conn);
    dbus_connection_unref(fb.conn);
    dbus_connection_free_data_slot(&fake_slot);
    pthread_mutex_destroy(&fb.lock);
}
#endif

int main(void)
{
    char dir[] = "/tmp/yasl-desktop-XXXXXX";
    assert(mkdtemp(dir));
    test_buffers();
    test_open_uri(dir);

    /* Hide the session bus so that desktop services fall back. */
    char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    address = address ? strdup(address) : NULL;
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("XDG_RUNTIME_DIR", dir, 1);
    test_appearance_fallback();
#if YASL_HAS_DBUS
    if (address) {
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
        test_bus(address);
    } else {
        printf("Skipping D-Bus checks: no session bus.\n");
    }
#endif
    free(address);
    rmdir(dir);
    printf("Desktop tests passed.\n");
    return 0;
}
