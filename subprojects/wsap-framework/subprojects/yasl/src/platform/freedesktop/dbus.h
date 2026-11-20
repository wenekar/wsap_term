#ifndef YASL_PRIVATE_FREEDESKTOP_DBUS_H
#define YASL_PRIVATE_FREEDESKTOP_DBUS_H

#include <stdbool.h>
#include <stdint.h>

/* Session-bus helpers. The first call that needs the bus opens a private
 * connection that later calls reuse; a dropped connection is reopened on the
 * next call. A mutex serializes the shared connection, so every helper is safe
 * to call from any thread. Without libdbus-1 or a reachable session bus,
 * bus_notify() and portal_read_int() return false and portal_double_click_ms()
 * returns zero. */

/* Sends org.freedesktop.Notifications.Notify and does not wait for a reply.
 * Returns false when the bus is unavailable or the message cannot be built or
 * sent. */
bool bus_notify(const char *app, const char *title, const char *body);
/* Reads an integer setting through the XDG Settings portal, waiting up to a
 * second for each reply. KDE reports some settings as a string holding a
 * decimal integer, which this also accepts. Writes *out on success and leaves
 * it unchanged on failure. */
bool portal_read_int(const char *ns, const char *key, int64_t *out);
/* Returns the KDE or GNOME double-click interval in milliseconds, or zero when
 * the portal reports no usable value. */
int portal_double_click_ms(void);
/* Closes the cached session bus and releases libdbus global resources. Call it
 * after the other helpers finish with the bus. */
void yasl_private_dbus_shutdown(void);

#endif
