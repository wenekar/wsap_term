/**
 * @file
 * Desktop services next to windowing: opening URIs, notifications, the bell,
 * appearance preferences, and process paths.
 *
 * Display and surface functions use the display's opening thread (the main
 * thread on Cocoa). Path and locale functions may be called from any thread.
 * On Linux, notifications and appearance use the D-Bus session bus when YASL
 * is built with libdbus-1; without it they report unsupported.
 *
 * Functions that fill a buffer return the full string length in bytes,
 * excluding the NUL, or zero on failure. A result less than cap means out
 * holds the NUL-terminated string. Otherwise out is unchanged; call again with
 * a buffer of at least the result plus one.
 */
#ifndef YASL_PRIVATE_DESKTOP_H
#define YASL_PRIVATE_DESKTOP_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Appearance preference flags returned by yasl_private_display_appearance(). */
enum yasl_private_appearance : uint32_t {
    YASL_PRIVATE_APPEARANCE_DARK          = 1 << 0, /**< The user prefers dark colors. */
    YASL_PRIVATE_APPEARANCE_LIGHT         = 1 << 1, /**< The user prefers light colors. */
    YASL_PRIVATE_APPEARANCE_HIGH_CONTRAST = 1 << 2, /**< The user asks for higher contrast. */
};

/**
 * Open a URI with the user's default handler.
 * @param d Display, or NULL.
 * @param uri NUL-terminated absolute URI with a scheme, such as https: or file:.
 * @return true if the request was handed to the system; false for NULL
 * arguments, a missing scheme, or a failed launch.
 * YASL checks only the URI syntax. The application decides which schemes and
 * hosts it trusts. Success does not confirm that a handler opened the URI.
 * Linux runs xdg-open from PATH in a new session, with default signal
 * dispositions, an empty signal mask, stdin on /dev/null, and no descriptors
 * above stderr. It forks a short-lived helper first, which raises SIGCHLD;
 * YASL reaps it unless the application's reaper does. Cocoa uses NSWorkspace.
 */
bool yasl_private_display_open_uri(struct yasl_private_display *d, const char *uri);
/**
 * Show a desktop notification.
 * @param d Display, or NULL.
 * @param title NUL-terminated UTF-8 summary.
 * @param body NUL-terminated UTF-8 body, or NULL for none.
 * @return true if the request was sent; false for NULL arguments or when no
 * notification service is available.
 * Linux sends org.freedesktop.Notifications.Notify with the display's app_id
 * as the application name and desktop-entry hint. Cocoa uses the user
 * notification center, which needs an app bundle with a bundle identifier,
 * and asks for permission on first use. Success does not confirm that the
 * notification was shown.
 */
bool yasl_private_display_notify(struct yasl_private_display *d, const char *title, const char *body);
/**
 * Read the user's appearance preferences.
 * @param d Display, or NULL.
 * @return A yasl_private_appearance bitmask, or zero for NULL, no preference, or no
 * way to read it.
 * Linux reads color-scheme and contrast from the XDG Settings portal, which
 * waits briefly for a reply, then falls back to the GTK_THEME variable. Cocoa
 * reads the effective appearance and the increase-contrast setting. The
 * result is not cached and no change notification is sent; query again when
 * the window is focused.
 */
uint32_t yasl_private_display_appearance(struct yasl_private_display *d);
/**
 * Ring the system bell for a surface.
 * @param s Surface, or NULL.
 * @return true if the request was sent; false for NULL or when unsupported.
 * The system decides whether the bell is audible, visual, or muted. Wayland
 * needs the xdg-system-bell protocol; X11 uses the XKB bell for the window
 * and Cocoa uses NSBeep.
 */
bool yasl_private_surface_bell(struct yasl_surface *s);
/**
 * Get the absolute path of the running executable, with symbolic links resolved.
 * @param out Output buffer, or NULL when cap is zero.
 * @param cap Size of out in bytes.
 * @return Path length, or zero on failure. See the buffer rule in this file.
 */
size_t yasl_private_executable_path(char *out, size_t cap);
/**
 * Get the resource directory of the app bundle that contains the executable.
 * @param out Output buffer, or NULL when cap is zero.
 * @param cap Size of out in bytes.
 * @return Path length, or zero when the executable is not in an app bundle.
 * Only Cocoa has app bundles; other platforms return zero.
 */
size_t yasl_private_resource_dir(char *out, size_t cap);
/**
 * Get the user's preferred locale as a POSIX locale name, such as "en_US.UTF-8".
 * @param out Output buffer, or NULL when cap is zero.
 * @param cap Size of out in bytes.
 * @return Name length, or zero on failure.
 * Linux returns the first nonempty LC_ALL, LC_CTYPE, or LANG, and "C.UTF-8"
 * when none is set. Cocoa builds the name from the current locale's language
 * and region with a ".UTF-8" codeset, because apps started from Finder have no
 * locale variables. Use the result to set LANG for child processes; YASL does
 * not change the process locale.
 */
size_t yasl_private_default_locale(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* YASL_PRIVATE_DESKTOP_H */
