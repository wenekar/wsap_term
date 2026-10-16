#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "theme/internal.h"
#include "util/name.h"

struct look {
    struct theme_catalog catalog;
    struct wsap_vtcore_theme base;
    struct appearance *committed;
    struct appearance *preview;
    void (*changed)(void *ud);
    void *ud;
    int previewed;
};

static const struct appearance *active(const struct look *look)
{
    return look->preview ? look->preview : look->committed;
}

static void publish(struct look *look)
{
    if (look->changed)
        look->changed(look->ud);
}

static bool is_css_path(const char *ref)
{
    size_t n = strlen(ref);
    return strchr(ref, '/') || (n > 4 && !strcasecmp(ref + n - 4, ".css"));
}

static struct appearance *build_startup(struct look *look, const char *ref)
{
    if (!ref || !ref[0])
        return NULL;
    if (is_css_path(ref))
        return appearance_load(NULL, ref, NULL, &look->base);
    struct theme_entry *e = theme_catalog_find(&look->catalog, ref);
    if (!e) {
        fprintf(stderr, "[%s] unknown theme '%s'\n", app_name(), ref);
        return NULL;
    }
    return appearance_ref(theme_entry_appearance(e, &look->base));
}

struct look *look_create(const struct wsap_vtcore_theme *base, const char *theme)
{
    struct look *look = calloc(1, sizeof(*look));
    if (!look)
        return NULL;
    look->base = *base;
    look->committed = build_startup(look, theme);
    if (!look->committed)
        look->committed = appearance_load(NULL, NULL, NULL, &look->base);
    if (!look->committed) {
        look_destroy(look);
        return NULL;
    }
    publish(look);
    return look;
}

void look_destroy(struct look *look)
{
    if (!look)
        return;
    appearance_unref(look->committed);
    appearance_unref(look->preview);
    theme_catalog_destroy(&look->catalog);
    free(look);
}

void look_set_listener(struct look *look, void (*changed)(void *ud), void *ud)
{
    look->changed = changed;
    look->ud = ud;
}

const struct css_sheet *look_sheet(const struct look *look)
{
    return &active(look)->chrome;
}

const struct wsap_vtcore_theme *look_palette(const struct look *look)
{
    return &active(look)->palette;
}

int look_theme_count(struct look *look)
{
    if (!look->catalog.scanned)
        theme_catalog_scan(&look->catalog);
    return look->catalog.count;
}

const struct look_theme *look_theme_at(struct look *look, int idx)
{
    if (idx < 0 || idx >= look_theme_count(look))
        return NULL;
    return &look->catalog.items[idx].info;
}

int look_rescan(struct look *look)
{
    look_cancel(look);
    theme_catalog_scan(&look->catalog);
    return look->catalog.count;
}

bool look_preview(struct look *look, int idx)
{
    if (idx < 0 || idx >= look_theme_count(look))
        return false;
    if (look->previewed == idx + 1)
        return true;
    struct appearance *ap =
        theme_entry_appearance(&look->catalog.items[idx], &look->base);
    if (!ap)
        return false;
    appearance_unref(look->preview);
    look->preview = appearance_ref(ap);
    look->previewed = idx + 1;
    publish(look);
    return true;
}

int look_previewed(const struct look *look)
{
    return look->previewed - 1;
}

void look_commit(struct look *look)
{
    if (!look->preview)
        return;
    appearance_unref(look->committed);
    look->committed = look->preview;
    look->preview = NULL;
    look->previewed = 0;
}

void look_cancel(struct look *look)
{
    if (!look->preview)
        return;
    appearance_unref(look->preview);
    look->preview = NULL;
    look->previewed = 0;
    publish(look);
}

const char *look_committed_name(const struct look *look)
{
    return look->committed->name ? look->committed->name : "";
}

bool look_is_committed(struct look *look, int idx)
{
    const char *name = look_committed_name(look);
    const struct look_theme *theme = look_theme_at(look, idx);
    return theme && name[0] && !strcmp(name, theme->name);
}

bool look_apply(struct look *look, const struct wsap_vtcore_theme *base, const char *theme)
{
    struct appearance *next;
    if (!theme || !theme[0]) {
        next = appearance_load(NULL, NULL, NULL, base);
    } else if (is_css_path(theme)) {
        next = appearance_load(NULL, theme, NULL, base);
    } else {
        struct theme_entry *entry = theme_catalog_find(&look->catalog, theme);
        if (!entry) return false;
        next = appearance_load(entry->info.name, entry->info.css_path,
                                entry->info.conf_path, base);
    }
    if (!next) return false;
    appearance_unref(look->preview);
    appearance_unref(look->committed);
    look->preview = NULL;
    look->previewed = 0;
    look->base = *base;
    look->committed = next;
    for (int i = 0; i < look->catalog.count; i++) {
        appearance_unref(look->catalog.items[i].built);
        look->catalog.items[i].built = NULL;
    }
    publish(look);
    return true;
}
