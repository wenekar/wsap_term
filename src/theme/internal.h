#ifndef LOOK_INTERNAL_H
#define LOOK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "theme/look.h"

struct app_identity;
#include "ui/css.h"

/* One key of a theme data block. */
struct theme_value {
    char *block;
    char *key;
    char *value;
};

/* The optional data blocks of a theme data file. The theme module stores
   them without a meaning. An application reads the block that it knows. */
struct theme_data {
    struct theme_value *items;
    int count;
    int cap;
};

struct appearance {
    struct css_sheet chrome;
    struct theme_data data;
    char *name;
    int refs;
};

/*
 * Loads an appearance with CSS chrome styles and a copy of `data`.
 * Returns a pointer to the loaded appearance, or NULL on failure.
 */
struct appearance *appearance_load(const struct app_identity *identity,
                                   const char *name, const char *css_path,
                                   const struct theme_data *data);

/*
 * Parses a theme data file. It fills the metadata of `meta` and stores the
 * data blocks in `data`. Returns false when the file cannot be read.
 */
bool theme_data_load(const char *path, struct theme_data *data,
                     struct look_theme *meta, const char *tag);

/*
 * Copies the items of `src` into `dst`. Returns false on allocation failure.
 */
bool theme_data_copy(struct theme_data *dst, const struct theme_data *src);

/*
 * Returns the value of `key` in the first block `block`, or NULL.
 */
const char *theme_data_value(const struct theme_data *data, const char *block,
                             const char *key);

/*
 * Releases the items of `data`.
 */
void theme_data_clear(struct theme_data *data);

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
    struct theme_data data;
    struct file_stamp css_stamp, data_stamp;
    uint32_t name_len;
    uint32_t label_len;
    char name_c0;
    char label_c0_lower;
};

struct theme_catalog {
    struct theme_entry *items;
    int count, cap;
    bool scanned;
    /* The app whose user and data theme directories the scan reads. */
    const struct app_identity *identity;
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
                                          const struct app_identity *identity);

#endif
