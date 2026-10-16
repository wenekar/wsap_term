#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "theme/internal.h"
#include "platform/platform.h"
#include "util/config.h"
#include "util/grow.h"
#include "util/path.h"

static const char CSS_SUFFIX[] = ".css";
static const char CONF_SUFFIX[] = "-term.conf";

static void entry_clear(struct theme_entry *e)
{
    free(e->info.name);
    free(e->info.label);
    free(e->info.css_path);
    free(e->info.conf_path);
    appearance_unref(e->built);
    *e = (struct theme_entry){0};
}

static struct theme_entry *entry_get(struct theme_catalog *c, const char *name,
                                     size_t len)
{
    char c0 = len ? name[0] : '\0';
    for (int i = 0; i < c->count; i++) {
        struct theme_entry *e = &c->items[i];
        if (e->name_len == len && e->name_c0 == c0 &&
            !memcmp(e->info.name, name, len))
            return e;
    }
    if (!UTIL_GROW(c->items, c->cap, c->count + 1))
        return NULL;
    struct theme_entry *e = &c->items[c->count];
    *e = (struct theme_entry){
        .info.name = strndup(name, len),
        .name_len = (uint32_t)len,
        .name_c0 = c0,
    };
    if (!e->info.name)
        return NULL;
    c->count++;
    return e;
}

static bool has_suffix(const char *s, size_t len, const char *suffix, size_t slen)
{
    return len > slen && !memcmp(s + len - slen, suffix, slen);
}

/* Directories scan in precedence order, so the first path found wins. */
static void scan_dir(struct theme_catalog *c, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *f = ent->d_name;
        size_t len = strlen(f);
        bool css = has_suffix(f, len, CSS_SUFFIX, sizeof(CSS_SUFFIX) - 1);
        bool conf = has_suffix(f, len, CONF_SUFFIX, sizeof(CONF_SUFFIX) - 1);
        if (f[0] == '.' || (!css && !conf))
            continue;
        size_t blen = len - (css ? sizeof(CSS_SUFFIX) : sizeof(CONF_SUFFIX)) + 1;
        struct theme_entry *e = entry_get(c, f, blen);
        char **slot = e ? (css ? &e->info.css_path : &e->info.conf_path) : NULL;
        if (slot && !*slot && asprintf(slot, "%s/%s", dir, f) < 0)
            *slot = NULL;
    }
    closedir(d);
}

static void label_line(void *ctx, const char *path, int line,
                       const char *k, const char *v)
{
    (void)path;
    (void)line;
    char **label = ctx;
    if (v && v[0] && !strcmp(k, "label") && !*label)
        *label = strdup(v);
}

static char *title_case(const char *name)
{
    char *out = malloc(strlen(name) + 1);
    if (!out)
        return NULL;
    size_t n = 0;
    bool word = true;
    for (const char *p = name; *p; p++) {
        if (*p == '-' || *p == '_') {
            if (n && out[n - 1] != ' ')
                out[n++] = ' ';
            word = true;
        } else {
            out[n++] = word ? (char)toupper((unsigned char)*p) : *p;
            word = false;
        }
    }
    out[n] = '\0';
    return out;
}

static int entry_compare(const void *a, const void *b)
{
    const struct theme_entry *ea = a, *eb = b;
    return strcasecmp(ea->info.label, eb->info.label);
}

