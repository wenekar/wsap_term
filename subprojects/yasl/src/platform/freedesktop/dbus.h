#ifndef YASL_PRIVATE_FREEDESKTOP_DBUS_H
#define YASL_PRIVATE_FREEDESKTOP_DBUS_H

#include <stdbool.h>
#include <stdint.h>

/* Session-bus helpers. The first call opens a private connection that later
 * calls reuse. Each returns false when YASL is built without libdbus-1 or no
 * session bus is reachable. Call from one thread at a time. */

/* Send org.freedesktop.Notifications.Notify without waiting for a reply. */
bool bus_notify(const char *app, const char *title, const char *body);
/* Read an integer setting through the XDG Settings portal, waiting briefly.
 * A string holding a decimal integer also counts, as KDE reports them. */
bool portal_read_int(const char *ns, const char *key, int64_t *out);
/* The KDE or GNOME double-click interval from the portal, or zero. */
int portal_double_click_ms(void);

#endif
