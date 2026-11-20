#ifndef LOOK_INTERNAL_H
#define LOOK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "theme/look.h"

struct app_identity;
#include "ui/css.h"

/* One key and value from a block of a theme data file. */
struct theme_value {
    char *block;
    char *key;
    char *value;
};

/* The items of the data blocks of a theme data file. The theme module stores
   them without interpreting them. An application reads the blocks that it
   knows. */
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
 * Loads an appearance with a copy of `data` and the CSS rules of `css_path`.
 * `identity` supplies the log prefix, and `name` becomes the theme name; both
 * may be NULL. `css_path` and `data` may be NULL. A CSS file that cannot be
 * read adds no rule, and a parse failure does not fail the load. Returns a new
 * reference that the caller releases with appearance_unref(), or NULL on
 * allocation failure.
 */
struct appearance *appearance_load(const struct app_identity *identity,
                                   const char *name, const char *css_path,
                                   const struct theme_data *data);

/*
 * Parses the theme data file at `path`, fills the metadata fields of `meta`,
 * and stores the data blocks in `data`. A stored value with the same block and
 * key is replaced. `tag` is the log prefix for warnings. A malformed line
 * produces a warning and does not fail the load. Returns false when the file
 * cannot be read.
 */
bool theme_data_load(const char *path, struct theme_data *data,
                     struct look_theme *meta, const char *tag);

/*
 * Copies the items of `src` into `dst` and replaces an item with the same
 * block and key. A NULL `src` adds nothing. Returns false on allocation
 * failure; `dst` then holds the items copied so far.
 */
bool theme_data_copy(struct theme_data *dst, const struct theme_data *src);

/*
 * Returns the borrowed value of `key` in block `block`, or NULL when `data`,
 * `block`, or `key` is NULL or the block or key is absent. The value stays
 * valid until the next write to `data` or theme_data_clear().
 */
const char *theme_data_value(const struct theme_data *data, const char *block,
                             const char *key);

/*
 * Releases the items of `data` and resets it to an empty state. Accepts NULL.
 */
void theme_data_clear(struct theme_data *data);

/*
 * Increments the reference count of appearance `ap` and returns `ap`. A NULL
 * `ap` returns NULL.
 */
struct appearance *appearance_ref(struct appearance *ap);

/*
 * Decrements the reference count of appearance `ap` and frees it when the
 * count reaches zero. Accepts NULL.
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
    /* The app whose user and data theme directories the scan reads. It must
       stay valid for the life of the catalog and may be NULL to skip those
       directories. */
    const struct app_identity *identity;
};

/*
 * Scans the user, development, application bundle, and data theme directories
 * and replaces the entries of catalog `c`. The first CSS file and the first
 * data file found for a name are kept, so the files of one theme can come from
 * different directories. A cached appearance stays when the entry name and
 * paths do not change. `c->identity` must be set before the first scan to
 * include the user and data directories.
 */
void theme_catalog_scan(struct theme_catalog *c);

/*
 * Releases the entries and cached appearances of catalog `c` and resets it to
 * an empty state.
 */
void theme_catalog_destroy(struct theme_catalog *c);

/*
 * Returns the entry of catalog `c` whose name equals `ref`, or whose label
 * equals `ref` when case is ignored. Scans the catalog when no scan has run.
 * Returns NULL when no entry matches. The entry stays valid until the next scan
 * or theme_catalog_destroy().
 */
struct theme_entry *theme_catalog_find(struct theme_catalog *c, const char *ref);

/*
 * Returns the cached appearance of entry `e`, or builds it from the entry name,
 * CSS file, and data when no appearance is cached or a file changed since the
 * build. A failed load keeps the cached appearance. The result is borrowed and
 * stays valid until the entry is rebuilt, rescanned, or destroyed.
 */
struct appearance *theme_entry_appearance(struct theme_entry *e,
                                          const struct app_identity *identity);

#endif