static bool same_path(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static void adopt_cache(struct theme_entry *e, struct theme_catalog *old)
{
    for (int i = 0; i < old->count; i++) {
        struct theme_entry *o = &old->items[i];
        if (o->name_len == e->name_len &&
            o->name_c0 == e->name_c0 &&
            !strcmp(o->info.name, e->info.name) &&
            same_path(o->info.css_path, e->info.css_path) &&
            same_path(o->info.conf_path, e->info.conf_path)) {
            e->built = o->built;
            e->css_stamp = o->css_stamp;
            e->conf_stamp = o->conf_stamp;
            o->built = NULL;
            return;
        }
    }
}

void theme_catalog_scan(struct theme_catalog *c)
{
    struct theme_catalog old = *c;
    *c = (struct theme_catalog){ .scanned = true };

    char user_dir[PATH_MAX];
    if (xdg_config_path(user_dir, sizeof(user_dir), "wsap_term/themes"))
        scan_dir(c, user_dir);
    /* The source tree's themes match this binary better than an older
     * installed copy. */
#ifdef WSAP_DEV_THEMES_DIR
    scan_dir(c, WSAP_DEV_THEMES_DIR);
#endif
    char bundle_dir[PATH_MAX];
    if (platform_resource_dir(bundle_dir, sizeof(bundle_dir) - sizeof("/themes")) == 0) {
        strcat(bundle_dir, "/themes");
        scan_dir(c, bundle_dir);
    }
#ifdef WSAP_DATADIR
    scan_dir(c, WSAP_DATADIR "/wsap_term/themes");
#endif

    int n = 0;
    for (int i = 0; i < c->count; i++) {
        struct theme_entry *e = &c->items[i];
        if (e->info.conf_path)
            config_parse_file(e->info.conf_path, label_line, &e->info.label);
        if (!e->info.label)
            e->info.label = title_case(e->info.name);
        if (!e->info.label) {
            entry_clear(e);
            continue;
        }
        e->label_len = (uint32_t)strlen(e->info.label);
        e->label_c0_lower = (char)tolower((unsigned char)e->info.label[0]);
        adopt_cache(e, &old);
        c->items[n++] = *e;
    }
    c->count = n;
    if (n > 1)
        qsort(c->items, (size_t)n, sizeof(*c->items), entry_compare);
    theme_catalog_destroy(&old);
}

void theme_catalog_destroy(struct theme_catalog *c)
{
    for (int i = 0; i < c->count; i++)
        entry_clear(&c->items[i]);
    free(c->items);
    *c = (struct theme_catalog){0};
}

struct theme_entry *theme_catalog_find(struct theme_catalog *c, const char *ref)
{
    if (!ref)
        return NULL;
    if (!c->scanned)
        theme_catalog_scan(c);
    size_t ref_len = strlen(ref);
    char ref_c0 = ref[0];
    char ref_c0_lower = (char)tolower((unsigned char)ref_c0);
    for (int i = 0; i < c->count; i++) {
        struct theme_entry *e = &c->items[i];
        if (e->name_len == ref_len && e->name_c0 == ref_c0 &&
            !strcmp(e->info.name, ref))
            return e;
        if (e->label_len == ref_len && e->label_c0_lower == ref_c0_lower &&
            !strcasecmp(e->info.label, ref))
            return e;
    }
    return NULL;
}

static struct file_stamp file_stamp(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0)
        return (struct file_stamp){0};
#ifdef __APPLE__
    struct timespec mtime = st.st_mtimespec;
#else
    struct timespec mtime = st.st_mtim;
#endif
    return (struct file_stamp){
        .mtime = mtime, .size = st.st_size, .ino = st.st_ino, .dev = st.st_dev,
    };
}

static bool same_stamp(struct file_stamp a, struct file_stamp b)
{
    return a.mtime.tv_sec == b.mtime.tv_sec &&
           a.mtime.tv_nsec == b.mtime.tv_nsec && a.size == b.size &&
           a.ino == b.ino && a.dev == b.dev;
}

struct appearance *theme_entry_appearance(struct theme_entry *e,
                                          const struct wsap_vtcore_theme *base)
{
    struct file_stamp css = file_stamp(e->info.css_path);
    struct file_stamp conf = file_stamp(e->info.conf_path);
    if (e->built && same_stamp(css, e->css_stamp) && same_stamp(conf, e->conf_stamp))
        return e->built;
    struct appearance *ap = appearance_load(e->info.name, e->info.css_path,
                                            e->info.conf_path, base);
    if (!ap)
        return e->built;
    appearance_unref(e->built);
    e->built = ap;
    e->css_stamp = css;
    e->conf_stamp = conf;
    return ap;
}
