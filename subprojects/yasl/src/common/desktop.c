#include "common/private_desktop.h"
#include "common/backend.h"
#include "common/desktop.h"

#include <string.h>

/* Large enough for any path the platform APIs return. */
#define DESKTOP_STR_MAX 4096

/* An RFC 3986 scheme, then ':', and no control characters anywhere. */
static bool uri_valid(const char *uri)
{
    unsigned char c = (unsigned char)uri[0];
    if ((unsigned)((c | 32) - 'a') >= 26)
        return false;
    const char *p = uri + 1;
    for (; *p != ':'; p++) {
        c = (unsigned char)*p;
        bool alpha = (unsigned)((c | 32) - 'a') < 26;
        bool digit = (unsigned)(c - '0') < 10;
        if (!alpha && !digit && c != '+' && c != '-' && c != '.')
            return false;
    }
    for (; *p; p++) {
        c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

/* Copy len bytes and a NUL when they fit, and report the full length. */
static size_t copy_out(char *out, size_t cap, const char *s, size_t len)
{
    if (len && len < cap) {
        memcpy(out, s, len);
        out[len] = '\0';
    }
    return len;
}

bool yasl_private_display_open_uri(struct yasl_private_display *d, const char *uri)
{
    if (!d || !uri || !uri_valid(uri))
        return false;
    return desktop_open_uri(uri);
}

bool yasl_private_display_notify(struct yasl_private_display *d, const char *title,
                                 const char *body)
{
    if (!d || !title)
        return false;
    return desktop_notify(d->app_id, title, body ? body : "");
}

uint32_t yasl_private_display_appearance(struct yasl_private_display *d)
{
    return d ? desktop_appearance() : 0;
}

bool yasl_private_surface_bell(struct yasl_surface *s)
{
    if (!s || !s->display->ops->surface_bell)
        return false;
    return s->display->ops->surface_bell(s);
}

size_t yasl_private_executable_path(char *out, size_t cap)
{
    if (!out && cap)
        return 0;
    char buf[DESKTOP_STR_MAX];
    return copy_out(out, cap, buf, desktop_executable_path(buf, sizeof(buf)));
}

size_t yasl_private_resource_dir(char *out, size_t cap)
{
    if (!out && cap)
        return 0;
    char buf[DESKTOP_STR_MAX];
    return copy_out(out, cap, buf, desktop_resource_dir(buf, sizeof(buf)));
}

size_t yasl_private_default_locale(char *out, size_t cap)
{
    if (!out && cap)
        return 0;
    char buf[DESKTOP_STR_MAX];
    return copy_out(out, cap, buf, desktop_default_locale(buf, sizeof(buf)));
}

#include "public_internal.h"
#include <errno.h>
int yasl_open_uri(const char *uri)
{
    if (!uri || !uri_valid(uri))
        return yasl_public_error(EINVAL);
    errno = 0;
    return yasl_public_status(desktop_open_uri(uri) || yasl_public_fail(errno));
}
int yasl_notify(const char *title, const char *body)
{
    if (!yasl_public_owner())
        return -errno;
    if (!title)
        return yasl_public_error(EINVAL);
    errno = 0;
    return yasl_public_status(yasl_private_display_notify(yasl_public_display(), title, body) ||
                              yasl_public_fail(errno));
}
unsigned yasl_get_appearance(void)
{
    return yasl_public_owner() ? yasl_private_display_appearance(yasl_public_display()) : 0;
}
int yasl_get_double_click_ms(void)
{
    return yasl_public_display() && yasl_public_owner()
               ? yasl_private_display_double_click_ms(yasl_public_display())
               : YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT;
}
int yasl_surface_bell(yasl_surface *s)
{
    if (!yasl_public_connected(s))
        return -errno;
    if (!s->display->ops->surface_bell)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(yasl_private_surface_bell(s) || yasl_public_fail(errno));
}
size_t yasl_get_system_string(enum yasl_system_string which, char *out, size_t cap)
{
    if ((!out && cap) || (unsigned)which > YASL_LOCALE)
        return yasl_public_fail(EINVAL), 0;
    char buf[DESKTOP_STR_MAX];
    errno = 0;
    size_t n = which == YASL_EXECUTABLE_PATH ? desktop_executable_path(buf, sizeof(buf))
               : which == YASL_RESOURCE_DIR  ? desktop_resource_dir(buf, sizeof(buf))
                                             : desktop_default_locale(buf, sizeof(buf));
    if (!n)
        return yasl_public_fail(errno ? errno : ENOENT), 0;
    /* A POSIX locale name without its encoding or modifier, the same on every platform. */
    if (which == YASL_LOCALE)
        n = strcspn(buf, ".@");
    return copy_out(out, cap, buf, n);
}
