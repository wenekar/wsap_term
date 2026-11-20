/* Window controls, monitor queries, and the standard paths of the user. */
#define _GNU_SOURCE
#include "public_internal.h"
#include "common/desktop.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Large enough for any path the platform APIs return. */
#define CONTROLS_STR_MAX 4096

static size_t copy_out(char *out, size_t cap, const char *s, size_t len)
{
    if (len && len < cap) {
        memcpy(out, s, len);
        out[len] = '\0';
    }
    return len;
}

/* Monitors */

int yasl_get_monitor_count(void)
{
    struct yasl_private_display *d = yasl_public_display();
    if (!d || !d->ops->display_monitors)
        return 0;
    int count = d->ops->display_monitors(d, NULL, 0);
    return count > 0 ? count : 0;
}

/* One query fills a bounded table, because a display has few monitors. */
#define MONITOR_MAX 16
int yasl_get_monitor(int index, struct yasl_monitor_info *out)
{
    struct yasl_private_display *d = yasl_public_display();
    if (!d || !d->ops->display_monitors)
        return yasl_public_error(ENOTSUP);
    if (!out || index < 0 || index >= MONITOR_MAX)
        return yasl_public_error(EINVAL);
    struct yasl_monitor_info all[MONITOR_MAX];
    int count = d->ops->display_monitors(d, all, MONITOR_MAX);
    if (index >= count)
        return yasl_public_error(EINVAL);
    *out = all[index];
    return 0;
}

int yasl_get_primary_monitor(struct yasl_monitor_info *out)
{
    if (!out)
        return yasl_public_error(EINVAL);
    int count = yasl_get_monitor_count();
    struct yasl_private_display *d = yasl_public_display();
    if (!d || !d->ops->display_monitors)
        return yasl_public_error(ENOTSUP);
    for (int i = 0; i < count && i < MONITOR_MAX; i++) {
        struct yasl_monitor_info info;
        if (yasl_get_monitor(i, &info) == 0 && info.primary) {
            *out = info;
            return 0;
        }
    }
    /* A platform that lists no flag still has one monitor that owns the origin. */
    if (count == 1 && yasl_get_monitor(0, out) == 0)
        return 0;
    return yasl_public_error(ENOENT);
}

/* Window controls */

int yasl_surface_set_position(yasl_surface *s, int x, int y)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (!s->display->ops->surface_set_position)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_position(
                                  s, (struct yasl_private_vector2i){x, y}) ||
                              yasl_public_fail(errno));
}

int yasl_surface_get_position(const yasl_surface *s, int *x, int *y)
{
    if (!yasl_public_surface((yasl_surface *)s, false))
        return -errno;
    if (!x || !y)
        return yasl_public_error(EINVAL);
    if (!s->display->ops->surface_get_position)
        return yasl_public_error(ENOTSUP);
    struct yasl_private_vector2i pos;
    if (!s->display->ops->surface_get_position(s, &pos))
        return yasl_public_error(errno ? errno : ENOTSUP);
    *x = pos.x;
    *y = pos.y;
    return 0;
}

int yasl_surface_set_size_limits(yasl_surface *s, int min_width, int min_height,
                                 int max_width, int max_height)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (min_width < 0 || min_height < 0 || max_width < 0 || max_height < 0)
        return yasl_public_error(EINVAL);
    if ((max_width && min_width > max_width) || (max_height && min_height > max_height))
        return yasl_public_error(EINVAL);
    if (!s->display->ops->surface_set_size_limits)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_size_limits(
                                  s, (struct yasl_private_vector2i){min_width, min_height},
                                  (struct yasl_private_vector2i){max_width, max_height}) ||
                              yasl_public_fail(errno));
}

int yasl_surface_set_aspect_ratio(yasl_surface *s, int numerator, int denominator)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (numerator < 0 || denominator < 0)
        return yasl_public_error(EINVAL);
    if (!numerator != !denominator)
        return yasl_public_error(EINVAL);
    if (!s->display->ops->surface_set_aspect)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_aspect(
                                  s, (struct yasl_private_vector2i){numerator, denominator}) ||
                              yasl_public_fail(errno));
}

int yasl_surface_client_to_screen(const yasl_surface *s, double x, double y,
                                  double *screen_x, double *screen_y)
{
    if (!yasl_public_surface((yasl_surface *)s, false))
        return -errno;
    if (!screen_x || !screen_y)
        return yasl_public_error(EINVAL);
    int left, top;
    int result = yasl_surface_get_position(s, &left, &top);
    if (result < 0)
        return result;
    *screen_x = (double)left + x;
    *screen_y = (double)top + y;
    return 0;
}

int yasl_surface_set_opacity(yasl_surface *s, double opacity)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (!(opacity >= 0.0 && opacity <= 1.0))
        return yasl_public_error(EINVAL);
    if (!s->display->ops->surface_set_opacity)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_opacity(s, opacity) ||
                              yasl_public_fail(errno));
}

int yasl_surface_set_visible(yasl_surface *s, bool visible)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (!s->display->ops->surface_set_visible)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_visible(s, visible) ||
                              yasl_public_fail(errno));
}

int yasl_surface_set_always_on_top(yasl_surface *s, bool enabled)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (!s->display->ops->surface_set_always_on_top)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_always_on_top(s, enabled) ||
                              yasl_public_fail(errno));
}

int yasl_surface_set_skip_taskbar(yasl_surface *s, bool enabled)
{
    if (!yasl_public_surface(s, true))
        return -errno;
    if (!s->display->ops->surface_set_skip_taskbar)
        return yasl_public_error(ENOTSUP);
    errno = 0;
    return yasl_public_status(s->display->ops->surface_set_skip_taskbar(s, enabled) ||
                              yasl_public_fail(errno));
}

