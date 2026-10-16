#define _GNU_SOURCE
#include "platform/freedesktop/dbus.h"

#if YASL_HAS_DBUS

#include <dbus/dbus.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The portal may need activation on first use; bound how long a query blocks. */
#define PORTAL_TIMEOUT_MS 1000

static DBusConnection *bus;

/* The session bus address, without libdbus's X11 autolaunch fallback. */
static char *session_address(void)
{
    const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env) return strdup(env);
    const char *run = getenv("XDG_RUNTIME_DIR");
    if (!run || !*run) return NULL;
    char path[4096];
    struct stat st;
    if (snprintf(path, sizeof(path), "%s/bus", run) >= (int)sizeof(path) ||
        stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
        return NULL;
    char *escaped = dbus_address_escape_value(path);
    if (!escaped) return NULL;
    char *address = NULL;
    if (asprintf(&address, "unix:path=%s", escaped) < 0) address = NULL;
    dbus_free(escaped);
    return address;
}

static DBusConnection *session_bus(void)
{
    if (bus && dbus_connection_get_is_connected(bus)) return bus;
    if (bus) {
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        bus = NULL;
    }
    char *address = session_address();
    if (!address) return NULL;
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_connection_open_private(address, &err);
    free(address);
    if (!c) {
        dbus_error_free(&err);
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    if (!dbus_bus_register(c, &err)) {
        dbus_error_free(&err);
        dbus_connection_close(c);
        dbus_connection_unref(c);
        return NULL;
    }
    bus = c;
    return bus;
}

bool bus_notify(const char *app, const char *title, const char *body)
{
    DBusConnection *c = session_bus();
    if (!c) return false;
    DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.Notifications",
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
    if (!msg) return false;

    const char *icon = "";
    dbus_uint32_t replaces = 0;
    dbus_int32_t expire = -1;
    DBusMessageIter it, sub, entry, value;
    dbus_message_iter_init_append(msg, &it);
    bool ok = dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app) &&
        dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces) &&
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon) &&
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title) &&
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &body);
    /* No actions. */
    ok = ok && dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &sub) &&
        dbus_message_iter_close_container(&it, &sub);
    /* Hints: desktop-entry names the application's .desktop file. */
    const char *hint = "desktop-entry";
    ok = ok && dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &sub) &&
        dbus_message_iter_open_container(&sub, DBUS_TYPE_DICT_ENTRY, NULL, &entry) &&
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &hint) &&
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &value) &&
        dbus_message_iter_append_basic(&value, DBUS_TYPE_STRING, &app) &&
        dbus_message_iter_close_container(&entry, &value) &&
        dbus_message_iter_close_container(&sub, &entry) &&
        dbus_message_iter_close_container(&it, &sub);
    ok = ok && dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &expire);

    if (ok) {
        dbus_message_set_no_reply(msg, TRUE);
        ok = dbus_connection_send(c, msg, NULL);
        if (ok) dbus_connection_flush(c);
    }
    dbus_message_unref(msg);
    return ok;
}

/* Read an integer from it, unwrapping variants: Settings.Read nests one more. */
static bool iter_int(DBusMessageIter *it, int64_t *out)
{
    DBusMessageIter inner[4];
    int depth = 0;
    while (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_VARIANT && depth < 4) {
        dbus_message_iter_recurse(it, &inner[depth]);
        it = &inner[depth++];
    }
    DBusBasicValue v;
    switch (dbus_message_iter_get_arg_type(it)) {
    case DBUS_TYPE_STRING: {
        dbus_message_iter_get_basic(it, &v);
        char *end;
        errno = 0;
        long long n = strtoll(v.str, &end, 10);
        if (errno || end == v.str || *end) return false;
        *out = n;
        return true;
    }
    case DBUS_TYPE_BYTE: dbus_message_iter_get_basic(it, &v); *out = v.byt; return true;
    case DBUS_TYPE_BOOLEAN: dbus_message_iter_get_basic(it, &v); *out = v.bool_val; return true;
    case DBUS_TYPE_INT16: dbus_message_iter_get_basic(it, &v); *out = v.i16; return true;
    case DBUS_TYPE_UINT16: dbus_message_iter_get_basic(it, &v); *out = v.u16; return true;
    case DBUS_TYPE_INT32: dbus_message_iter_get_basic(it, &v); *out = v.i32; return true;
    case DBUS_TYPE_UINT32: dbus_message_iter_get_basic(it, &v); *out = v.u32; return true;
    case DBUS_TYPE_INT64: dbus_message_iter_get_basic(it, &v); *out = v.i64; return true;
    case DBUS_TYPE_UINT64:
        dbus_message_iter_get_basic(it, &v);
        if (v.u64 > INT64_MAX) return false;
        *out = (int64_t)v.u64;
        return true;
    default: return false;
    }
}

bool portal_read_int(const char *ns, const char *key, int64_t *out)
{
    DBusConnection *c = session_bus();
    if (!c) return false;
    /* ReadOne is portal version 2; older portals only have Read. */
    static const char *const methods[] = {"ReadOne", "Read"};
    for (int i = 0; i < 2; i++) {
        DBusMessage *msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings", methods[i]);
        if (!msg) return false;
        if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key,
                                      DBUS_TYPE_INVALID)) {
            dbus_message_unref(msg);
            return false;
        }
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, msg,
                PORTAL_TIMEOUT_MS, &err);
        dbus_message_unref(msg);
        if (!reply) {
            bool retry = dbus_error_has_name(&err, DBUS_ERROR_UNKNOWN_METHOD);
            dbus_error_free(&err);
            if (retry) continue;
            return false;
        }
        DBusMessageIter it;
        bool ok = dbus_message_iter_init(reply, &it) && iter_int(&it, out);
        dbus_message_unref(reply);
        return ok;
    }
    return false;
}

#else

bool bus_notify(const char *app, const char *title, const char *body)
{
    (void)app;
    (void)title;
    (void)body;
    return false;
}

bool portal_read_int(const char *ns, const char *key, int64_t *out)
{
    (void)ns;
    (void)key;
    (void)out;
    return false;
}

#endif

int portal_double_click_ms(void)
{
    /* kdeglobals holds only values the user changed, so a KDE answer is
     * deliberate; the GNOME key can be a default from another portal backend. */
    int64_t ms;
    if (portal_read_int("org.kde.kdeglobals.KDE", "DoubleClickInterval", &ms) ||
        portal_read_int("org.gnome.desktop.peripherals.mouse", "double-click", &ms))
        return ms > 0 && ms <= INT32_MAX ? (int)ms : 0;
    return 0;
}
