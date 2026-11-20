#ifndef LOOK_LOOK_H
#define LOOK_LOOK_H

#include <stdbool.h>

struct css_sheet;
struct app_identity;

/* The color scheme that a theme declares. The `polarity` key of the theme data
   file sets dark or light; an absent or unrecognized value leaves
   THEME_POLARITY_UNKNOWN. */
enum theme_polarity {
    THEME_POLARITY_UNKNOWN = 0,
    THEME_POLARITY_DARK,
    THEME_POLARITY_LIGHT,
};

struct look_theme {
    char *name;
    char *label;
    enum theme_polarity polarity;
    /* NULL when the catalog holds no CSS file. */
    char *css_path;
    /* The file that holds the metadata and optional data blocks. NULL when the
       catalog holds no data file. */
    char *data_path;
};

struct look;

/*
 * Creates a theme manager for `identity` and applies the initial `theme`.
 * `identity` supplies the theme directories and must stay valid until
 * look_destroy(). `theme` may be NULL, a CSS file path, or a catalog theme name
 * or label; a NULL, empty, or unknown name gives the manager an appearance
 * without theme rules. Returns NULL on allocation failure.
 */
struct look *look_create(const struct app_identity *identity,
                         const char *theme);

/*
 * Releases the appearances and catalog of theme manager `look`. Accepts NULL.
 */
void look_destroy(struct look *look);

/*
 * Sets the function called after the active appearance changes. `ud` is passed
 * to every call, and a NULL `changed` removes the listener. The callback runs
 * from look_preview(), look_cancel(), and look_apply() after the new appearance
 * is active. look_commit() keeps the active appearance and does not call it.
 */
void look_set_listener(struct look *look, void (*changed)(void *ud), void *ud);

/*
 * Returns the CSS sheet of the active appearance: the preview appearance when
 * one is active, otherwise the committed appearance. The sheet holds the theme
 * rules only. The caller layers it above its own toolkit and app sheet. The
 * pointer stays valid until the active appearance changes or `look` is
 * destroyed.
 */
const struct css_sheet *look_sheet(const struct look *look);

/*
 * Returns the value of `key` in data block `block` of the active appearance,
 * or NULL when `block` or `key` is NULL or has no value in that appearance.
 * Settings that an application reads with this call belong in the theme data
 * file. The value stays valid until the active appearance changes or `look` is
 * destroyed.
 */
const char *look_theme_value(const struct look *look, const char *block,
                             const char *key);

/*
 * Returns the number of themes in the catalog. Scans the theme directories
 * when no scan has run.
 */
int look_theme_count(struct look *look);

/*
 * Returns the metadata of the catalog theme at index `idx`, or NULL when `idx`
 * is out of range. The pointer stays valid until look_rescan() or
 * look_destroy().
 */
const struct look_theme *look_theme_at(struct look *look, int idx);

/*
 * Rescans the theme directories and replaces the catalog entries and cached
 * appearances, canceling any active preview. Returns the new theme count.
 * Pointers from look_theme_at() become invalid.
 */
int look_rescan(struct look *look);

/*
 * Activates the catalog theme at index `idx` as the preview appearance without
 * changing the committed theme, and notifies listeners. Returns false when
 * `idx` is out of range or the appearance fails to load. A repeated preview of
 * the same index returns true without notifying.
 */
bool look_preview(struct look *look, int idx);

/*
 * Returns the catalog index of the active preview theme, or -1 if none is
 * active.
 */
int look_previewed(const struct look *look);

/*
 * Commits the preview appearance as the committed appearance and clears the
 * preview. Does nothing when no preview is active. The active appearance does
 * not change, so no listener runs.
 */
void look_commit(struct look *look);

/*
 * Drops the preview reference and restores the committed appearance, then
 * notifies listeners. Does nothing when no preview is active.
 */
void look_cancel(struct look *look);

/*
 * Loads `theme` as the committed appearance, clears any preview, and notifies
 * listeners. `theme` may be NULL or empty for an appearance without theme
 * rules, a CSS file path, or a catalog theme name or label. On failure the
 * committed appearance does not change and no listener runs. On success the
 * cached appearance of every catalog entry is released. Returns true on
 * success.
 */
bool look_apply(struct look *look, const char *theme);

/*
 * Returns the name of the committed theme, or an empty string when the
 * appearance has no name. An appearance loaded from a CSS file path or from no
 * theme has no name. The pointer stays valid until the committed appearance
 * changes or `look` is destroyed.
 */
const char *look_committed_name(const struct look *look);

/*
 * Returns true when the catalog theme at index `idx` has the committed theme
 * name. Returns false when the committed theme is unnamed or `idx` is out of
 * range. The comparison is case-sensitive.
 */
bool look_is_committed(struct look *look, int idx);

#endif