/* Files */

size_t yasl_get_user_dir(enum yasl_user_dir which, char *out, size_t cap)
{
    if ((!out && cap) || (unsigned)which > YASL_DIR_DESKTOP)
        return yasl_public_fail(EINVAL), 0;
    char buf[CONTROLS_STR_MAX];
    struct yasl_private_display *d = yasl_public_display();
    errno = 0;
    size_t len = desktop_user_dir(which, d ? d->app_id : NULL, buf, sizeof(buf));
    if (!len)
        return yasl_public_fail(errno ? errno : ENOENT), 0;
    return copy_out(out, cap, buf, len);
}

/* Modified time in nanoseconds since the epoch. */
static int64_t modified_ns(const struct stat *st)
{
    int64_t ns = (int64_t)st->st_mtime * 1000000000;
#if defined(__APPLE__)
    ns += st->st_mtimespec.tv_nsec;
#elif defined(__linux__) || defined(_POSIX_C_SOURCE)
    ns += st->st_mtim.tv_nsec;
#endif
    return ns;
}

static void fill_info(struct yasl_file_info *out, const struct stat *st, bool is_link)
{
    out->size = (uint64_t)st->st_size;
    out->modified_ns = modified_ns(st);
    out->is_symlink = is_link;
    out->is_dir = !is_link && S_ISDIR(st->st_mode);
    out->is_file = !is_link && S_ISREG(st->st_mode);
}

int yasl_file_stat(const char *path, struct yasl_file_info *out)
{
    if (!path || !out)
        return yasl_public_error(EINVAL);
    struct stat st;
    if (stat(path, &st) != 0)
        return yasl_public_error(errno);
    fill_info(out, &st, false);
    struct stat link;
    if (lstat(path, &link) == 0 && S_ISLNK(link.st_mode)) {
        /* Keep the link itself as the reported kind. */
        fill_info(out, &link, true);
        out->is_dir = false;
        out->is_file = false;
    }
    return 0;
}

struct yasl_dir {
    DIR *handle;
    char path[CONTROLS_STR_MAX];
};

yasl_dir *yasl_dir_open(const char *path)
{
    if (!path)
        return yasl_public_fail(EINVAL), NULL;
    DIR *handle = opendir(path);
    if (!handle)
        return NULL;
    size_t len = strlen(path);
    if (len >= sizeof(((yasl_dir *)0)->path)) {
        closedir(handle);
        return yasl_public_fail(ENAMETOOLONG), NULL;
    }
    yasl_dir *dir = calloc(1, sizeof(*dir));
    if (!dir) {
        closedir(handle);
        return yasl_public_fail(ENOMEM), NULL;
    }
    dir->handle = handle;
    memcpy(dir->path, path, len + 1);
    return dir;
}

int yasl_dir_next(yasl_dir *dir, struct yasl_dir_entry *out)
{
    if (!dir || !out)
        return yasl_public_error(EINVAL);
    for (;;) {
        /* readdir returns NULL for the end and for an error alike. */
        errno = 0;
        struct dirent *entry = readdir(dir->handle);
        if (!entry)
            return errno ? yasl_public_error(errno) : 0;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        size_t len = strlen(entry->d_name);
        if (len >= sizeof(out->name))
            continue;
        char full[CONTROLS_STR_MAX];
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir->path, entry->d_name) >= sizeof(full))
            continue;
        if (yasl_file_stat(full, &out->info) != 0)
            continue;
        memcpy(out->name, entry->d_name, len + 1);
        return 1;
    }
}

void yasl_dir_close(yasl_dir *dir)
{
    if (!dir)
        return;
    closedir(dir->handle);
    free(dir);
}

int yasl_load_asset(const char *name, void **data, size_t *size)
{
    if (!name || !data || !size)
        return yasl_public_error(EINVAL);
    *data = NULL;
    *size = 0;
    char roots[3][CONTROLS_STR_MAX];
    size_t root_count = 0;
    char resource[CONTROLS_STR_MAX];
    if (desktop_resource_dir(resource, sizeof(resource)))
        memcpy(roots[root_count++], resource, strlen(resource) + 1);
    char executable[CONTROLS_STR_MAX];
    if (desktop_executable_path(executable, sizeof(executable))) {
        char *slash = strrchr(executable, '/');
        if (slash && slash != executable) {
            *slash = '\0';
            memcpy(roots[root_count++], executable, strlen(executable) + 1);
        }
    }
    memcpy(roots[root_count++], ".", 2);
    int saved = ENOENT;
    for (size_t i = 0; i < root_count; i++) {
        char path[CONTROLS_STR_MAX];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", roots[i], name) >= sizeof(path)) {
            saved = ENAMETOOLONG;
            continue;
        }
        FILE *file = fopen(path, "rb");
        if (!file) {
            if (errno != ENOENT) saved = errno;
            continue;
        }
        size_t cap = 4096, used = 0;
        char *buffer = malloc(cap);
        bool ok = buffer != NULL;
        while (ok) {
            if (used == cap) {
                if (cap > SIZE_MAX / 2) { ok = false; saved = EFBIG; break; }
                char *grown = realloc(buffer, cap * 2);
                if (!grown) { ok = false; saved = ENOMEM; break; }
                buffer = grown;
                cap *= 2;
            }
            size_t got = fread(buffer + used, 1, cap - used, file);
            used += got;
            if (got == 0) {
                if (ferror(file)) { ok = false; saved = EIO; }
                break;
            }
        }
        fclose(file);
        if (!ok) {
            free(buffer);
            continue;
        }
        char *final = realloc(buffer, used + 1);
        if (!final) final = buffer;
        final[used] = '\0';
        *data = final;
        *size = used;
        return 0;
    }
    return yasl_public_error(saved);
}
