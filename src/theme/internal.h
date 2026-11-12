#ifndef LOOK_INTERNAL_H
#define LOOK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "theme/look.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "ui/css.h"

struct appearance {
    struct wsap_vtcore_theme palette;
    struct css_sheet chrome;
    char *name;
    int refs;
};

/*
 * Loads an appearance bundle containing CSS chrome styles and terminal palette.
 * Returns a pointer to the loaded appearance, or NULL on failure.
 */
struct appearance *appearance_load(const char *name, const char *css_path,
                                   const char *conf_path,
                                   const struct wsap_vtcore_theme *base);

/*
 * Increments the reference count of appearance `ap`.
 * Returns the appearance pointer.
 */
struct appearance *appearance_ref(struct appearance *ap);

/*
 * Decrements the reference count of appearance `ap` and frees it when zero.
 */
void appearance_unref(struct appearance *ap);

struct file_stamp {
    struct timespec mtime;
    off_t size;
    ino_t ino;
    dev_t dev;
};

struct theme_entry {
    struct look_theme info;
    struct appearance *built;
    struct file_stamp css_stamp, conf_stamp;
    uint32_t name_len;
    uint32_t label_len;
    char name_c0;
    char label_c0_lower;
};

struct theme_catalog {
    struct theme_entry *items;
    int count, cap;
    bool scanned;
};

/*
 * Scans theme directories and populates catalog `c`.
 */
void theme_catalog_scan(struct theme_catalog *c);

/*
 * Releases resources owned by theme catalog `c`.
 */
void theme_catalog_destroy(struct theme_catalog *c);

/*
 * Searches catalog `c` for a theme matching reference string `ref`.
 * Returns a pointer to the matching entry, or NULL if not found.
 */
struct theme_entry *theme_catalog_find(struct theme_catalog *c, const char *ref);

/*
 * Builds or retrieves the cached appearance for theme entry `e`.
 * Returns the appearance pointer, or NULL on error.
 */
struct appearance *theme_entry_appearance(struct theme_entry *e,
                                          const struct wsap_vtcore_theme *base);

#endif
